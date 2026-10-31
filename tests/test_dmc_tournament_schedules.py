"""Torch-free admission for paired-deal development/holdout split."""
from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))
from tournament import schedule_seeds, bootstrap


def main():
    a = schedule_seeds(30, "development")
    b = schedule_seeds(30, "holdout")
    assert len(a) == len(set(a)) == len(b) == len(set(b)) == 30
    assert set(a).isdisjoint(b)
    assert schedule_seeds(30, "development") == a
    assert schedule_seeds(30, "holdout") == b
    for count, label in ((0, "holdout"), (257, "development"),
                         (30, "unknown")):
        try:
            schedule_seeds(count, label)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted invalid paired-deal schedule")
    assert bootstrap([0.0] * 30) == [0.0, 0.0]
    assert bootstrap([1.0] * 30) == [1.0, 1.0]
    print("mygames.dmc_pair_holdout_schedule: PASS")


if __name__ == "__main__":
    main()
