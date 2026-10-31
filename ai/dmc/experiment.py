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
import shutil
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
    # A verified old C11 generator checkout may differ from the CURRENT
    # learner checkout, but both exact Git commits MUST be pinned. The
    # prior canonical Replay and generator digests remain enforced below.
    if args.source_checkout:
        source_checkout = Path(args.source_checkout)
        if source_checkout.is_symlink() or not source_checkout.is_dir():
            raise ValueError("source checkout must be a real Git worktree directory")
        source_repo = source_checkout.resolve(strict=True)
    else:
        source_repo = repo
    source_revision(source_repo, args.source_revision)
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
    allowed = {*SPLITS, "manifest.v1.json", "models", "training.v1.json",
               "trials", "holdout-lock.v1.json", "holdout.v1.json"}
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


def optimizer_policy_flags(args) -> list[str]:
    """Forward explicitly selected policy to the SINGLE native-Replay learner."""
    options = [
        "--sampling", args.sampling,
        "--checkpoint-policy", args.checkpoint_policy,
        "--validation-interval", str(args.validation_interval),
    ]
    if args.require_validation_advantage:
        options.append("--require-validation-advantage")
    return options


def admit_balanced_sources(root: Path, manifest: dict) -> None:
    """Fail before creating a model directory or importing PyTorch.

    Both independent complete-game pools must contain WIN and LOSS for every
    acting role of every recorded behavior source. Digest/ACL checks remain
    the responsibility of verify_existing_manifest/build_experiment.
    """
    from train_policy import make_role_sampler
    pools = {}
    for split in ("train", "validation"):
        paths = file_list(root, manifest["splits"][split])
        pools[split] = [read_episode(p) for p in paths]
    if {e.model_version for e in pools["train"]} != {
            e.model_version for e in pools["validation"]}:
        raise ValueError("train/validation source-policy cohorts differ")
    for role in range(3):
        make_role_sampler(pools["train"], role)
        make_role_sampler(pools["validation"], role)


