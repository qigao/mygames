"""Pure stdlib complete-game DMC stratified sampler and validation gate.

The C11 rules and sealed Replay remain the only source of legal moves,
per-role observations, and terminal rewards; this changes only sampling.
With explicitly verified behavior-source manifests, a batch draws uniformly
across source model version, outcome sign, whole game, and acting-role move.
It does not authenticate a model ID, source binary, or private checkpoint.
"""
from __future__ import annotations

import random
from collections import defaultdict
from math import isfinite
from typing import Sequence

from replay import Episode, Sample


class EpisodeRoleSampler:
    def __init__(self, sources, groups):
        self._sources = sources
        self._groups = groups

    def batch(self, rng: random.Random, size: int) -> list[Sample]:
        if size <= 0 or size > 65536:
            raise ValueError("stratified DMC optimizer batch out of bounds")
        samples = []
        for _ in range(size):
            source = self._sources[rng.randrange(len(self._sources))]
            sign = (-1, 1)[rng.randrange(2)]
            episodes = self._groups[source][sign]
            _, role_rows = episodes[rng.randrange(len(episodes))]
            samples.append(role_rows[rng.randrange(len(role_rows))])
        return samples

    def strata_counts(self) -> dict[str, dict[str, int]]:
        return {str(source): {
            "positive_games": len(self._groups[source][1]),
            "negative_games": len(self._groups[source][-1]),
        } for source in self._sources}

    def aggregate_validation(self, per_episode: dict[int, dict]) -> dict:
        """Average errors with EXACTLY the source/sign/game/move batch weights.

        Inputs are per-role, per-complete-episode means, never individual
        transitions. All registered games/roles must have finite measurements.
        This is a tuning metric, not an independent gameplay-skill estimate.
        """
        if not isinstance(per_episode, dict):
            raise ValueError("validation requires per-episode measurements")
        keys = ("mse", "mae", "zero_q_mse")
        expected_ids = set()
        source_means = []
        total_decisions = 0
        for source in self._sources:
            sign_means = []
            for sign in (-1, 1):
                games = self._groups[source][sign]
                cell_totals = {key: 0.0 for key in keys}
                for episode_id, role_rows in games:
                    if episode_id in expected_ids:
                        raise ValueError("duplicate validation episode ID")
                    expected_ids.add(episode_id)
                    entry = per_episode.get(episode_id)
                    if not isinstance(entry, dict) or type(entry.get("count")) is not int or \
                            entry["count"] != len(role_rows):
                        raise ValueError("incomplete per-game validation count")
                    total_decisions += entry["count"]
                    for key in keys:
                        value = entry.get(key)
                        if type(value) not in (int, float) or \
                                not isfinite(value) or value < 0:
                            raise ValueError("nonfinite/invalid per-game validation " + key)
                        cell_totals[key] += value
                sign_means.append({key: cell_totals[key] / len(games)
                                   for key in keys})
            source_means.append({key: sum(cell[key] for cell in sign_means) / 2.0
                                 for key in keys})
        if set(per_episode) != expected_ids:
            raise ValueError("missing/unexpected validation episode ID")
        result = {key: sum(cell[key] for cell in source_means) /
                  len(source_means) for key in keys}
        if not all(isfinite(value) for value in result.values()):
            raise ValueError("nonfinite balanced validation aggregation")
        return {"count": total_decisions, "episodes": len(expected_ids),
                "weighting": "episode-source-outcome", **result}


def make_role_sampler(episodes: Sequence[Episode], role: int) -> EpisodeRoleSampler:
    """Admit only complete games with both win/loss classes per role/source."""
    if role not in (0, 1, 2) or not episodes:
        raise ValueError("missing episodes or invalid DMC role")
    groups = defaultdict(lambda: {1: [], -1: []})
    seen = set()
    for ep in episodes:
        if ep.episode_id in seen:
            raise ValueError("duplicate complete-game Episode ID")
        seen.add(ep.episode_id)
        if ep.model_version <= 0:
            raise ValueError("invalid behavior/source model version")
        actor_rows = tuple(s for s in ep.samples if s.role == role)
        if not actor_rows:
            raise ValueError("complete episode missing required acting role")
        signs = {1 if s.target > 0 else -1 if s.target < 0 else 0
                 for s in actor_rows}
        if len(signs) != 1 or 0 in signs:
            raise ValueError("inconsistent role reward within complete game")
        sign = next(iter(signs))
        groups[ep.model_version][sign].append((ep.episode_id, actor_rows))
    for source, outcomes in groups.items():
        if not outcomes[1] or not outcomes[-1]:
            raise ValueError(
                f"source {source} role {role} lacks winning/losing complete episodes")
    return EpisodeRoleSampler(tuple(sorted(groups)), groups)


def require_validation_advantage(metrics: dict) -> None:
    """Never publish an explicitly gated candidate worse than zero-Q."""
    for role in ("landlord", "landlord_up", "landlord_down"):
        stats = metrics.get(role)
        if not isinstance(stats, dict):
            raise ValueError("incomplete role validation metrics")
        mse, baseline = stats.get("mse"), stats.get("zero_q_mse")
        if (type(mse) not in (int, float) or
                type(baseline) not in (int, float) or
                not isfinite(mse) or not isfinite(baseline) or
                baseline <= 0 or mse >= baseline):
            raise ValueError(
                f"validation zero-Q baseline gate failed for {role}; no checkpoint published")
