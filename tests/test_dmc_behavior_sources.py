"""R9a: real native C11 multi-behavior cohort provenance and rejection tests.

Runs the EXISTING OS-CSPRNG MyGames game/replay producer, not synthetic
episodes or a second rule engine. Private cards/files remain local in 0700.
No heavy PyTorch/NumPy and no artifact upload.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COLLECT = ROOT / "ai/dmc/collect_behavior_source.py"
VERIFY = ROOT / "ai/dmc/verify_behavior_sources.py"
BEHAVIORS = ("mixed-v1", "shed-most", "legal-index")
SPLITS = ("train", "validation", "test")


def call(args, *, successful: bool) -> str:
    response = subprocess.run(
        [sys.executable, *map(str, args)], cwd=ROOT, capture_output=True,
        text=True, timeout=180,
    )
    if successful:
        if response.returncode != 0:
            raise AssertionError("real C11 behavior provenance gate rejected")
    else:
        if response.returncode == 0:
            raise AssertionError("invalid/overlapping source cohort admitted")
    return response.stdout.strip()


def verifier_args(roots, sha, revision):
    args = [VERIFY, "--source-revision", revision,
            "--generator-sha256", sha, "--objective", "win-sign"]
    for behavior in BEHAVIORS:
        args += ["--require-behavior", behavior]
    for root in roots:
        args += ["--cohort", root]
    return args


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_dmc_behavior_sources.py NATIVE_GENERATOR SOURCE_SHA")
    generator, revision = Path(sys.argv[1]).absolute(), sys.argv[2]
    if not generator.is_file() or len(revision) != 40:
        raise ValueError("real built C11 generator/exact HEAD required")
    generator_sha = hashlib.sha256(generator.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="mygames-private-source-") as tmp:
        base = Path(tmp)
        cohorts = []
        for behavior_index, behavior in enumerate(BEHAVIORS):
            for split_index, split in enumerate(SPLITS):
                root = base / f"{behavior}-{split}"
                root.mkdir(mode=0o700)
                episode_id = 33001 + behavior_index * 100 + split_index * 10
                command = [
                    COLLECT, "--native-generator", generator,
                    "--generator-sha256", generator_sha,
                    "--source-checkout", ROOT,
                    "--source-revision", revision,
                    "--private-dir", root, "--first-id", episode_id,
                    "--episodes", 1, "--behavior", behavior,
                    "--split", split, "--objective", "win-sign",
                ]
                result = json.loads(call(command, successful=True))
                assert result["status"] == "sealed-native-C11-cohort"
                assert (result["behavior"], result["split"]) == (behavior, split)
                assert result["episodes"] == 1
                assert result["ready_for_multisource_optimization"] is False
                assert (root / "behavior.v1.json").is_file()
                assert len(list(root.iterdir())) == 2
                cohorts.append(root)
                # Atomic no-replace: no partial/duplicate source can be appended.
                call(command, successful=False)
                assert len(list(root.iterdir())) == 2

        verified = json.loads(call(
            verifier_args(cohorts, generator_sha, revision), successful=True))
        assert verified["cohorts_verified"] == 9
        assert verified["complete_episodes_by_split"] == {
            "train": 3, "validation": 3, "test": 3}
        assert verified["ready_for_multisource_optimization"] is False
        assert verified["cross_split_episode_ids_and_bytes_disjoint"]

        # Wrong binary/source pin, omission, duplication, and a reused cohort
        # are rejected BEFORE any optimizer can interpret numerical model IDs.
        call(verifier_args(cohorts, "0" * 64, revision), successful=False)
        call(verifier_args(cohorts, generator_sha, "0" * 40), successful=False)
        call(verifier_args(cohorts[:-1], generator_sha, revision), successful=False)
        call(verifier_args(cohorts[:-1] + [cohorts[0]],
                           generator_sha, revision), successful=False)

        # Mutating just the manifest, or native Replay content, must fail.
        root = cohorts[0]
        marker = root / "behavior.v1.json"
        original_manifest = marker.read_bytes()
        manifest = json.loads(original_manifest)
        assert manifest["model_version"] == 1
        try:
            manifest["behavior"] = "unverified-policy"
            marker.write_text(json.dumps(manifest))
            call(verifier_args(cohorts, generator_sha, revision), successful=False)
        finally:
            marker.write_bytes(original_manifest)
        # JSON bool/float are not valid integer source provenance.
        try:
            altered_type = json.loads(original_manifest)
            altered_type["model_version"] = True
            marker.write_text(json.dumps(altered_type))
            call(verifier_args(cohorts, generator_sha, revision), successful=False)
            altered_type = json.loads(original_manifest)
            altered_type["episodes"][0]["role_decisions"] = [1.0, 1.0, 1.0]
            marker.write_text(json.dumps(altered_type))
            call(verifier_args(cohorts, generator_sha, revision), successful=False)
        finally:
            marker.write_bytes(original_manifest)
        replay_path = root / f"episode-{33001}.mgdmc"
        original_replay = replay_path.read_bytes()
        try:
            altered = bytearray(original_replay)
            altered[-1] ^= 1  # fail both private digest and Replay CRC
            replay_path.write_bytes(altered)
            call(verifier_args(cohorts, generator_sha, revision), successful=False)
        finally:
            replay_path.write_bytes(original_replay)

        # An extra source file, symlink, or stale partial publication is never
        # silently allowed in a supposedly complete private cohort.
        extra = root / ".native.partial"
        extra.write_bytes(b"not-a-complete-replay")
        try:
            call(verifier_args(cohorts, generator_sha, revision), successful=False)
        finally:
            extra.unlink()

        # Restored original immutable bytes still pass the independently
        # sourced matrix audit, without reopening the held-out split for train.
        call(verifier_args(cohorts, generator_sha, revision), successful=True)
    print("mygames.dmc_native_behavior_source_provenance: PASS")


if __name__ == "__main__":
    main()
