#!/usr/bin/env python3
"""Aggregate PUBLIC-ONLY paired game AI evaluation results.

A cluster is one identical physical 54-card shuffle, evaluated in the three
landlord-seat rotations with BOTH candidate-vs-baseline team assignments.
Never treat the six matches as statistically independent observations.
No hidden hands, replay rows or other player secrets appear in this report.
"""
from __future__ import annotations
import argparse
import json
import math
from pathlib import Path
import random
import re

ROW = re.compile(
    r"^MYGAMES_PAIRED_V1 seed_index=(\d+) landlord_rotations=(\d+) "
    r"games=(\d+) win_sign_sum=(-?\d+) game_points_sum=(-?\d+)$"
)
TOTAL = re.compile(
    r"^MYGAMES_PAIRED_TOTAL_V1 clusters=(\d+) matches=(\d+) "
    r"win_sign_sum=(-?\d+) game_points_sum=(-?\d+)$"
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1)))]


def bootstrap_clusters(values: list[float], n_replicates: int = 4000) -> list[float]:
    if not values or n_replicates <= 0:
        raise ValueError("need nonempty independent deal clusters")
    rng = random.Random(0x51ca1e)
    n = len(values)
    samples = []
    for _ in range(n_replicates):
        samples.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    return [percentile(samples, 0.025), percentile(samples, 0.975)]


def parse_aggregate(text: str) -> list[dict]:
    clusters: list[dict] = []
    total = None
    for raw in text.splitlines():
        if not raw.strip():
            continue
        line = raw.strip()
        if match := ROW.fullmatch(line):
            seed_index, rotations, games, wins, points = map(int, match.groups())
            if rotations != 3 or games != 6 or seed_index != len(clusters):
                raise ValueError("paired seat-rotation / seed cluster not complete")
            if wins % 2 != 0 or abs(wins) > games:
                raise ValueError("invalid signed win total for matched pairs")
            if not math.isfinite(float(points)):
                raise ValueError("nonfinite signed point total")
            clusters.append({
                "cluster_id": seed_index,
                "games": games,
                "win_sign_sum": wins,
                "game_points_sum": points,
            })
        elif match := TOTAL.fullmatch(line):
            if total is not None:
                raise ValueError("duplicate aggregate footer")
            total = tuple(map(int, match.groups()))
        else:
            raise ValueError("unknown or private record in public stats: " +
                             line[:32])
    if not clusters or total is None:
        raise ValueError("incomplete paired evaluation")
    if (total != (
        len(clusters), 6 * len(clusters),
        sum(r["win_sign_sum"] for r in clusters),
        sum(r["game_points_sum"] for r in clusters)
    )):
        raise ValueError("aggregate footer does not reconcile with clusters")
    return clusters


def summarize(clusters: list[dict]) -> dict:
    n = len(clusters)
    if n == 0:
        raise ValueError("no paired episodes")
    wins = [r["win_sign_sum"] / r["games"] for r in clusters]
    scores = [r["game_points_sum"] / r["games"] for r in clusters]
    # Operational threshold: require at least 30 independent deals to
    # calculate *exploratory* cluster-bootstrap intervals. Even 30 is NOT
    # a guarantee of model superiority or generalization.
    sufficient = n >= 30
    return {
        "schema": "mygames-dmc-paired-results-v1",
        "independent_deal_clusters": n,
        "matched_games": sum(r["games"] for r in clusters),
        "candidate_mean_signed_win": sum(wins) / n,
        "candidate_mean_nonmonetary_points": sum(scores) / n,
        "confidence": {
            "resampling_unit": "independent 54-card shuffle, NOT a game/seat",
            "threshold_met": sufficient,
            "bootstrap_95_win_sign": bootstrap_clusters(wins) if sufficient else None,
            "bootstrap_95_points": bootstrap_clusters(scores) if sufficient else None,
        },
        "qualified_as_trained_AI_evaluation": False,
        "policy_comparison": "untrained shed-most vs deterministic legal-index baseline"
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="PUBLIC aggregate only")
    args = parser.parse_args()
    path = Path(args.input)
    if not path.is_file() or path.stat().st_size > 256 * 1024:
        raise ValueError("public aggregate missing or too large")
    report = summarize(parse_aggregate(path.read_text(encoding="utf-8")))
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
