"""Strict private frozen model SOURCE evidence; filesystem contract only.

The test checkpoint is deliberately NOT a neural model; this test proves
no-clobber, digest-bound source metadata, not PyTorch optimization or skill.
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
from model_source_record import (
    make_record, publish_record, public_training_evidence,
    record_path, verify_record,
)


def reject(callback):
    try:
        callback()
    except (ValueError, PermissionError, FileExistsError):
        return
    raise AssertionError("invalid model/source provenance admitted")


def main():
    with tempfile.TemporaryDirectory(prefix="mygames-dmc-source-") as name:
        parent = Path(name)
        model = parent / "frozen-private-v3.pt"
        model.write_bytes(b"NOT REAL TORCH WEIGHTS, SOURCE FILE CONTRACT ONLY")
        model.chmod(0o600)
        digest = hashlib.sha256(model.read_bytes()).hexdigest()
        labels = ["mixed-v1", "shed-most", "legal-index"]
        metrics = {r: {"mse": 0.6, "zero_q_mse": 1.0}
                   for r in ("landlord", "landlord_up", "landlord_down")}
        steps = dict(zip(metrics, (20, 40, 60)))
        params = {
            "checkpoint_sha256": digest, "model_version": 4,
            "source_revision": "a" * 40, "learner_revision": "b" * 40,
            "generator_sha256": "c" * 64, "source_bundle_sha256": "d" * 64,
            "sources": labels, "train_count": 72, "tuning_count": 72,
            "steps_by_role": steps, "aligned_by_role": metrics,
        }
        bad = make_record(**params)
        bad["candidate_model_version"] = True
        reject(lambda: publish_record(model, bad))
        assert not record_path(model).exists()

        bad = make_record(**params)
        bad["validation_aligned_mse_by_role"]["landlord_up"]["mse"] = float("nan")
        reject(lambda: publish_record(model, bad))
        assert not record_path(model).exists()

        bad = make_record(**params)
        bad["validation_aligned_mse_by_role"]["landlord"]["mse"] = 1.0
        reject(lambda: publish_record(model, bad))
        assert not record_path(model).exists()

        good = make_record(**params)
        evidence = public_training_evidence(good)
        assert evidence["schema"] == "mygames-r9-public-training-evidence-v1"
        assert evidence["source_policy_labels"] == sorted(labels)
        assert evidence["all_tuning_roles_better_than_zero_q"]
        assert evidence["unseen_test_measured"] is False
        assert evidence["gameplay_skill_qualified"] is False
        assert evidence["production_bot_promoted"] is False
        for role in metrics:
            assert evidence["roles"][role] == {
                "selected_optimizer_step": steps[role],
                "tuning_aligned_q_mse": 0.6,
                "tuning_zero_q_mse": 1.0,
                "tuning_q_mse_reduction": 0.4,
            }
        assert "episode_id" not in str(evidence)
        assert "private" not in str(evidence)
        result = publish_record(model, good)
        assert result["source_record_verified"]
        assert result["qualified_as_production_bot"] is False
        marker = record_path(model)
        assert stat.S_IMODE(marker.stat().st_mode) & 0o077 == 0
        assert verify_record(model, expected_model_version=4,
                             expected_bundle_sha256="d" * 64) == good
        reject(lambda: publish_record(model, good))
        reject(lambda: verify_record(model, expected_model_version=5))
        reject(lambda: verify_record(
            model, expected_model_version=4, expected_bundle_sha256="e" * 64))

        original = marker.read_bytes()
        try:
            tampered = json.loads(original)
            tampered["verified_behavior_sources"] = ["mixed-v1", "mixed-v1"]
            marker.write_text(json.dumps(tampered))
            reject(lambda: verify_record(model, expected_model_version=4))
        finally:
            marker.write_bytes(original)
        try:
            model.write_bytes(b"CORRUPTED CHECKPOINT")
            reject(lambda: verify_record(model, expected_model_version=4))
        finally:
            model.write_bytes(b"NOT REAL TORCH WEIGHTS, SOURCE FILE CONTRACT ONLY")
        assert verify_record(model, expected_model_version=4) == good
    print("mygames.dmc_private_frozen_source_record: PASS")


if __name__ == "__main__":
    main()
