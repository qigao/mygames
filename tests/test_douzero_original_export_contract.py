#!/usr/bin/env python3
"""Torch/ONNX-free negative admission of original DouZero model conversion.

NO fake checkpoint may pass for model publication: these intentionally
invalid fixture bytes never reach torch.load or create an ONNX artifact.
This source test is NOT trained model numerical parity.
"""
from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "ai/douzero/export_original_wp_onnx.py"
spec = importlib.util.spec_from_file_location("pinned_douzero_exporter_test", SCRIPT)
assert spec and spec.loader
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


def rejected(f):
    try:
        f()
    except (ValueError, PermissionError, FileExistsError, FileNotFoundError):
        return
    raise AssertionError("invalid ORIGINAL DouZero role source was admitted")


def main():
    exporter.verified_source()  # original 32/32 checked separately
    assert exporter.PROFILE == "douzero.original.wp"
    assert exporter.UPSTREAM_COMMIT == "718a5c920bf3361e34178a38f3b80458e176b351"
    assert exporter.MODEL_TYPES == {
        "landlord": 319, "landlord_up": 430, "landlord_down": 430}

    with tempfile.TemporaryDirectory(prefix="original-douzero-export-contract-") as d:
        root = Path(d)
        root.chmod(0o700)
        checkpoint = root / "not-a-model.ckpt"
        checkpoint.write_bytes(b"INTENTIONALLY INVALID CHECKPOINT CONTRACT FIXTURE")
        checkpoint.chmod(0o600)
        sha = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        full = root / "not-original-model.tar"
        full.write_bytes(b"INTENTIONALLY INVALID WP MODEL.TAR TEST FIXTURE")
        full.chmod(0o600)
        full_sha = hashlib.sha256(full.read_bytes()).hexdigest()
        out = root / "result"
        args = SimpleNamespace(
            role="landlord", checkpoint=checkpoint,
            expected_checkpoint_sha256=sha,
            full_training_checkpoint=full,
            expected_full_training_sha256=full_sha,
            model_version=104, out_dir=out)
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "role": "legacy_dmc"})))
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "model_version": 0})))
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "expected_checkpoint_sha256": "f" * 64})))
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "expected_checkpoint_sha256": "not_sha"})))
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "expected_full_training_sha256": "d" * 64})))
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "expected_full_training_sha256": "BAD"})))
        checkpoint.chmod(0o644)
        rejected(lambda: exporter.export(args))
        checkpoint.chmod(0o600)
        full.chmod(0o644)
        rejected(lambda: exporter.export(args))
        full.chmod(0o600)
        link = root / "symlink.ckpt"
        link.symlink_to(checkpoint)
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "checkpoint": link})))
        link.unlink()
        out.mkdir(mode=0o700)
        rejected(lambda: exporter.export(args))
        assert list(out.iterdir()) == []
        out.rmdir()
        other = root / "no-such-file.ckpt"
        rejected(lambda: exporter.export(SimpleNamespace(
            **{**vars(args), "checkpoint": other})))
        assert not out.exists()
        assert stat.S_IMODE(checkpoint.stat().st_mode) == 0o600
    print("mygames.douzero_original_export_preflight: PASS "
          "source pinned, invalid/old checkpoint forbidden, no ONNX created")


if __name__ == "__main__":
    main()
