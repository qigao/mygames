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
from train_policy import make_role_sampler, require_validation_advantage


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

    valid = {role: {"mse": mse, "zero_q_mse": 1.0}
             for role, mse in (("landlord", 0.5), ("landlord_up", 0.8),
                               ("landlord_down", 0.95))}
    require_validation_advantage(valid)
    for unacceptable in (1.0, 1.00001, float("nan")):
        invalid = {k: dict(v) for k, v in valid.items()}
        invalid["landlord_down"]["mse"] = unacceptable
        require_rejected(lambda: require_validation_advantage(invalid))
    print("mygames.dmc_episode_source_outcome_sampling: PASS")


if __name__ == "__main__":
    main()
