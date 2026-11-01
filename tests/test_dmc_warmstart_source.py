"""Torch-free negative/lineage acceptance of opt-in frozen-Q warm-start.

FAKE model files are used here to test private-file/provenance mechanics.
This test does NOT claim actual PyTorch optimizer or native C11 game ability.
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
from warmstart_source import admit_warmstart_source, lineage_path, publish_lineage


def rejected(callback):
    try:
        callback()
    except (ValueError, PermissionError, FileExistsError, FileNotFoundError):
        return
    raise AssertionError("invalid or unproven warm-start source was admitted")


def main():
    with tempfile.TemporaryDirectory(prefix="mygames-r9-warm-") as tmp:
        base = Path(tmp)
        parent = base / "frozen-source-v102.pt"
        candidate = base / "warm-candidate-v104.pt"
        parent.write_bytes(b"NOT A TORCH CHECKPOINT, PRIVATE CONTRACT FIXTURE")
        candidate.write_bytes(b"NOT A WARM-TRAINED MODEL, PRIVATE CONTRACT FIXTURE")
        parent.chmod(0o600)
        candidate.chmod(0o600)
        parent_sha = hashlib.sha256(parent.read_bytes()).hexdigest()
        source = {
            "parent_checkpoint_sha256": parent_sha,
            "parent_model_version": 102,
            "onpolicy_source_manifest_bundle_sha256": "f" * 64,
            "source_revision_claim": "a" * 40,
            "native_library_sha256": "b" * 64,
            "train_count": 96,
            "validation_count": 48,
        }
        empty_train = base / "train"
        empty_validation = base / "validation"
        empty_train.mkdir(mode=0o700)
        empty_validation.mkdir(mode=0o700)
        rejected(lambda: admit_warmstart_source(
            parent_checkpoint=parent, parent_model_version=102,
            native_library_sha256="b" * 64, source_revision="a" * 40,
            train_root=empty_train, validation_root=empty_validation))
        rejected(lambda: admit_warmstart_source(
            parent_checkpoint=parent, parent_model_version=102,
            native_library_sha256="b" * 64, source_revision="a" * 40,
            train_root=empty_train, validation_root=empty_train))

        names = ("landlord", "landlord_up", "landlord_down")
        metrics = {name: {"mse": 0.8, "zero_q_mse": 1.0}
                   for name in names}
        report = {
            "selected_validation_step_by_role": {
                "landlord": 40, "landlord_up": 60, "landlord_down": 80},
            "validation_aligned_after": metrics,
        }
        rejected(lambda: publish_lineage(candidate, parent, source, 102, report))
        rejected(lambda: publish_lineage(candidate, parent, source, 101, report))
        rejected(lambda: publish_lineage(
            candidate, parent, {**source, "parent_checkpoint_sha256": "e" * 64},
            104, report))
        assert not lineage_path(candidate).exists()
        bad = {**report, "validation_aligned_after": {
            **metrics, "landlord": {"mse": 1.0, "zero_q_mse": 1.0}}}
        rejected(lambda: publish_lineage(candidate, parent, source, 104, bad))
        assert not lineage_path(candidate).exists()

        result = publish_lineage(candidate, parent, source, 104, report)
        assert result["warmstart_parent_model_version"] == 102
        assert result["candidate_model_version"] == 104
        assert result["gameplay_skill_qualified"] is False
        assert result["virgin_test_opened"] is False
        marker = lineage_path(candidate)
        assert marker.is_file() and stat.S_IMODE(marker.stat().st_mode) & 0o077 == 0
        data = json.loads(marker.read_text())
        assert data["candidate_checkpoint_sha256"] == hashlib.sha256(
            candidate.read_bytes()).hexdigest()
        assert data["parent_checkpoint_sha256"] == parent_sha
        assert data["onpolicy_source_manifest_bundle_sha256"] == "f" * 64
        rejected(lambda: publish_lineage(candidate, parent, source, 104, report))
        assert not any(p.name.startswith(".dmc-warmstart-") for p in base.iterdir())
    print("mygames.dmc_private_warmstart_lineage_contract: PASS")


if __name__ == "__main__":
    main()
