"""Standard-library strict model/source manifest admission negatives.

True complete C11 Replay positive acceptance runs privately in the trusted
collector environment; CI must NOT fabricate a successful on-policy source.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))
from verify_onpolicy import verify


def reject(fn):
    try:
        fn()
    except (ValueError, PermissionError, FileNotFoundError):
        return
    raise AssertionError("invalid on-policy provenance was accepted")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        checkpoint = base / "frozen.pt"
        checkpoint.write_bytes(b"not a real model -- provenance check only")
        checkpoint.chmod(0o600)
        sha = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        dirs = []
        for name in ("train", "validation", "test"):
            directory = base / name
            directory.mkdir(mode=0o700)
            dirs.append(directory)
        args = SimpleNamespace(
            train_dir=dirs[0], validation_dir=dirs[1], test_dir=dirs[2],
            checkpoint=checkpoint, checkpoint_sha256=sha,
            native_library_sha256="a" * 64,
            source_revision="b" * 40,
            behavior_model_version=3, next_model_version=4,
        )
        reject(lambda: verify(args))  # no complete private manifests
        args.next_model_version = 3
        reject(lambda: verify(args))  # cannot reuse behavior model version
        args.next_model_version = 4
        args.checkpoint_sha256 = "0" * 64
        reject(lambda: verify(args))  # immutable frozen weight mismatch
        args.checkpoint_sha256 = sha
        manifest = {
            "schema": "mygames-private-onpolicy-v1",
            "behavior_policy": "frozen-greedy-q-v3",
            "source_revision_claim": args.source_revision,
            "native_library_sha256": args.native_library_sha256,
            "source_checkpoint_sha256": args.checkpoint_sha256,
            "model_version": args.behavior_model_version,
            "rules_version": 1, "objective": "win-sign",
            "shuffle": "OS-CSPRNG-c11", "episodes": [],
            "role_decisions": [0, 0, 0], "total_decisions": 0,
            "model_score_batches_by_role": [0, 0, 0],
        }
        for directory in dirs:
            path = directory / "onpolicy.v1.json"
            path.write_text(json.dumps(manifest))
            path.chmod(0o600)
        reject(lambda: verify(args))  # zero complete C11 episodes
        args.test_dir = args.train_dir
        reject(lambda: verify(args))  # cannot recycle train as test
    print("mygames.dmc_onpolicy_manifest_rejections: PASS")


if __name__ == "__main__":
    main()
