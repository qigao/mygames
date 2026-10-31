#!/usr/bin/env python3
"""Run an isolated, reproducible-by-digest MyGames DMC offline experiment.

The ONLY game engine is the separately built native C11 generator. Dataset
partitions contain complete privately generated OS-shuffled matches, not
per-step splits. No HTTP, real user data, TurboDB, broker or automatic
downloads. --train requires a separately configured PyTorch installation.
This program is a local operator CLI, not a service or scheduled worker.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

from replay import read_episode

MAX_GAMES_PER_SPLIT = 96
MAX_ID = 2**63 - 1
SCHEMA = "mygames-private-dmc-experiment-v1"
GENERATOR_LINE = re.compile(
    r"MYGAMES_PRIVATE_CORPUS_V1 episodes=(\d+) steps=(\d+) "
    r"landlord=(\d+) landlord_up=(\d+) landlord_down=(\d+) "
    r"objective=(win-sign|game-points)"
)
SPLITS = ("train", "validation", "test")
ROLE_NAMES = ("landlord", "landlord_up", "landlord_down")
OBJ_IDS = {"win-sign": 1, "game-points": 2}


def check_private_dir(path: Path) -> None:
    item = path.lstat()  # never silently follow a symlink
    if not stat.S_ISDIR(item.st_mode) or item.st_uid != os.geteuid() or \
            (stat.S_IMODE(item.st_mode) & 0o077):
        raise PermissionError("offline data directory must be owned, private and not a symlink")


def check_executable(path: Path) -> Path:
    if path.is_symlink():
        raise ValueError("native generator path must not be a symlink")
    resolved = path.resolve(strict=True)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError("native C11 corpus generator missing/not executable")
    return resolved


def source_revision(repo: Path, expected: str) -> None:
    if not re.fullmatch(r"[0-9a-f]{40}", expected):
        raise ValueError("exact 40-character Git commit SHA required")
    current = execute(["git", "rev-parse", "HEAD"], cwd=repo)
    if current != expected:
        raise ValueError("source revision mismatch with checked-out Git HEAD")


def generator_identity(native: Path) -> dict:
    # This binds a private experiment to the actual binary which generated
    # the canonical samples, not just its writable basename or an operator
    # asserted commit SHA. SHA-256 is provenance, NOT authentication.
    info = native.stat()
    if not stat.S_ISREG(info.st_mode):
        raise ValueError("not a regular native generator executable")
    return {"name": native.name, "sha256": hashlib.sha256(native.read_bytes()).hexdigest()}


def execute(args: list[str], cwd: Path) -> str:
    # Only trusted local CLI invocations; never interpolate user paths into a shell.
    completed = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                               check=True, timeout=1800)
    return completed.stdout.strip()


def no_clobber_json(root: Path, name: str, value: dict) -> None:
    destination = root / name
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("immutable experiment manifest already exists")
    fd, path = tempfile.mkstemp(prefix=".dmc-private-manifest-", dir=root)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            json.dump(value, out, sort_keys=True, separators=(",", ":"))
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.link(path, destination)  # atomic NO-CLOBBER, no os.replace
        dirfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        if os.path.exists(path):
            os.unlink(path)


def record_batch(folder: Path, ids: range, requested_objective: str,
                 native: Path, repo: Path) -> dict:
    expected_files = {f"episode-{i}.mgdmc" for i in ids}
    found_files = {p.name for p in folder.iterdir()}
    if found_files != expected_files:
        raise ValueError("incomplete/extra private Replay files, including partial or symlink")
    records = []
    counts = [0, 0, 0]
    total_steps = 0
    for eid in ids:
        path = folder / f"episode-{eid}.mgdmc"
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or \
                (stat.S_IMODE(info.st_mode) & 0o077) or info.st_nlink != 1:
            raise PermissionError("Replay must be owned 0600 regular file with one link")
        raw = path.read_bytes()
        episode = read_episode(path)
        if episode.episode_id != eid or episode.model_version != 1 or \
                episode.rules_version != 1 or episode.objective != OBJ_IDS[requested_objective] or \
                episode.landlord_seat != (eid - 1) % 3:
            raise ValueError("Replay source/role/objective admission failed")
        per_role = [0, 0, 0]
        for step in episode.samples:
            per_role[step.role] += 1
        for role in range(3):
            counts[role] += per_role[role]
        total_steps += len(episode.samples)
        records.append({"file": f"{folder.name}/{path.name}",
                        "sha256": hashlib.sha256(raw).hexdigest(),
                        "episode_id": eid,
                        "accepted_actions": len(episode.samples),
                        "landlord_seat": episode.landlord_seat,
                        "winner_seat": episode.winner_seat,
                        "role_counts": dict(zip(ROLE_NAMES, per_role))})
    if not all(counts):
        raise ValueError("dataset split lacks at least one complete role sample")
    return {"episodes": len(records), "decisions": total_steps,
            "role_counts": dict(zip(ROLE_NAMES, counts)),
            "records": records}


def build_experiment(args, repo: Path) -> dict:
    if sys.platform != "linux":
        raise RuntimeError("private offline generator currently supports Linux only")
    root = Path(args.private_root).absolute()
    check_private_dir(root)
    native = check_executable(Path(args.native_generator))
    if args.first_id < 1 or args.first_id + sum(
            (args.train_games, args.validation_games, args.test_games)) - 1 > MAX_ID:
        raise ValueError("invalid first episode ID / overflow")
    source_revision(repo, args.source_revision)
    for split in SPLITS:
        count = getattr(args, f"{split}_games")
        if not 1 <= count <= MAX_GAMES_PER_SPLIT:
            raise ValueError(f"{split} games must be 1..{MAX_GAMES_PER_SPLIT}")
        if (root / split).exists() or (root / split).is_symlink():
            raise FileExistsError(f"experiment split already exists: {split}")
    if (root / "manifest.v1.json").exists() or (root / "models").exists():
        raise FileExistsError("experiment already has a manifest or models")

    result = {"schema": SCHEMA, "source_revision": args.source_revision,
              "native_generator": generator_identity(native),
              "source_policy": "untrained-mixed-baseline-v1",
              "rules_version": 1, "model_version": 1,
              "objective": args.objective, "splits": {}}
    first_id = args.first_id
    for split in SPLITS:
        count = getattr(args, f"{split}_games")
        folder = root / split
        folder.mkdir(mode=0o700, exist_ok=False)
        check_private_dir(folder)
        output = execute([
            str(native), "--private-dir", str(folder),
            "--episodes", str(count), "--first-id", str(first_id),
            "--objective", args.objective
        ], cwd=repo)
        match = GENERATOR_LINE.fullmatch(output)
        if not match or int(match.group(1)) != count or \
                match.group(6) != args.objective:
            raise ValueError("native generator completion is unverified")
        data = record_batch(folder, range(first_id, first_id + count),
                            args.objective, native, repo)
        if data["decisions"] != int(match.group(2)) or [
            data["role_counts"][r] for r in ROLE_NAMES
        ] != [int(match.group(i)) for i in (3, 4, 5)]:
            raise ValueError("generator public aggregate differs from verified files")
        result["splits"][split] = data
        first_id += count

    hashes = [x["sha256"] for group in result["splits"].values()
              for x in group["records"]]
    if len(hashes) != len(set(hashes)):
        raise ValueError("identical private Replay data across splits")
    ids = [x["episode_id"] for group in result["splits"].values()
           for x in group["records"]]
    if len(ids) != len(set(ids)):
        raise ValueError("episode ID overlap across splits")
    return result



def verify_existing_manifest(args, repo: Path) -> dict:
    """Validate an IMMUTABLE completed corpus without regenerating anything.

    Reuse accepts only the exact same checkout SHA, exact generator identity,
    and the original three independent Replay partitions. Every file is
    reopened, schema-checked and rehashed; editing a manifest cannot create
    an eligible experiment or silently redirect any path outside PRIVATE_ROOT.
    """
    root = Path(args.private_root).absolute()
    check_private_dir(root)
    source_revision(repo, args.source_revision)
    native = check_executable(Path(args.native_generator))
    path = root / "manifest.v1.json"
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or \
            (stat.S_IMODE(info.st_mode) & 0o077) or info.st_nlink != 1 or \
            info.st_size > 512 * 1024:
        raise PermissionError("source manifest must be private, immutable and bounded")
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or set(manifest) != {
        "schema", "source_revision", "native_generator",
        "source_policy", "rules_version", "model_version",
        "objective", "splits"
    } or manifest["schema"] != SCHEMA or \
            manifest["source_revision"] != args.source_revision or \
            manifest["native_generator"] != generator_identity(native) or \
            manifest["source_policy"] != "untrained-mixed-baseline-v1" or \
            manifest["rules_version"] != 1 or manifest["model_version"] != 1 or \
            manifest["objective"] != args.objective or \
            not isinstance(manifest["splits"], dict) or \
            set(manifest["splits"]) != set(SPLITS):
        raise ValueError("source manifest provenance / model / rules mismatch")
    total = args.train_games + args.validation_games + args.test_games
    if args.first_id < 1 or args.first_id + total - 1 > MAX_ID:
        raise ValueError("invalid original experiment episode ID range")
    expected = args.first_id
    for split in SPLITS:
        n = getattr(args, f"{split}_games")
        if not 1 <= n <= MAX_GAMES_PER_SPLIT:
            raise ValueError("invalid expected split size")
        folder = root / split
        check_private_dir(folder)
        rebuilt = record_batch(folder, range(expected, expected + n),
                               args.objective, native, repo)
        if manifest["splits"][split] != rebuilt:
            raise ValueError("private Replay digest, role, episode or count changed")
        expected += n
    if len({x["episode_id"] for group in manifest["splits"].values()
            for x in group["records"]}) != total:
        raise ValueError("source manifest has overlapping episode IDs")
    if len({x["sha256"] for group in manifest["splits"].values()
            for x in group["records"]}) != total:
        raise ValueError("source manifest has duplicated private replay content")
    allowed = {*SPLITS, "manifest.v1.json", "models", "training.v1.json"}
    if {p.name for p in root.iterdir()} - allowed:
        raise ValueError("private experiment has unexpected root entries")
    return manifest


def file_list(root: Path, split: dict) -> list[str]:
    return [str(root / item["file"]) for item in split["records"]]


def verify_reader(root: Path, manifest: dict, repo: Path) -> dict:
    base = [sys.executable, str(repo / "ai/dmc/train_dmc.py")]
    train_files = file_list(root, manifest["splits"]["train"])
    val_files = file_list(root, manifest["splits"]["validation"])
    test_files = file_list(root, manifest["splits"]["test"])
    output = execute(base + [
        "--verify-only", "--replay", *train_files, "--validation-replay",
        *val_files, "--objective", manifest["objective"], "--rules-version", "1"
    ], cwd=repo)
    train_report = json.loads(output)
    test_report = json.loads(execute(base + [
        "--verify-only", "--replay", *test_files,
        "--objective", manifest["objective"], "--rules-version", "1"
    ], cwd=repo))
    if (train_report["episodes"] != len(train_files) or
            train_report["validation_episodes"] != len(val_files) or
            test_report["episodes"] != len(test_files)):
        raise ValueError("Python reader did not accept exact episode counts")
    return {"train_episodes": len(train_files),
            "validation_episodes": len(val_files),
            "test_episodes": len(test_files),
            "train_decisions": train_report["completed_steps"],
            "validation_decisions": train_report["validation_steps"],
            "test_decisions": test_report["completed_steps"]}


def train_and_test(root: Path, manifest: dict, repo: Path, args) -> dict:
    # Generate/test examples are PRIVATE. This command never publishes
    # a model into the Room Owner, public CI artifacts or production.
    modeldir = root / "models"
    modeldir.mkdir(mode=0o700, exist_ok=False)
    check_private_dir(modeldir)
    checkpoint = modeldir / "dmc-unpromoted-v3.pt"
    base = [sys.executable, str(repo / "ai/dmc/train_dmc.py")]
    train_files = file_list(root, manifest["splits"]["train"])
    val_files = file_list(root, manifest["splits"]["validation"])
    test_files = file_list(root, manifest["splits"]["test"])
    training = json.loads(execute(base + [
        "--train", "--replay", *train_files,
        "--validation-replay", *val_files,
        "--objective", manifest["objective"], "--rules-version", "1",
        "--output", str(checkpoint), "--model-version", str(args.model_version),
        "--steps", str(args.steps),
        "--batch-size", str(args.batch_size), "--learning-rate",
        str(args.learning_rate), "--device", args.device,
        "--seed", str(args.seed)
    ], cwd=repo))
    held_out = json.loads(execute(base + [
        "--evaluate-only", "--replay", *test_files,
        "--objective", manifest["objective"], "--rules-version", "1",
        "--checkpoint", str(checkpoint), "--model-version", str(args.model_version),
        "--device", args.device,
        "--batch-size", str(args.batch_size)
    ], cwd=repo))
    digest = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    if training["optimizer"]["checkpoint_sha256"] != digest:
        raise ValueError("checkpoint digest changed between fitting and test")
    return {"checkpoint_sha256": digest,
            "validation_before": training["optimizer"]["validation_before"],
            "validation_after": training["optimizer"]["validation_after"],
            "independent_test": held_out["metrics"],
            "trained_policy_qualified": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-generator", required=True)
    parser.add_argument("--private-root", required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--first-id", type=int, required=True)
    parser.add_argument("--train-games", type=int, default=12)
    parser.add_argument("--validation-games", type=int, default=6)
    parser.add_argument("--test-games", type=int, default=6)
    parser.add_argument("--objective", choices=("win-sign", "game-points"), required=True)
    parser.add_argument("--train", action="store_true",
                        help="actually optimize PyTorch Q and test on independent games")
    parser.add_argument("--model-version", type=int,
                        help="new frozen model ID, > every source behavior model version")
    parser.add_argument("--reuse", action="store_true",
                        help="verify and reuse immutable existing private manifest; no new games")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", default="cpu")
    args = parser.parse_args()
    if args.steps < 1 or args.batch_size < 1 or args.seed < 0 or \
            not 0 < args.learning_rate <= 1:
        parser.error("invalid optimizer configuration")
    if args.train and (args.model_version is None or
                       not 1 < args.model_version < 2**64):
        parser.error("--train requires --model-version > native source model 1")
    if not args.train and args.model_version is not None:
        parser.error("--model-version is only valid with --train")
    repo = Path(__file__).resolve().parents[2]
    manifest = (verify_existing_manifest(args, repo) if args.reuse
                else build_experiment(args, repo))
    root = Path(args.private_root).absolute()
    verified = verify_reader(root, manifest, repo)
    report = {"schema": SCHEMA, "source_revision": args.source_revision,
              "objective": args.objective, "verified": verified,
              "reused_existing_data": args.reuse,
              "torch_optimization_executed": args.train}
    if args.train:
        if (root / "models").exists() or (root / "training.v1.json").exists():
            raise FileExistsError("trained model/metadata exists; refuse overwrite")
        report["model"] = train_and_test(root, manifest, repo, args)
    if not args.reuse:
        # Source manifest is admitted only AFTER all Replay partitions
        # pass; never create a false complete run after a failed job.
        no_clobber_json(root, "manifest.v1.json", manifest)
    if args.train:
        # Report is ALSO privately retained and never published to public
        # artifacts, exposing only aggregate role metrics and model SHA-256.
        no_clobber_json(root, "training.v1.json", report)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