def train_and_test(root: Path, manifest: dict, repo: Path, args) -> dict:
    # Generate/test examples are PRIVATE. This command never publishes
    # a model into the Room Owner, public CI artifacts or production.
    modeldir = root / "models"
    if modeldir.exists() or modeldir.is_symlink():
        raise FileExistsError("private model directory exists; refuse overwrite")
    if args.sampling == "episode-source-outcome":
        admit_balanced_sources(root, manifest)
    base = [sys.executable, str(repo / "ai/dmc/train_dmc.py")]
    train_files = file_list(root, manifest["splits"]["train"])
    val_files = file_list(root, manifest["splits"]["validation"])
    test_files = file_list(root, manifest["splits"]["test"])
    # Do not poison an immutable --reuse corpus with an empty 'models'
    # directory if balanced-source admission, optimizer, zero-Q gate, or
    # independent held-out evaluation fails. Publish only after all succeed.
    staging = Path(tempfile.mkdtemp(prefix=".dmc-training-stage-", dir=root))
    try:
        check_private_dir(staging)
        checkpoint = staging / "dmc-unpromoted-v3.pt"
        training = json.loads(execute(base + [
            "--train", "--replay", *train_files,
            "--validation-replay", *val_files,
            "--objective", manifest["objective"], "--rules-version", "1",
            "--output", str(checkpoint), "--model-version", str(args.model_version),
            "--steps", str(args.steps),
            "--batch-size", str(args.batch_size), "--learning-rate",
            str(args.learning_rate), "--device", args.device,
            "--seed", str(args.seed), *optimizer_policy_flags(args)
        ], cwd=repo))
        # Explicitly verify that the subprocess used the requested policy,
        # rather than silently accepting/reporting a different learner mode.
        optimizer = training["optimizer"]
        if optimizer["sampling"] != args.sampling or \
                optimizer["checkpoint_policy"] != args.checkpoint_policy or \
                optimizer["validation_selection_weighting"] != (
                    "episode-source-outcome" if args.sampling == "episode-source-outcome"
                    else "transition") or \
                optimizer["validation_zero_baseline_gate"] is not bool(
                    args.require_validation_advantage):
            raise ValueError("learner policy report does not match selected experiment")
        held_out = json.loads(execute(base + [
            "--evaluate-only", "--replay", *test_files,
            "--objective", manifest["objective"], "--rules-version", "1",
            "--checkpoint", str(checkpoint),
            "--model-version", str(args.model_version),
            "--device", args.device, "--batch-size", str(args.batch_size)
        ], cwd=repo))
        digest = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
        if optimizer["checkpoint_sha256"] != digest:
            raise ValueError("checkpoint digest changed between fitting and test")
        # No model file is published before a successful independent test.
        # A hard link is immutable/no-clobber and remains in the same private
        # filesystem; unlinking the staging source leaves exactly one owner.
        modeldir.mkdir(mode=0o700, exist_ok=False)
        check_private_dir(modeldir)
        os.link(checkpoint, modeldir / checkpoint.name)
        dirfd = os.open(modeldir, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
        result = {"checkpoint_sha256": digest,
                  "validation_before": optimizer["validation_before"],
                  "validation_after": optimizer["validation_after"],
                  "independent_test": held_out["metrics"],
                  "sampling": optimizer["sampling"],
                  "checkpoint_policy": optimizer["checkpoint_policy"],
                  "validation_selection_weighting":
                      optimizer["validation_selection_weighting"],
                  "selected_validation_step_by_role":
                      optimizer["selected_validation_step_by_role"],
                  "validation_zero_baseline_gate":
                      optimizer["validation_zero_baseline_gate"],
                  "trained_policy_qualified": False}
        if args.sampling == "episode-source-outcome":
            result["validation_aligned_before"] = optimizer[
                "validation_aligned_before"]
            result["validation_aligned_after"] = optimizer[
                "validation_aligned_after"]
        return result
    finally:
        shutil.rmtree(staging)




TRIAL_SCHEMA = "mygames-private-dmc-trial-v1"
HOLDOUT_SCHEMA = "mygames-private-dmc-heldout-comparison-v1"
TRIAL_NAME = re.compile(r"[a-z][a-z0-9-]{0,31}")


def valid_trial_name(name: str) -> str:
    if not isinstance(name, str) or TRIAL_NAME.fullmatch(name) is None or \
            name.endswith("-"):
        raise ValueError("trial names must be bounded lowercase identifiers")
    return name


def private_regular_file(path: Path, max_bytes: int) -> bytes:
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or \
            (stat.S_IMODE(info.st_mode) & 0o077) or info.st_nlink != 1 or \
            not (0 < info.st_size <= max_bytes):
        raise PermissionError("private trial file must be owner-only regular/one-link")
    return path.read_bytes()


def trial_registry(root: Path, *, create: bool) -> Path:
    registry = root / "trials"
    if not registry.exists() and not registry.is_symlink() and create:
        registry.mkdir(mode=0o700, exist_ok=False)
    check_private_dir(registry)
    # Only COMPLETE published trials are accepted. Partial directories
    # after a crash are blockers, not inputs silently repaired/reused.
    entries = list(registry.iterdir())
    if len(entries) > 16:
        raise ValueError("too many private candidate trials")
    for trial in entries:
        valid_trial_name(trial.name)
        check_private_dir(trial)
        if {p.name for p in trial.iterdir()} != {
                "model-v3.pt", "trial.v1.json"}:
            raise ValueError("incomplete/unexpected frozen trial contents")
    return registry


def read_frozen_trial(registry: Path, name: str, root: Path,
                      manifest: dict, args) -> tuple[dict, Path]:
    valid_trial_name(name)
    directory = registry / name
    check_private_dir(directory)
    if {p.name for p in directory.iterdir()} != {
            "model-v3.pt", "trial.v1.json"}:
        raise ValueError("unsealed private DMC trial")
    raw = private_regular_file(directory / "trial.v1.json", 64 * 1024)
    meta = json.loads(raw)
    checkpoint = directory / "model-v3.pt"
    content = private_regular_file(checkpoint, 160 * 1024 * 1024)
    if not isinstance(meta, dict) or meta.get("schema") != TRIAL_SCHEMA or \
            meta.get("trial") != name or \
            meta.get("source_revision") != args.source_revision or \
            meta.get("learner_revision") != (
                args.learner_revision or args.source_revision) or \
            meta.get("corpus_manifest_sha256") != hashlib.sha256(
                (root / "manifest.v1.json").read_bytes()).hexdigest() or \
            meta.get("native_generator_sha256") != (
                manifest["native_generator"]["sha256"]) or \
            meta.get("objective") != args.objective or \
            type(meta.get("model_version")) is not int or \
            not 1 < meta["model_version"] < 2**64 or \
            meta.get("checkpoint_sha256") != hashlib.sha256(content).hexdigest() or \
            meta.get("heldout_evaluated") is not False:
        raise ValueError("trial provenance/checkpoint digest mismatch")
    return meta, checkpoint


def train_frozen_trial(root: Path, manifest: dict, repo: Path,
                       args) -> dict:
    """Fit and freeze ONE candidate using TRAIN+VALIDATION ONLY.

    The genuine independent TEST Replay is verified for provenance by the
    outer reader, but NEVER scored against candidate weights here.
    """
    name = valid_trial_name(args.trial)
    if any((root / filename).exists() or (root / filename).is_symlink()
           for filename in ("models", "training.v1.json", "holdout-lock.v1.json",
                            "holdout.v1.json")):
        raise ValueError("heldout/legacy model already observed or published")
    if args.sampling == "episode-source-outcome":
        admit_balanced_sources(root, manifest)
    registry = trial_registry(root, create=True)
    target = registry / name
    if target.exists() or target.is_symlink():
        raise FileExistsError("immutable trial already exists")
    train_files = file_list(root, manifest["splits"]["train"])
    val_files = file_list(root, manifest["splits"]["validation"])
    base = [sys.executable, str(repo / "ai/dmc/train_dmc.py")]
    stage = Path(tempfile.mkdtemp(prefix=".dmc-trial-stage-", dir=root))
    try:
        check_private_dir(stage)
        candidate = stage / "model-v3.pt"
        training = json.loads(execute(base + [
            "--train", "--replay", *train_files,
            "--validation-replay", *val_files,
            "--objective", manifest["objective"], "--rules-version", "1",
            "--output", str(candidate), "--model-version",
            str(args.model_version), "--steps", str(args.steps),
            "--batch-size", str(args.batch_size), "--learning-rate",
            str(args.learning_rate), "--device", args.device,
            "--seed", str(args.seed), *optimizer_policy_flags(args)
        ], cwd=repo))
        opt = training["optimizer"]
        if opt["sampling"] != args.sampling or \
                opt["checkpoint_policy"] != args.checkpoint_policy or \
                opt["validation_selection_weighting"] != (
                    "episode-source-outcome" if args.sampling == "episode-source-outcome"
                    else "transition") or \
                opt["validation_zero_baseline_gate"] is not bool(
                    args.require_validation_advantage) or \
                opt["candidate_model_version"] != args.model_version:
            raise ValueError("trial optimizer policy / model version mismatch")
        digest = hashlib.sha256(
            private_regular_file(candidate, 160 * 1024 * 1024)).hexdigest()
        if digest != opt["checkpoint_sha256"]:
            raise ValueError("learner checkpoint digest changed before freeze")
        record = {
            "schema": TRIAL_SCHEMA, "trial": name,
            "source_revision": args.source_revision,
            "learner_revision": args.learner_revision or args.source_revision,
            "native_generator_sha256": manifest["native_generator"]["sha256"],
            "corpus_manifest_sha256": hashlib.sha256(
                (root / "manifest.v1.json").read_bytes()).hexdigest(),
            "objective": args.objective, "model_version": args.model_version,
            "checkpoint_sha256": digest,
            "sampling": opt["sampling"],
            "checkpoint_policy": opt["checkpoint_policy"],
            "validation_selection_weighting": opt["validation_selection_weighting"],
            "selected_validation_step_by_role": opt["selected_validation_step_by_role"],
            "validation_before": opt["validation_before"],
            "validation_after": opt["validation_after"],
            "validation_zero_baseline_gate": opt["validation_zero_baseline_gate"],
            "training_steps": args.steps, "seed": args.seed,
            "heldout_evaluated": False, "trained_policy_qualified": False,
        }
        if args.sampling == "episode-source-outcome":
            record["validation_aligned_before"] = opt["validation_aligned_before"]
            record["validation_aligned_after"] = opt["validation_aligned_after"]
        # Only after training + validation + checkpoint digest have PASSED do
        # we create a visible trial directory; its manifest is published LAST.
        target.mkdir(mode=0o700, exist_ok=False)
        check_private_dir(target)
        os.link(candidate, target / "model-v3.pt")  # no-clobber, same device
        no_clobber_json(target, "trial.v1.json", record)
        return {"trial": name, "checkpoint_sha256": digest,
                "heldout_scored": False,
                "validation_selection_weighting":
                    opt["validation_selection_weighting"],
                "trained_policy_qualified": False}
    finally:
        shutil.rmtree(stage)


def evaluate_frozen_trials(root: Path, manifest: dict, repo: Path,
                           args) -> dict:
    """Consume held-out TEST ONCE for TWO already frozen candidate models.

    Once holdout-lock exists, a failed/crashed comparison still counts as
    observed; do not permit retrials/retraining on the same TEST episodes.
    """
    names = args.evaluate_trials
    if len(names) != 2 or len(set(names)) != 2:
        raise ValueError("exactly two distinct frozen trials required")
    if any((root / filename).exists() or (root / filename).is_symlink()
           for filename in ("holdout-lock.v1.json", "holdout.v1.json",
                            "models", "training.v1.json")):
        raise FileExistsError("TEST already consumed or legacy model published")
    registry = trial_registry(root, create=False)
    entries = [read_frozen_trial(registry, name, root, manifest, args)
               for name in names]
    if entries[0][0]["model_version"] == entries[1][0]["model_version"]:
        raise ValueError("distinct candidates must have distinct model identities")
    # Freeze the exact comparison set and digests BEFORE any held-out Q
    # evaluation. This marker is intentionally not rolled back on failure.
    lock = {"schema": HOLDOUT_SCHEMA, "status": "consumed",
            "trial_names": names,
            "trial_sha256": [entry["checkpoint_sha256"] for entry, _ in entries],
            "corpus_manifest_sha256": hashlib.sha256(
                (root / "manifest.v1.json").read_bytes()).hexdigest(),
            "learner_revision": args.learner_revision or args.source_revision}
    no_clobber_json(root, "holdout-lock.v1.json", lock)
    test_files = file_list(root, manifest["splits"]["test"])
    base = [sys.executable, str(repo / "ai/dmc/train_dmc.py")]
    # One Torch process scores BOTH frozen models on each identical complete
    # C11 Replay episode; only per-role, cluster-aggregated MSE leaves it.
    # No individual episode IDs, private hands or action features are logged.
    compared = json.loads(execute(base + [
        "--compare-frozen", "--replay", *test_files,
        "--objective", manifest["objective"], "--rules-version", "1",
        "--checkpoint", str(entries[0][1]),
        "--model-version", str(entries[0][0]["model_version"]),
        "--compare-checkpoint", str(entries[1][1]),
        "--compare-model-version", str(entries[1][0]["model_version"]),
        "--device", args.device, "--batch-size", str(args.batch_size)
    ], cwd=repo))
    if compared.get("candidate_model_versions") != [
            entry[0]["model_version"] for entry in entries]:
        raise ValueError("paired evaluator candidate model identity mismatch")
    paired = compared["paired_complete_game_mse"]
    if paired["independent_complete_games"] != len(test_files) or \
            paired["trained_gameplay_skill_qualified"] is not False:
        raise ValueError("paired evaluator game-count or qualification mismatch")
    results = {names[0]: {
        role: {"mse": scores["a_transition_mse"]}
        for role, scores in paired["roles"].items()},
        names[1]: {
        role: {"mse": scores["b_transition_mse"]}
        for role, scores in paired["roles"].items()}}
    record = {"schema": HOLDOUT_SCHEMA,
              "source_revision": args.source_revision,
              "learner_revision": args.learner_revision or args.source_revision,
              "corpus_manifest_sha256": lock["corpus_manifest_sha256"],
              "trial_names": names, "results": results,
              "paired_complete_game_mse": paired,
              "independent_return_q_only": True,
              "heldout_now_observed": True,
              "trained_policy_qualified": False}
    no_clobber_json(root, "holdout.v1.json", record)
    return {"heldout_comparison_sealed": True,
            "trials": names, "results": results,
            "paired_complete_game_mse": paired,
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
    parser.add_argument("--source-checkout",
                        help="existing ORIGINAL exact-source Git checkout; --reuse only")
    parser.add_argument("--learner-revision",
                        help="exact CURRENT trainer Git HEAD; required with --source-checkout")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--sampling",
                        choices=("transition", "episode-source-outcome"),
                        default="transition")
    parser.add_argument("--checkpoint-policy",
                        choices=("final", "best-validation"), default="final")
    parser.add_argument("--validation-interval", type=int, default=20)
    parser.add_argument("--require-validation-advantage", action="store_true")
    parser.add_argument("--trial",
                        help="optional immutable TRAIN+VALIDATION only candidate name")
    parser.add_argument("--evaluate-trials", nargs=2, metavar=("TRIAL_A", "TRIAL_B"),
                        help="consume held-out TEST once for two frozen candidates")
    args = parser.parse_args()
    if args.trial and (not args.train or not args.reuse or args.evaluate_trials):
        parser.error("--trial requires --reuse --train and forbids --evaluate-trials")
    if args.evaluate_trials and (args.train or not args.reuse or
                                 args.trial or args.model_version is not None):
        parser.error("--evaluate-trials requires --reuse and forbids --train/model-version")
    if args.trial:
        valid_trial_name(args.trial)
    if args.evaluate_trials:
        for name in args.evaluate_trials:
            valid_trial_name(name)
        if len(set(args.evaluate_trials)) != 2:
            parser.error("--evaluate-trials needs two different trial names")
    if args.steps < 1 or args.batch_size < 1 or args.seed < 0 or \
            not 0 < args.learning_rate <= 1 or args.validation_interval < 1:
        parser.error("invalid optimizer configuration")
    if not args.train and (args.sampling != "transition" or \
                           args.checkpoint_policy != "final" or \
                           args.validation_interval != 20 or \
                           args.require_validation_advantage):
        parser.error("optimizer policy flags require --train")
    if args.train and (args.model_version is None or
                       not 1 < args.model_version < 2**64):
        parser.error("--train requires --model-version > native source model 1")
    if not args.train and args.model_version is not None:
        parser.error("--model-version is only valid with --train")
    if bool(args.source_checkout) != bool(args.learner_revision):
        parser.error("--source-checkout and --learner-revision must be supplied together")
    if args.source_checkout and not args.reuse:
        parser.error("--source-checkout is for immutable --reuse only; no new games")
    repo = Path(__file__).resolve().parents[2]
    if args.learner_revision:
        # Explicitly bind new learner code to its own exact checkout HEAD.
        # A declared source revision is NOT silently treated as learner code.
        source_revision(repo, args.learner_revision)
    manifest = (verify_existing_manifest(args, repo) if args.reuse
                else build_experiment(args, repo))
    root = Path(args.private_root).absolute()
    verified = verify_reader(root, manifest, repo)
    report = {"schema": SCHEMA, "source_revision": args.source_revision,
              "learner_revision": args.learner_revision or args.source_revision,
              "cross_revision_reuse": bool(args.source_checkout and
                    args.learner_revision != args.source_revision),
              "objective": args.objective, "verified": verified,
              "reused_existing_data": args.reuse,
              "torch_optimization_executed": args.train}
    if args.evaluate_trials:
        report["heldout"] = evaluate_frozen_trials(root, manifest, repo, args)
    elif args.trial:
        report["trial"] = train_frozen_trial(root, manifest, repo, args)
    elif args.train:
        if ((root / "models").exists() or (root / "training.v1.json").exists() or
                (root / "trials").exists() or (root / "holdout-lock.v1.json").exists()):
            raise FileExistsError("legacy model conflicts with trial/holdout registry")
        report["model"] = train_and_test(root, manifest, repo, args)
    if not args.reuse:
        # Source manifest is admitted only AFTER all Replay partitions
        # pass; never create a false complete run after a failed job.
        no_clobber_json(root, "manifest.v1.json", manifest)
    if args.train and not args.trial:
        # Legacy single-run training retains its original private marker.
        no_clobber_json(root, "training.v1.json", report)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
