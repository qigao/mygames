"""Stdlib-only episode/source/outcome optimizer policy contract.

Fixtures are SYNTHETIC for sampler invariants; the separate C11 Replay-v1
test continues to verify true authoritative episodes. No Torch/NumPy in CI.
"""
from __future__ import annotations

from collections import Counter
from pathlib import Path
import random
import sys

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "ai" / "dmc"))

from replay import Episode, Sample
from train_policy import (make_role_sampler, require_validation_advantage,
                          paired_complete_game_mse)


def episode(ep_id, source, landlord_wins, length):
    steps = []
    for role in range(3):
        target = 1.0 if landlord_wins == (role == 0) else -1.0
        for idx in range(length):
            steps.append(Sample(
                idx + 1, ep_id, role, role,
                bytes(319 if role == 0 else 430), bytes(810), bytes(54), target))
    return Episode(ep_id, source, 1, 1, 0,
                   0 if landlord_wins else 1, tuple(steps))


def require_rejected(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError("invalid DMC sampling/validation was accepted")


def main():
    episodes = [
        episode(source * 100 + (10 if landlord_wins else 0) + length,
                source, landlord_wins, length)
        for source in (1, 3)
        for landlord_wins in (False, True)
        for length in (1, 20)
    ]
    for role in range(3):
        sampler = make_role_sampler(episodes, role)
        assert sampler.strata_counts() == {
            "1": {"positive_games": 2, "negative_games": 2},
            "3": {"positive_games": 2, "negative_games": 2}}
        a = sampler.batch(random.Random(43), 4096)
        b = sampler.batch(random.Random(43), 4096)
        assert [(r.chosen_cards, r.role) for r in a] == [
            (r.chosen_cards, r.role) for r in b]
        sources = Counter(r.chosen_cards // 100 for r in a)
        wins = Counter(r.target for r in a)
        lengths = Counter(r.chosen_cards % 10 for r in a)
        for groups, values in (
            (sources, (1, 3)), (wins, (-1.0, 1.0)), (lengths, (0, 1))
        ):
            assert all(1800 <= groups[value] <= 2300 for value in values)
        for size in (0, -1, 65537):
            require_rejected(lambda: sampler.batch(random.Random(7), size))
        require_rejected(lambda: make_role_sampler(episodes[:1], role))
        require_rejected(lambda: make_role_sampler(episodes + [episodes[0]], role))
        # Metric is a source/sign/game average, NOT a transition average.
        # One long bad game outweighs many short games under the old metric;
        # the new metric deliberately gives both games equal per-stratum mass.
        measurements = {}
        for ep in episodes:
            role_rows = [s for s in ep.samples if s.role == role]
            adverse_long = (ep.model_version == 1 and ep.winner_seat == 0
                            and len(role_rows) == 20)
            measurements[ep.episode_id] = {
                "count": len(role_rows),
                "mse": 6.0 if adverse_long else 0.0,
                "mae": 1.0 if adverse_long else 0.0,
                "zero_q_mse": 1.0,
            }
        aggregate = sampler.aggregate_validation(measurements)
        assert aggregate["weighting"] == "episode-source-outcome"
        assert aggregate["count"] == 84 and aggregate["episodes"] == 8
        assert abs(aggregate["mse"] - 0.75) < 1e-12
        assert abs(aggregate["mae"] - 0.125) < 1e-12
        assert aggregate["zero_q_mse"] == 1.0
        old_transition_mse = sum(v["mse"] * v["count"]
                                 for v in measurements.values()) / 84
        assert old_transition_mse > 1.0 > aggregate["mse"]
        for broken in (
            {k: v for k, v in measurements.items() if k != episodes[0].episode_id},
            {**measurements, 999999: measurements[episodes[0].episode_id]},
            {**measurements, episodes[0].episode_id: {
                **measurements[episodes[0].episode_id], "count": 999}},
            {**measurements, episodes[0].episode_id: {
                **measurements[episodes[0].episode_id], "mse": float("nan")}},
            {**measurements, episodes[0].episode_id: {
                **measurements[episodes[0].episode_id], "zero_q_mse": float("inf")}},
        ):
            require_rejected(lambda bad=broken: sampler.aggregate_validation(bad))

    valid = {role: {"mse": mse, "zero_q_mse": 1.0}
             for role, mse in (("landlord", 0.5), ("landlord_up", 0.8),
                               ("landlord_down", 0.95))}
    require_validation_advantage(valid)
    # The aligned score admits an otherwise sound candidate that the long-
    # episode-biased transition MSE would reject. Gate operates on its input
    # metric; the learner selects the aligned metric in stratified mode.
    aligned_gate = {role: {"mse": 0.75, "zero_q_mse": 1.0} for role in valid}
    require_validation_advantage(aligned_gate)
    require_rejected(lambda: require_validation_advantage({
        role: {"mse": 120.0 / 84.0, "zero_q_mse": 1.0} for role in valid
    }))
    for unacceptable in (1.0, 1.00001, float("nan")):
        invalid = {k: dict(v) for k, v in valid.items()}
        invalid["landlord_down"]["mse"] = unacceptable
        require_rejected(lambda: require_validation_advantage(invalid))
    # Synthetic comparison arithmetic only, NOT real trained DDZ skill.
    # Short and long games must contribute equally to cluster inference.
    complete_games = [
        {"episode_id": i + 1, "roles": {
            role: {"count": 20 if i == 0 else 1,
                   "a_mse": 12.0 if i == 0 else 0.0,
                   "b_mse": 1.0, "zero_q_mse": 1.0}
            for role in ("landlord", "landlord_up", "landlord_down")}}
        for i in range(30)
    ]
    score = paired_complete_game_mse(complete_games)
    assert score["independent_complete_games"] == 30
    assert score["resampling_unit"].startswith("whole sealed game")
    assert score["exploratory_interval_available"]
    assert not score["trained_gameplay_skill_qualified"]
    for role in ("landlord", "landlord_up", "landlord_down"):
        row = score["roles"][role]
        assert abs(row["a_equal_game_mse"] - 0.4) < 1e-12
        assert row["b_equal_game_mse"] == row["zero_q_equal_game_mse"] == 1.0
        assert abs(row["b_reduction_equal_game_mse"] + 0.6) < 1e-12
        assert row["a_transition_mse"] > row["b_transition_mse"]
        assert len(row["bootstrap_95_b_reduction"]) == 2
    assert paired_complete_game_mse(complete_games) == score
    small = paired_complete_game_mse(complete_games[:29])
    assert small["exploratory_interval_available"] is False
    assert small["roles"]["landlord"]["bootstrap_95_b_reduction"] is None
    for invalid in (
        complete_games + [complete_games[0]],
        [{**complete_games[0], "episode_id": 0}] + complete_games[1:],
        [{**complete_games[0], "roles": {"landlord": complete_games[0]["roles"]["landlord"]}}]
        + complete_games[1:],
        [{**complete_games[0], "roles": {
            **complete_games[0]["roles"], "landlord": {
                **complete_games[0]["roles"]["landlord"], "a_mse": float("nan")}}}]
        + complete_games[1:],
        [{**complete_games[0], "roles": {
            **complete_games[0]["roles"], "landlord": {
                **complete_games[0]["roles"]["landlord"], "count": 0}}}]
        + complete_games[1:],
    ):
        require_rejected(lambda bad=invalid: paired_complete_game_mse(bad))
    print("mygames.dmc_episode_source_outcome_sampling: PASS")


if __name__ == "__main__":
    main()
