"""Validate native paired A/B results and cluster-safe uncertainty policy."""
from __future__ import annotations
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from ai.dmc.paired_stats import parse_aggregate, summarize


def must_reject(body: str) -> None:
    try:
        parse_aggregate(body)
    except ValueError:
        return
    raise AssertionError("invalid public statistical result was admitted")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_dmc_paired_stats.py <public aggregate>")
    raw = Path(sys.argv[1]).read_text(encoding="utf-8")
    rows = parse_aggregate(raw)
    summary = summarize(rows)
    assert summary["matched_games"] == 6 * len(rows) == 12
    assert summary["independent_deal_clusters"] == 2
    assert not summary["confidence"]["threshold_met"]
    assert summary["confidence"]["bootstrap_95_win_sign"] is None
    assert summary["qualified_as_trained_AI_evaluation"] is False
    assert -1.0 <= summary["candidate_mean_signed_win"] <= 1.0
    # Tamper the aggregate footer and reject; no silent repair or guess.
    must_reject(raw.replace("MYGAMES_PAIRED_TOTAL_V1 clusters=2",
                            "MYGAMES_PAIRED_TOTAL_V1 clusters=3"))
    must_reject(raw.replace("landlord_rotations=3", "landlord_rotations=2"))
    must_reject(raw + "PRIVATE_CARD_MASK=123\n")
    # A 30 independent-deal synthetic all-zero null collection has exactly
    # zero cluster-bootstrap interval in both metrics.
    null_rows = [{
        "cluster_id": i, "games": 6, "win_sign_sum": 0,
        "game_points_sum": 0
    } for i in range(30)]
    result = summarize(null_rows)
    assert result["confidence"]["threshold_met"] is True
    assert result["confidence"]["bootstrap_95_win_sign"] == [0.0, 0.0]
    assert result["confidence"]["bootstrap_95_points"] == [0.0, 0.0]
    check = subprocess.run(
        [sys.executable, "ai/dmc/paired_stats.py", "--input", sys.argv[1]],
        cwd=ROOT, capture_output=True, text=True, check=True
    )
    report = json.loads(check.stdout)
    assert report == summary
    print("mygames.dmc_paired_cluster_stats: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
