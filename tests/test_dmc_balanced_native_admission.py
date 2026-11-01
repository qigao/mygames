"""M5k R9b: POSITIVE training-admission gate using 144 genuine C11 games.

No PyTorch or GPU is installed in GitHub CI. This proves real OS-CSPRNG
source-balanced TRAIN and TUNING are ACCEPTED, not training effectiveness.
Never prints raw Replay cards/features/IDs/checkpoint files or uses TEST.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))
from train_policy import make_role_sampler
from verified_behavior_training import admit_training_sources, exact_replay_paths

BEHAVIORS = ("mixed-v1", "shed-most", "legal-index")
SPLITS = ("train", "validation")
GAMES_PER_COHORT = 24


def run_collector(generator, digest, revision, folder, source, split, first_id):
    cmd = [
        sys.executable, str(ROOT / "ai/dmc/collect_behavior_source.py"),
        "--native-generator", str(generator), "--generator-sha256", digest,
        "--source-checkout", str(ROOT), "--source-revision", revision,
        "--private-dir", str(folder), "--first-id", str(first_id),
        "--episodes", str(GAMES_PER_COHORT), "--behavior", source,
        "--split", split, "--objective", "win-sign",
    ]
    completed = subprocess.run(
        cmd, cwd=ROOT, capture_output=True, text=True, timeout=180,
    )
    if completed.returncode:
        raise RuntimeError("true native C11 behavior cohort collection rejected")
    report = json.loads(completed.stdout)
    if (report.get("status") != "sealed-native-C11-cohort" or
            report.get("episodes") != GAMES_PER_COHORT or
            report.get("ready_for_multisource_optimization") is not False or
            report.get("behavior") != source or report.get("split") != split):
        raise ValueError("native private source completion metadata mismatch")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_dmc_balanced_native_admission.py GENERATOR HEAD_SHA")
    generator = Path(sys.argv[1]).absolute()
    revision = sys.argv[2]
    if not generator.is_file() or len(revision) != 40:
        raise ValueError("exact immutable source and built native C11 generator required")
    digest = hashlib.sha256(generator.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="mygames-r9b-private-") as tmp:
        base = Path(tmp)
        train_roots, validation_roots = [], []
        for source_index, source in enumerate(BEHAVIORS):
            for split_index, split in enumerate(SPLITS):
                folder = base / f"{source}-{split}"
                folder.mkdir(mode=0o700)
                first_id = 50001 + source_index * 1000 + split_index * 100
                run_collector(generator, digest, revision, folder,
                              source, split, first_id)
                (train_roots if split == "train" else validation_roots).append(folder)

        admitted = admit_training_sources(
            train_cohorts=train_roots,
            validation_cohorts=validation_roots,
            required_behaviors=BEHAVIORS,
            source_revision=revision,
            generator_sha256=digest,
            objective="win-sign",
        )
        if (admitted["episodes_by_split"] != {
                "train": GAMES_PER_COHORT * 3,
                "validation": GAMES_PER_COHORT * 3} or
                set(admitted["sources"]) != set(BEHAVIORS) or
                admitted["qualified_gameplay_skill"] is not False):
            raise AssertionError("native 3-source 2-split admission mismatch")
        train = admitted["train_episodes"]
        validation = admitted["validation_episodes"]
        assert len(train) == len(validation) == GAMES_PER_COHORT * 3
        assert {e.episode_id for e in train}.isdisjoint(
            {e.episode_id for e in validation})
        for role in range(3):
            for name, episodes, labels in (
                ("train", train, admitted["train_source_by_episode"]),
                ("validation", validation, admitted["validation_source_by_episode"]),
            ):
                sampler = make_role_sampler(
                    episodes, role, verified_source_by_episode=labels)
                assert set(sampler.strata_counts()) == set(BEHAVIORS)
                for outcomes in sampler.strata_counts().values():
                    assert outcomes["positive_games"] >= 1
                    assert outcomes["negative_games"] >= 1
                assert len(sampler.batch(random.Random(role + 31), 128)) == 128

        # No foreign/unverified Replay may be appended to the admitted list.
        exact_replay_paths(admitted["train_files"],
                           admitted["train_files"])
        try:
            exact_replay_paths(
                admitted["train_files"] + admitted["validation_files"][:1],
                admitted["train_files"])
        except ValueError:
            pass
        else:
            raise AssertionError("TEST/TUNING Replay smuggling accepted")
        assert not (base / "test").exists()
        assert not (base / "models").exists()
    print("mygames.dmc_real_native_balanced_source_admission: PASS")
    print("3 sources x 2 sealed TRAIN/TUNING cohorts x 24 genuine C11 games")
    print("All roles and source-outcome buckets present; TEST unopened; Torch not invoked")


if __name__ == "__main__":
    main()
