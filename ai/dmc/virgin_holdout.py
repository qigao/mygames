#!/usr/bin/env python3
"""One-shot, privately sealed R9 DouDizhu native paired-deal skill evaluation.

Only explicitly frozen model and source-verified records are admitted. The
independent TEST shuffle schedule is created *after* freezing, stays 0600 and
cannot be evaluated twice. A durable consumption marker is committed BEFORE
native C11 matches or PyTorch inference. Never send seed/deck/weight data to
CI artifacts, stdout, network services or a public Game Owner.

Model source SHA-256 is provenance, not a cryptographic build attestation.
The novelty check excludes known public R5 schedules and duplicates in this
plan, not unknown historical private runs. This test is offline/experimental.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import stat
import tempfile
from types import SimpleNamespace

from model_source_record import record_path, verify_record
from tournament import evaluate, validate_fresh_deal_seeds, schedule_seeds
from verify_behavior_sources import HEX64

PLAN = "r9-holdout-plan.v1.json"
LOCK = "r9-holdout-lock.v1.json"
RESULT = "r9-holdout-result.v1.json"
CLAIM_SUFFIX = ".r9-final-test-claim.v1.json"
SCHEMA = "mygames-private-r9-fresh-paired-plan-v1"
PLAN_KEYS = frozenset((
    "schema", "checkpoint_sha256", "source_record_sha256",
    "model_version", "generator_source_manifest_sha256",
    "native_tournament_sha256", "rules_version", "objective",
    "baseline", "seed_clusters", "identity_clusters", "seeds",
    "confidence", "model_frozen_before_schedule", "nonce",
))
BASELINE = "shed-most"
OLD_DEALS = set(schedule_seeds(256, "development")) | set(
    schedule_seeds(256, "holdout"))


def private_path(path: Path, *, directory: bool) -> os.stat_result:
    item = path.lstat()
    if ((not stat.S_ISDIR(item.st_mode) if directory else
         not stat.S_ISREG(item.st_mode)) or
            item.st_uid != os.geteuid() or
            stat.S_IMODE(item.st_mode) & 0o077 or
            (not directory and item.st_nlink != 1)):
        raise PermissionError("private evaluation files require owner-only ACL and no links")
    return item


def private_sha(path: Path, *, max_bytes: int) -> str:
    meta = private_path(path, directory=False)
    if not 0 < meta.st_size <= max_bytes:
        raise ValueError("private evaluation artifact missing or outside size bound")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def claim_path(checkpoint: Path) -> Path:
    model = checkpoint.absolute()
    return model.with_name(model.name + CLAIM_SUFFIX)


def private_root_digest(root: Path) -> str:
    # Bind each one-shot test to the particular owner-private directory.
    # Replaying/copied plan bytes under a second directory cannot qualify.
    return hashlib.sha256(str(root.resolve(strict=True)).encode("utf-8")).hexdigest()


def canonical_json(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":"),
                       allow_nan=False) + "\n").encode("utf-8")


def write_once(root: Path, filename: str, document: dict) -> str:
    private_path(root, directory=True)
    if (filename not in (PLAN, LOCK, RESULT) and
            (not filename.endswith(CLAIM_SUFFIX) or
             Path(filename).name != filename)):
        raise ValueError("invalid R9 fixed completion file name")
    dst = root / filename
    if dst.exists() or dst.is_symlink():
        raise FileExistsError("R9 holdout record already exists; refuse overwrite")
    raw = canonical_json(document)
    if len(raw) > 32768:
        raise ValueError("R9 private holdout record exceeds strict bound")
    fd, tmp = tempfile.mkstemp(prefix=".r9-private-", dir=root)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.link(tmp, dst)
        dirfd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)
    return hashlib.sha256(raw).hexdigest()


def checked_model_and_runtime(checkpoint: Path, native_library: Path,
                              model_version: int):
    if type(model_version) is not int or not 2 <= model_version < 2**64:
        raise ValueError("explicit immutable candidate model version required")
    checkpoint = checkpoint.absolute()
    native_library = native_library.absolute()
    record = verify_record(checkpoint, expected_model_version=model_version)
    if (record["model_promoted"] is not False or
            record["source_model_versions"] != [1] or
            record["objective"] != "win-sign" or
            record["rules_version"] != 1):
        raise ValueError("selected frozen model is not an eligible R9 candidate")
    source_digest = private_sha(record_path(checkpoint), max_bytes=16384)
    native_digest = private_sha(native_library, max_bytes=256 * 1024 * 1024)
    return record, source_digest, native_digest


def new_deals(count: int):
    if type(count) is not int or not 30 <= count <= 256:
        raise ValueError("R9 requires precommitted 30..256 independent deal clusters")
    deals = set()
    for _ in range(count * 64):
        value = secrets.randbelow(2**32 - 1) + 1
        if value not in OLD_DEALS:
            deals.add(value)
        if len(deals) == count:
            result = list(deals)
            secrets.SystemRandom().shuffle(result)
            return validate_fresh_deal_seeds(result, count)
    raise RuntimeError("unable to draw fresh unique TEST-only deck seeds")


def validate_plan(data: dict):
    if not isinstance(data, dict) or set(data) != PLAN_KEYS or \
            data["schema"] != SCHEMA or \
            data["objective"] != "win-sign" or \
            data["baseline"] != BASELINE or \
            data["confidence"] != "4000-whole-deal-bootstrap-95" or \
            data["model_frozen_before_schedule"] is not True or \
            type(data["rules_version"]) is not int or \
            data["rules_version"] != 1 or \
            type(data["model_version"]) is not int or \
            not 2 <= data["model_version"] < 2**64 or \
            type(data["identity_clusters"]) is not int or \
            not 1 <= data["identity_clusters"] <= data["seed_clusters"] or \
            not isinstance(data["nonce"], str) or \
            len(data["nonce"]) != 32 or \
            any(ch not in "0123456789abcdef" for ch in data["nonce"]):
        raise ValueError("incorrect R9 fresh TEST plan schema/threshold")
    for name in ("checkpoint_sha256", "source_record_sha256",
                 "generator_source_manifest_sha256", "native_tournament_sha256"):
        if not isinstance(data[name], str) or HEX64.fullmatch(data[name]) is None:
            raise ValueError("missing selected immutable model/source digest")
    validate_fresh_deal_seeds(data["seeds"], data["seed_clusters"])


def read_plan(root: Path) -> tuple[dict, str]:
    private_path(root, directory=True)
    path = root / PLAN
    sha = private_sha(path, max_bytes=32768)
    data = json.loads(path.read_text(encoding="utf-8"),
                      parse_constant=lambda _: (_ for _ in ()).throw(
                          ValueError("nonfinite JSON in private holdout plan")))
    validate_plan(data)
    return data, sha


def prepare(*, root: Path, checkpoint: Path, native_library: Path,
            model_version: int, seed_clusters: int, identity_clusters: int):
    root = root.absolute()
    private_path(root, directory=True)
    if any(root.iterdir()):
        raise FileExistsError("fresh R9 evaluation directory must start EMPTY")
    record, source_sha, native_sha = checked_model_and_runtime(
        checkpoint, native_library, model_version)
    model_claim = claim_path(checkpoint)
    if model_claim.exists() or model_claim.is_symlink():
        raise FileExistsError("this immutable model has already reserved its FINAL test")
    if type(identity_clusters) is not int or not 1 <= identity_clusters <= seed_clusters:
        raise ValueError("identity-null cluster count invalid")
    seeds = new_deals(seed_clusters)
    plan = {
        "schema": SCHEMA, "checkpoint_sha256": record["checkpoint_sha256"],
        "source_record_sha256": source_sha, "model_version": model_version,
        "generator_source_manifest_sha256": record["source_manifest_bundle_sha256"],
        "native_tournament_sha256": native_sha, "rules_version": 1,
        "objective": "win-sign", "baseline": BASELINE,
        "seed_clusters": seed_clusters, "identity_clusters": identity_clusters,
        "seeds": list(seeds), "confidence": "4000-whole-deal-bootstrap-95",
        "model_frozen_before_schedule": True,
        "nonce": secrets.token_hex(16),
    }
    validate_plan(plan)
    digest = hashlib.sha256(canonical_json(plan)).hexdigest()
    # The checkpoint-local claim is committed FIRST. A crash after claim but
    # before the plan publication fails closed and permanently reserves this
    # frozen candidate. Never allow a second test plan for the same file.
    write_once(model_claim.parent, model_claim.name, {
        "schema": "mygames-private-r9-model-final-test-claim-v1",
        "checkpoint_sha256": record["checkpoint_sha256"],
        "source_record_sha256": source_sha,
        "native_tournament_sha256": native_sha,
        "plan_sha256": digest,
        "private_root_sha256": private_root_digest(root),
        "model_version": model_version,
        "baseline": BASELINE,
    })
    if write_once(root, PLAN, plan) != digest:
        raise RuntimeError("private R9 plan publication differs from immutable model claim")
    return {"status": "prepared-not-consumed", "plan_sha256": digest,
            "deal_clusters": seed_clusters, "paired_matches": seed_clusters * 6,
            "checkpoint_sha256": record["checkpoint_sha256"],
            "tested_game_skill": False, "production_bot_promoted": False}


def admit_and_lock(*, root: Path, checkpoint: Path, native_library: Path,
                   model_version: int):
    """One-shot consumption lock always precedes the first actual TEST match."""
    root = root.absolute()
    plan, plan_digest = read_plan(root)
    if (root / LOCK).exists() or (root / LOCK).is_symlink() or \
            (root / RESULT).exists() or (root / RESULT).is_symlink():
        raise FileExistsError("R9 TEST has already been consumed or completed")
    record, source_sha, native_sha = checked_model_and_runtime(
        checkpoint, native_library, model_version)
    model_claim = claim_path(checkpoint)
    item = private_path(model_claim, directory=False)
    if not 0 < item.st_size <= 16384:
        raise ValueError("immutable model-level R9 test reservation missing or oversized")
    claim = json.loads(model_claim.read_text(encoding="utf-8"),
                       parse_constant=lambda _: (_ for _ in ()).throw(
                           ValueError("nonfinite model final-test claim")))
    if (not isinstance(claim, dict) or
            set(claim) != {
                "schema", "checkpoint_sha256", "source_record_sha256",
                "native_tournament_sha256", "plan_sha256",
                "private_root_sha256", "model_version", "baseline"} or
            claim["schema"] != "mygames-private-r9-model-final-test-claim-v1" or
            claim["checkpoint_sha256"] != record["checkpoint_sha256"] or
            claim["source_record_sha256"] != source_sha or
            claim["native_tournament_sha256"] != native_sha or
            claim["plan_sha256"] != plan_digest or
            claim["private_root_sha256"] != private_root_digest(root) or
            type(claim["model_version"]) is not int or
            claim["model_version"] != model_version or
            claim["baseline"] != BASELINE):
        raise ValueError("plan is not the model's sole registered FINAL holdout")
    if (model_version != plan["model_version"] or
            record["checkpoint_sha256"] != plan["checkpoint_sha256"] or
            source_sha != plan["source_record_sha256"] or
            native_sha != plan["native_tournament_sha256"] or
            record["source_manifest_bundle_sha256"] !=
            plan["generator_source_manifest_sha256"]):
        raise ValueError("frozen R9 model/source/runtime changed since TEST plan creation")
    if set(p.name for p in root.iterdir()) != {PLAN}:
        raise ValueError("private holdout folder contains unexpected uncommitted files")
    write_once(root, LOCK, {
        "schema": "mygames-private-r9-test-consumption-v1",
        "plan_sha256": plan_digest,
        "checkpoint_sha256": record["checkpoint_sha256"],
        "model_version": model_version,
        "consumed_before_first_test_match": True,
        "time_utc": datetime.now(timezone.utc).isoformat(),
    })
    return plan, plan_digest


def qualification(report: dict, *, expected_clusters: int,
                  identity_clusters: int) -> dict:
    """Strict frozen-model shed-most skill gate, not production promotion."""
    if (not isinstance(report, dict) or
            report.get("baseline") != BASELINE or
            report.get("test_seed_schedule") !=
                "sealed-r9-frozen-model-one-shot" or
            type(report.get("score_failures")) is not int or
            report["score_failures"] != 0 or
            type(report.get("independent_shuffle_clusters")) is not int or
            report["independent_shuffle_clusters"] != expected_clusters or
            type(report.get("paired_complete_matches")) is not int or
            report["paired_complete_matches"] != expected_clusters * 6 or
            type(report.get("identity_null_completed_matches")) is not int or
            report["identity_null_completed_matches"] != identity_clusters * 6):
        raise ValueError("incomplete or unqualified paired native C11 match report")
    win, points = (report.get("cluster_bootstrap_95_win_rate"),
                   report.get("cluster_bootstrap_95_points"))
    if (not isinstance(win, list) or len(win) != 2 or
            not isinstance(points, list) or len(points) != 2 or
            any(type(v) not in (int, float) or not math.isfinite(v)
                for v in win + points) or
            not 0 <= win[0] <= win[1] <= 1 or
            points[0] > points[1]):
        raise ValueError("missing/nonfinite whole-deal 95% bootstrap intervals")
    return {
        "strong_baseline": BASELINE,
        "cluster_bootstrap_95_lower_win_rate": win[0],
        "cluster_bootstrap_95_lower_points_per_game": points[0],
        "frozen_model_skill_gate_passed": win[0] > 0.5 and points[0] > 0,
        "production_bot_promoted": False,
    }


def evaluate_once(*, root: Path, checkpoint: Path, native_library: Path,
                  model_version: int):
    plan, plan_sha = admit_and_lock(
        root=root, checkpoint=checkpoint, native_library=native_library,
        model_version=model_version)
    # A crash, interrupted evaluation or missing Q after this line leaves the
    # private lock committed and the previously virgin TEST irreversibly used.
    config = SimpleNamespace(
        checkpoint=checkpoint, native_library=native_library,
        model_version=model_version, seed_clusters=plan["seed_clusters"],
        identity_clusters=plan["identity_clusters"],
        baseline=BASELINE, baseline_type=0, seed_schedule="development",
        include_aggregates=False,
    )
    report = evaluate(config, frozen_test_seeds=tuple(plan["seeds"]))
    result = qualification(
        report, expected_clusters=plan["seed_clusters"],
        identity_clusters=plan["identity_clusters"])
    summary = {
        "schema": "mygames-private-r9-frozen-skill-result-v1",
        "plan_sha256": plan_sha,
        "checkpoint_sha256": plan["checkpoint_sha256"],
        "model_version": model_version, "baseline": BASELINE,
        "independent_shuffle_clusters": report["independent_shuffle_clusters"],
        "paired_complete_matches": report["paired_complete_matches"],
        "identity_null_completed_matches":
            report["identity_null_completed_matches"],
        "candidate_win_rate": report["candidate_win_rate"],
        "candidate_mean_nonmonetary_points":
            report["candidate_mean_nonmonetary_points"],
        "cluster_bootstrap_95_win_rate":
            report["cluster_bootstrap_95_win_rate"],
        "cluster_bootstrap_95_points": report["cluster_bootstrap_95_points"],
        **result,
    }
    write_once(root.absolute(), RESULT, summary)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_subparsers(dest="mode", required=True)
    prepare_cmd = mode.add_parser("prepare")
    eval_cmd = mode.add_parser("evaluate")
    for cmd in (prepare_cmd, eval_cmd):
        cmd.add_argument("--private-dir", type=Path, required=True)
        cmd.add_argument("--checkpoint", type=Path, required=True)
        cmd.add_argument("--native-library", type=Path, required=True)
        cmd.add_argument("--model-version", type=int, required=True)
    prepare_cmd.add_argument("--seed-clusters", type=int, default=64)
    prepare_cmd.add_argument("--identity-clusters", type=int, default=2)
    args = parser.parse_args()
    if args.mode == "prepare":
        report = prepare(
            root=args.private_dir, checkpoint=args.checkpoint,
            native_library=args.native_library, model_version=args.model_version,
            seed_clusters=args.seed_clusters,
            identity_clusters=args.identity_clusters)
    else:
        report = evaluate_once(
            root=args.private_dir, checkpoint=args.checkpoint,
            native_library=args.native_library, model_version=args.model_version)
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
