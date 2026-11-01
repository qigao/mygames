#!/usr/bin/env python3
"""Prepare and train a bounded, real native C11 multi-behavior DMC corpus.

Offline operator only. No new rules engine, deterministic production shuffle,
network service, Torch auto-install, private artifact upload or TEST access.

"prepare" seals 2-3 strategies into multi-shard immutable TRAIN/TUNING
Replay using the authoritative C11 OS-CSPRNG generator. "train" reopens and
audits those exact files and calls the EXISTING verified PyTorch learner.
This is not a model skill claim; a completely separate, virgin paired native
TEST is mandatory after model selection and freeze.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from verified_behavior_training import (
    MAX_GAMES_PER_SHARD, MAX_SHARDS_PER_SOURCE,
    admit_training_sources, _roster,
)
from verify_behavior_sources import HEX40, HEX64, require_private

ROOT = Path(__file__).resolve().parents[2]
PLAN = "r9-source-collection-plan.v1.json"
COMPLETED = "r9-source-collection-complete.v1.json"
SPLITS = ("train", "validation")
PER_SOURCE_CAPACITY = MAX_GAMES_PER_SHARD * MAX_SHARDS_PER_SOURCE


def private_json(root: Path, filename: str, document: dict):
    if filename not in (PLAN, COMPLETED):
        raise ValueError("unknown private source completion marker")
    target = root / filename
    if target.exists() or target.is_symlink():
        raise FileExistsError("private source record already published")
    raw = (json.dumps(document, sort_keys=True, separators=(",", ":"),
                      allow_nan=False) + "\n").encode("utf-8")
    if len(raw) > 16384:
        raise ValueError("private operator source marker too large")
    fd, stage = tempfile.mkstemp(dir=root, prefix=".r9-collection-")
    try:
        with os.fdopen(fd, "wb") as dest:
            dest.write(raw)
            dest.flush()
            os.fsync(dest.fileno())
        os.link(stage, target)
        dfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        if os.path.exists(stage):
            os.unlink(stage)
    return hashlib.sha256(raw).hexdigest()


def checked_root(path: Path) -> Path:
    root = path.absolute()
    require_private(root, directory=True)
    return root


def checked_file(path: Path) -> bytes:
    meta = require_private(path, directory=False)
    if not 0 < meta.st_size <= 16384:
        raise ValueError("private R9 operator plan size invalid")
    return path.read_bytes()


def source_cohorts(root: Path, data: dict):
    if not isinstance(data, dict) or set(data) != {"train", "validation"}:
        raise ValueError("source cohort index must contain TRAIN and TUNING only")
    result = {}
    all_names = set()
    for split in SPLITS:
        rows = data[split]
        if not isinstance(rows, list) or len(rows) < 2 or len(rows) > 48:
            raise ValueError("invalid count of real native source shards")
        paths = []
        for name in rows:
            if (not isinstance(name, str) or len(name) > 80 or
                    "/" in name or "\\" in name or name in (".", "..") or
                    name in all_names or not name.endswith(tuple(
                        f"-{split}-shard{j:02d}"
                        for j in range(MAX_SHARDS_PER_SOURCE)))):
                raise ValueError("invalid/private cohort path or duplicate shard")
            all_names.add(name)
            path = root / name
            require_private(path, directory=True)
            paths.append(path)
        result[split] = paths
    return result, all_names


def admit_complete(root: Path):
    root = checked_root(root)
    plan_bytes, done_bytes = (
        checked_file(root / PLAN), checked_file(root / COMPLETED))
    plan = json.loads(plan_bytes)
    done = json.loads(done_bytes)
    if (not isinstance(plan, dict) or not isinstance(done, dict) or
            plan.get("schema") != "mygames-r9-native-sharded-plan-v1" or
            done.get("schema") != "mygames-r9-native-sharded-complete-v1" or
            done.get("plan_sha256") != hashlib.sha256(plan_bytes).hexdigest() or
            done.get("source_revision") != plan.get("source_revision") or
            done.get("generator_sha256") != plan.get("generator_sha256") or
            not isinstance(plan.get("behaviors"), list) or
            plan["behaviors"] != sorted(_roster(plan["behaviors"])) or
            not isinstance(plan.get("source_revision"), str) or
            HEX40.fullmatch(plan["source_revision"]) is None or
            not isinstance(plan.get("generator_sha256"), str) or
            HEX64.fullmatch(plan["generator_sha256"]) is None):
        raise ValueError("R9 source plan/completion does not bind generator revision")
    cohorts, names = source_cohorts(root, done.get("cohorts"))
    if set(p.name for p in root.iterdir()) != names | {PLAN, COMPLETED}:
        raise ValueError("missing/partial/extra files in immutable source experiment")
    audit = admit_training_sources(
        train_cohorts=cohorts["train"],
        validation_cohorts=cohorts["validation"],
        required_behaviors=plan["behaviors"],
        source_revision=plan["source_revision"],
        generator_sha256=plan["generator_sha256"],
        objective="win-sign")
    if (audit["manifest_bundle_sha256"] != done.get("source_bundle_sha256") or
            audit["episodes_by_split"] != done.get("complete_episodes_by_split") or
            audit["shards_by_split"] != done.get("shards_by_split")):
        raise ValueError("source complete marker differs from audited native bytes")
    return plan, cohorts, audit


def prepare(args):
    root = checked_root(args.private_root)
    behaviors = tuple(sorted(_roster(args.behavior)))
    if any(root.iterdir()):
        raise FileExistsError("new source collection must use an EMPTY private root")
    if (not 1 <= args.train_games_per_behavior <= PER_SOURCE_CAPACITY or
            not 1 <= args.validation_games_per_behavior <= PER_SOURCE_CAPACITY or
            not 1 <= args.first_episode_id or
            args.first_episode_id + len(behaviors) * len(SPLITS) *
            PER_SOURCE_CAPACITY - 1 >= 2**63 or
            not isinstance(args.source_revision, str) or
            HEX40.fullmatch(args.source_revision) is None or
            not isinstance(args.generator_sha256, str) or
            HEX64.fullmatch(args.generator_sha256) is None):
        raise ValueError("invalid bounded real C11 source policy/games/episode IDs")
    executable = args.native_generator.absolute()
    if executable.is_symlink() or not executable.is_file():
        raise ValueError("real native generator executable missing or a symlink")
    if hashlib.sha256(executable.read_bytes()).hexdigest() != args.generator_sha256:
        raise ValueError("native generator digest mismatch before collecting any games")
    checkout = args.source_checkout.absolute()
    if not checkout.is_dir() or checkout.is_symlink():
        raise ValueError("exact existing source checkout required")
    if os.path.commonpath((str(root.resolve(strict=True)),
                           str(checkout.resolve(strict=True)))) == str(
                           checkout.resolve(strict=True)):
        raise ValueError("private source corpus cannot live inside Git checkout")

    plan = {
        "schema": "mygames-r9-native-sharded-plan-v1",
        "source_revision": args.source_revision,
        "generator_sha256": args.generator_sha256,
        "objective": "win-sign",
        "behaviors": list(behaviors),
        "first_episode_id": args.first_episode_id,
        "train_games_per_behavior": args.train_games_per_behavior,
        "validation_games_per_behavior": args.validation_games_per_behavior,
        "max_games_per_shard": MAX_GAMES_PER_SHARD,
        "max_shards_per_behavior": MAX_SHARDS_PER_SOURCE,
        "virgin_test_read": False,
        "model_training_completed": False,
    }
    plan_sha = private_json(root, PLAN, plan)  # claim before generating
    names = {"train": [], "validation": []}
    for source_index, behavior in enumerate(behaviors):
        for split_index, split in enumerate(SPLITS):
            budget = (args.train_games_per_behavior if split == "train" else
                      args.validation_games_per_behavior)
            for shard in range(math.ceil(budget / MAX_GAMES_PER_SHARD)):
                count = min(MAX_GAMES_PER_SHARD,
                            budget - shard * MAX_GAMES_PER_SHARD)
                name = f"{behavior}-{split}-shard{shard:02d}"
                folder = root / name
                folder.mkdir(mode=0o700, exist_ok=False)
                eid = (args.first_episode_id +
                       (source_index * len(SPLITS) + split_index) *
                       PER_SOURCE_CAPACITY + shard * MAX_GAMES_PER_SHARD)
                cmd = [
                    sys.executable, str(ROOT / "ai/dmc/collect_behavior_source.py"),
                    "--native-generator", str(executable),
                    "--generator-sha256", args.generator_sha256,
                    "--source-checkout", str(checkout),
                    "--source-revision", args.source_revision,
                    "--private-dir", str(folder),
                    "--first-id", str(eid), "--episodes", str(count),
                    "--behavior", behavior, "--split", split,
                    "--objective", "win-sign",
                ]
                run = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                                     text=True, timeout=1800)
                if run.returncode:
                    raise RuntimeError("authoritative native source collection failed")
                result = json.loads(run.stdout)
                if (result.get("status") != "sealed-native-C11-cohort" or
                        result.get("episodes") != count or
                        result.get("behavior") != behavior or
                        result.get("split") != split):
                    raise RuntimeError("native source producer completion mismatch")
                names[split].append(name)

    audit = admit_training_sources(
        train_cohorts=[root / x for x in names["train"]],
        validation_cohorts=[root / x for x in names["validation"]],
        required_behaviors=behaviors,
        source_revision=args.source_revision,
        generator_sha256=args.generator_sha256, objective="win-sign")
    expected = {
        "train": len(behaviors) * args.train_games_per_behavior,
        "validation": len(behaviors) * args.validation_games_per_behavior}
    if audit["episodes_by_split"] != expected:
        raise RuntimeError("source role/outcome admission or total games rejected")
    private_json(root, COMPLETED, {
        "schema": "mygames-r9-native-sharded-complete-v1",
        "plan_sha256": plan_sha,
        "source_revision": args.source_revision,
        "generator_sha256": args.generator_sha256,
        "cohorts": names,
        "complete_episodes_by_split": expected,
        "shards_by_split": audit["shards_by_split"],
        "source_bundle_sha256": audit["manifest_bundle_sha256"],
        "virgin_test_read": False,
    })  # COMPLETION LAST. Partial/crashed source is always UNADMITTED.
    return {
        "status": "real-C11-sharded-source-corpus-sealed",
        "behaviors": list(behaviors),
        "episodes": expected, "shards_by_split": audit["shards_by_split"],
        "source_bundle_sha256": audit["manifest_bundle_sha256"],
        "ready_for_optional_real_torch_optimizer": True,
        "trained_game_skill_qualified": False,
        "TEST_opened": False,
    }


def train(args):
    # One verified, immutable plan + completion snapshot drives the TRAIN
    # invocation. Avoid separately re-reading potentially changed metadata.
    plan, cohorts, audit = admit_complete(args.private_root)
    from run_verified_behavior_experiment import run
    return run(SimpleNamespace(
        train_cohort=cohorts["train"], validation_cohort=cohorts["validation"],
        source=audit["sources"], source_revision=plan["source_revision"],
        generator_sha256=plan["generator_sha256"],
        learner_revision=args.learner_revision, output=args.output,
        model_version=args.model_version, steps=args.steps,
        batch_size=args.batch_size, learning_rate=args.learning_rate,
        validation_interval=args.validation_interval, seed=args.seed,
        device=args.device,
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    prep = modes.add_parser("prepare")
    prep.add_argument("--private-root", required=True, type=Path)
    prep.add_argument("--native-generator", required=True, type=Path)
    prep.add_argument("--generator-sha256", required=True)
    prep.add_argument("--source-checkout", required=True, type=Path)
    prep.add_argument("--source-revision", required=True)
    prep.add_argument("--behavior", action="append", required=True,
                      choices=("mixed-v1", "shed-most", "legal-index"))
    prep.add_argument("--train-games-per-behavior", type=int, default=384)
    prep.add_argument("--validation-games-per-behavior", type=int, default=96)
    prep.add_argument("--first-episode-id", required=True, type=int)
    training = modes.add_parser("train")
    training.add_argument("--private-root", required=True, type=Path)
    training.add_argument("--output", required=True, type=Path)
    training.add_argument("--learner-revision", required=True)
    training.add_argument("--model-version", required=True, type=int)
    training.add_argument("--steps", type=int, default=400)
    training.add_argument("--batch-size", type=int, default=32)
    training.add_argument("--learning-rate", type=float, default=1e-4)
    training.add_argument("--validation-interval", type=int, default=20)
    training.add_argument("--seed", type=int, default=31)
    training.add_argument("--device", default="cpu")
    args = parser.parse_args()
    output = prepare(args) if args.mode == "prepare" else train(args)
    print(json.dumps(output, sort_keys=True))


if __name__ == "__main__":
    main()
