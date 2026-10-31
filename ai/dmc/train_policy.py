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
            role_rows = episodes[rng.randrange(len(episodes))]
            samples.append(role_rows[rng.randrange(len(role_rows))])
        return samples

    def strata_counts(self) -> dict[str, dict[str, int]]:
        return {str(source): {
            "positive_games": len(self._groups[source][1]),
            "negative_games": len(self._groups[source][-1]),
        } for source in self._sources}


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
        groups[ep.model_version][sign].append(actor_rows)
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
