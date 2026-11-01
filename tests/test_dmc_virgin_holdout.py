"""Torch-free R9c one-shot TEST lock and frozen artifact admission tests.

Deliberately FAKE checkpoint/native-library bytes. Only filesystem protocol,
model identity and confidence arithmetic, NOT game skill, are tested here.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import stat
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))
from model_source_record import make_record, publish_record
from virgin_holdout import (PLAN, LOCK, RESULT, OLD_DEALS, admit_and_lock,
                            prepare, qualification, read_plan,
                            validate_fresh_deal_seeds)


def reject(fn):
    try:
        fn()
    except (ValueError, PermissionError, FileExistsError):
        return
    raise AssertionError("invalid/reused private R9 TEST was accepted")


def main():
    with tempfile.TemporaryDirectory(prefix="mygames-r9-holdout-") as tmp:
        private = Path(tmp)
        model = private / "frozen.pt"
        model.write_bytes(b"FAKE TORCH CHECKPOINT - NOT AN ACTUAL TRAINED MODEL")
        model.chmod(0o600)
        native = private / "native_test_only.so"
        native.write_bytes(b"FAKE C11 SHARED LIBRARY - NO INFERENCE")
        native.chmod(0o600)
        source_record = make_record(
            checkpoint_sha256=hashlib.sha256(model.read_bytes()).hexdigest(),
            model_version=31, source_revision="a" * 40,
            learner_revision="b" * 40, generator_sha256="c" * 64,
            source_bundle_sha256="d" * 64,
            sources=("shed-most", "legal-index"),
            train_count=96, tuning_count=48,
            steps_by_role={r: 20 for r in (
                "landlord", "landlord_up", "landlord_down")},
            aligned_by_role={r: {"mse": 0.9, "zero_q_mse": 1.0} for r in (
                "landlord", "landlord_up", "landlord_down")},
        )
        publish_record(model, source_record)
        holdout = private / "holdout"
        holdout.mkdir(mode=0o700)
        reject(lambda: prepare(root=holdout, checkpoint=model,
                               native_library=native, model_version=32,
                               seed_clusters=30, identity_clusters=2))
        assert list(holdout.iterdir()) == []
        prepared = prepare(root=holdout, checkpoint=model,
                           native_library=native, model_version=31,
                           seed_clusters=30, identity_clusters=2)
        assert prepared["tested_game_skill"] is False
        assert prepared["production_bot_promoted"] is False
        plan, digest = read_plan(holdout)
        assert len(plan["seeds"]) == 30
        assert set(plan["seeds"]).isdisjoint(OLD_DEALS)
        assert stat.S_IMODE((holdout / PLAN).stat().st_mode) & 0o077 == 0
        assert not (holdout / LOCK).exists()
        reject(lambda: prepare(root=holdout, checkpoint=model,
                               native_library=native, model_version=31,
                               seed_clusters=30, identity_clusters=2))
        reject(lambda: validate_fresh_deal_seeds([1] * 30, 30))
        reject(lambda: validate_fresh_deal_seeds(
            list(plan["seeds"][:-1]) + [next(iter(OLD_DEALS))], 30))
        reject(lambda: validate_fresh_deal_seeds(
            list(plan["seeds"][:-1]) + [True], 30))
        original = native.read_bytes()
        try:
            native.write_bytes(b"TAMPERED NATIVE LIBRARY")
            reject(lambda: admit_and_lock(root=holdout, checkpoint=model,
                                          native_library=native, model_version=31))
            assert not (holdout / LOCK).exists()
        finally:
            native.write_bytes(original)
        p, sha = admit_and_lock(root=holdout, checkpoint=model,
                               native_library=native, model_version=31)
        assert p == plan and sha == digest
        assert (holdout / LOCK).is_file()
        assert not (holdout / RESULT).exists()
        reject(lambda: admit_and_lock(root=holdout, checkpoint=model,
                                      native_library=native, model_version=31))
        example = {
            "baseline": "shed-most",
            "test_seed_schedule": "sealed-r9-frozen-model-one-shot",
            "score_failures": 0,
            "independent_shuffle_clusters": 30,
            "paired_complete_matches": 180,
            "identity_null_completed_matches": 12,
            "cluster_bootstrap_95_win_rate": [0.55, 0.72],
            "cluster_bootstrap_95_points": [0.1, 1.3],
        }
        score = qualification(example, expected_clusters=30,
                              identity_clusters=2)
        assert score["frozen_model_skill_gate_passed"]
        assert score["production_bot_promoted"] is False
        weaker = dict(example)
        weaker["cluster_bootstrap_95_win_rate"] = [0.5, 0.67]
        assert not qualification(weaker, expected_clusters=30,
                                 identity_clusters=2)["frozen_model_skill_gate_passed"]
        reject(lambda: qualification({**example, "score_failures": 1},
                                     expected_clusters=30, identity_clusters=2))
        reject(lambda: qualification({**example, "paired_complete_matches": 179},
                                     expected_clusters=30, identity_clusters=2))
        reject(lambda: qualification({**example,
                                      "cluster_bootstrap_95_points": [float("nan"), 1.3]},
                                     expected_clusters=30, identity_clusters=2))
    print("mygames.dmc_virgin_r9_holdout_one_shot_contract: PASS")


if __name__ == "__main__":
    main()
