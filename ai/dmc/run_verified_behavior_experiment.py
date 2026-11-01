#!/usr/bin/env python3
"""Run one offline, source-pinned multi-behavior DMC learner experiment.

TRAIN and TUNING cohorts are genuine, private, immutable MyGames C11 Replay.
An optional operator-configured PyTorch environment performs the actual
optimizer updates. This script does NOT generate new games, open TEST,
download a backend, select tournament seeds, or promote a model.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import stat
import subprocess
import sys

from model_source_record import verify_record, record_path
from verified_behavior_training import (
    admit_training_sources, verify_exact_learner_revision,
)

ROOT = Path(__file__).resolve().parents[2]


def run(args):
    if (args.model_version is None or
            not 2 <= args.model_version < 2**64 or
            not 1 <= args.steps <= 1000000 or
            not 1 <= args.batch_size <= 65536 or
            not 0 < args.learning_rate <= 1 or
            not 1 <= args.validation_interval <= args.steps):
        raise ValueError("bounded frozen model/optimizer parameters are required")
    verify_exact_learner_revision(args.learner_revision, ROOT)
    audited = admit_training_sources(
        train_cohorts=args.train_cohort,
        validation_cohorts=args.validation_cohort,
        required_behaviors=args.source,
        source_revision=args.source_revision,
        generator_sha256=args.generator_sha256,
        objective="win-sign",
    )
    target = args.output.absolute()
    if (target.exists() or target.is_symlink() or
            record_path(target).exists() or record_path(target).is_symlink()):
        raise FileExistsError("refuse overwrite of any existing checkpoint/source record")
    parent = target.parent
    info = parent.lstat()
    if (not stat.S_ISDIR(info.st_mode) or
            info.st_uid != os.geteuid() or
            stat.S_IMODE(info.st_mode) & 0o077):
        raise PermissionError("private checkpoint directory must already be owned 0700")
    if os.path.commonpath((str(parent.resolve(strict=True)), str(ROOT))) == str(ROOT):
        raise ValueError("store private learned weights OUTSIDE the source checkout")
    # The learner re-admits each cohort and obtains its exact immutable
    # Replay list from trusted manifests. Do NOT expand the thousands of
    # individual private files into OS command-line arguments. TEST unopened.
    cmd = [
        sys.executable, str(ROOT / "ai/dmc/train_dmc.py"),
        "--train", "--rules-version", "1", "--objective", "win-sign",
        "--output", str(target),
        "--model-version", str(args.model_version),
        "--steps", str(args.steps),
        "--batch-size", str(args.batch_size),
        "--learning-rate", str(args.learning_rate),
        "--seed", str(args.seed),
        "--device", args.device,
        "--sampling", "episode-source-outcome",
        "--checkpoint-policy", "best-validation",
        "--validation-interval", str(args.validation_interval),
        "--require-validation-advantage",
        "--behavior-source-revision", args.source_revision,
        "--behavior-generator-sha256", args.generator_sha256,
        "--behavior-learner-revision", args.learner_revision,
    ]
    for source in audited["sources"]:
        cmd.extend(("--behavior-required-source", source))
    for directory in args.train_cohort:
        cmd.extend(("--behavior-train-cohort", str(directory.absolute())))
    for directory in args.validation_cohort:
        cmd.extend(("--behavior-validation-cohort", str(directory.absolute())))
    completed = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                               text=True, timeout=7200)
    if completed.returncode != 0:
        # Do not echo stdout/stderr: a native/PyTorch library might print
        # private paths. Preserve the nonzero result and report class only.
        raise RuntimeError("verified real DMC PyTorch training failed; no promotion")
    learner_report = json.loads(completed.stdout)
    optimizer = learner_report["optimizer"]
    if (learner_report.get("source_manifest_bundle_sha256") !=
            audited["manifest_bundle_sha256"] or
            optimizer.get("candidate_model_version") != args.model_version or
            learner_report.get("heldout_test_opened_for_training") is not False or
            learner_report.get("qualified_gameplay_skill") is not False or
            optimizer.get("validation_selection_weighting") !=
            "episode-source-outcome"):
        raise RuntimeError("learner result/source selection did not match pinned admission")
    record = verify_record(
        target, expected_model_version=args.model_version,
        expected_bundle_sha256=audited["manifest_bundle_sha256"])
    if (optimizer["checkpoint_sha256"] != record["checkpoint_sha256"] or
            record["source_revision"] != args.source_revision or
            record["learner_revision"] != args.learner_revision or
            record["native_generator_sha256"] != args.generator_sha256 or
            record["verified_behavior_sources"] != list(audited["sources"])):
        raise RuntimeError("frozen checkpoint missing exact policy/source binding")
    # Only PUBLIC counts and digest are reported, no raw weights/Replay/IDs.
    return {
        "status": "private-immutable-multisource-candidate",
        "candidate_model_version": args.model_version,
        "checkpoint_sha256": optimizer["checkpoint_sha256"],
        "source_bundle_sha256": audited["manifest_bundle_sha256"],
        "sources": list(audited["sources"]),
        "complete_train_games": audited["episodes_by_split"]["train"],
        "complete_tuning_games": audited["episodes_by_split"]["validation"],
        "selected_validation_step_by_role":
            optimizer["selected_validation_step_by_role"],
        "valid_source_record": True,
        "heldout_test_opened": False,
        "game_skill_qualified": False,
        "production_bot_promoted": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--train-cohort", type=Path, action="append", required=True)
    parser.add_argument("--validation-cohort", type=Path, action="append",
                        required=True)
    parser.add_argument("--source", action="append", choices=(
        "mixed-v1", "shed-most", "legal-index"), required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--generator-sha256", required=True)
    parser.add_argument("--learner-revision", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--model-version", type=int, required=True)
    parser.add_argument("--steps", type=int, default=400)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--validation-interval", type=int, default=20)
    parser.add_argument("--seed", type=int, default=31)
    parser.add_argument("--device", default="cpu")
    print(json.dumps(run(parser.parse_args()), sort_keys=True))


if __name__ == "__main__":
    main()
