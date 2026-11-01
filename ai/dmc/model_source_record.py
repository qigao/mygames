"""Immutable, private evidence for a verified multi-behavior DMC checkpoint.

This is an owner-local *source provenance record*, not a signed artifact or
permission to deploy the bot. A new R9 model is unadmitted when this record is
missing, mismatched, unreadable or incomplete. Never publish private weights.
"""
from __future__ import annotations

import hashlib
import json
from math import isfinite
import os
from pathlib import Path
import re
import stat
import tempfile

from verify_behavior_sources import BEHAVIORS, HEX40, HEX64

SCHEMA = "mygames-dmc-verified-sources-v1"
ROLES = ("landlord", "landlord_up", "landlord_down")
KEYS = frozenset((
    "schema", "candidate_model_version", "checkpoint_sha256",
    "source_revision", "learner_revision", "native_generator_sha256",
    "source_manifest_bundle_sha256", "verified_behavior_sources",
    "source_model_versions", "rules_version", "objective",
    "training_complete_games", "tuning_complete_games",
    "selected_validation_step_by_role", "validation_aligned_mse_by_role",
    "model_promoted", "provenance_signed",
))


def _private(path: Path, *, directory: bool):
    item = path.lstat()
    if ((not stat.S_ISDIR(item.st_mode) if directory else
         not stat.S_ISREG(item.st_mode)) or
            item.st_uid != os.geteuid() or
            stat.S_IMODE(item.st_mode) & 0o077 or
            (not directory and item.st_nlink != 1)):
        raise PermissionError("private source record/checkpoint must be owner-only")
    return item


def _sha(value) -> bool:
    return isinstance(value, str) and HEX64.fullmatch(value) is not None


def record_path(checkpoint: str | Path) -> Path:
    model = Path(checkpoint)
    return model.with_name(model.name + ".sources.v1.json")


def _admit_content(record: dict):
    if not isinstance(record, dict) or set(record) != KEYS or \
            record["schema"] != SCHEMA or \
            record["source_model_versions"] != [1] or \
            type(record["rules_version"]) is not int or \
            record["rules_version"] != 1 or record["objective"] != "win-sign" or \
            type(record["candidate_model_version"]) is not int or \
            not 1 < record["candidate_model_version"] < 2**64 or \
            record["model_promoted"] is not False or \
            record["provenance_signed"] is not False:
        raise ValueError("invalid model-source record schema/ABI/promotion")
    for field in ("source_revision", "learner_revision"):
        value = record[field]
        if not isinstance(value, str) or HEX40.fullmatch(value) is None:
            raise ValueError("invalid canonical source or learner commit SHA")
    for field in ("checkpoint_sha256", "native_generator_sha256",
                  "source_manifest_bundle_sha256"):
        if not _sha(record[field]):
            raise ValueError("invalid immutable source/model digest")
    sources = record["verified_behavior_sources"]
    if not isinstance(sources, list) or not 2 <= len(sources) <= 3 or \
            any(type(x) is not str for x in sources) or \
            sources != sorted(set(sources)) or not set(sources).issubset(BEHAVIORS):
        raise ValueError("model missing verified distinct policy identities")
    for count in ("training_complete_games", "tuning_complete_games"):
        value = record[count]
        if type(value) is not int or value < len(sources) or value > 288:
            raise ValueError("invalid bounded complete-game source counts")
    steps = record["selected_validation_step_by_role"]
    metrics = record["validation_aligned_mse_by_role"]
    if not isinstance(steps, dict) or set(steps) != set(ROLES) or \
            not isinstance(metrics, dict) or set(metrics) != set(ROLES):
        raise ValueError("incomplete selected role-specific checkpoints/metrics")
    for role in ROLES:
        if type(steps[role]) is not int or not 1 <= steps[role] <= 1000000:
            raise ValueError("invalid selected role update step")
        row = metrics[role]
        if not isinstance(row, dict) or set(row) != {
                "mse", "zero_q_mse"}:
            raise ValueError("missing aligned Q/zero-Q baseline comparison")
        mse, baseline = row["mse"], row["zero_q_mse"]
        if (type(mse) not in (int, float) or
                type(baseline) not in (int, float) or
                not isfinite(mse) or not isfinite(baseline) or
                mse < 0 or baseline <= 0 or mse >= baseline):
            raise ValueError("one role failed aligned zero-Q publication gate")


def _model_sha(checkpoint: Path) -> str:
    item = _private(checkpoint, directory=False)
    if not 0 < item.st_size <= 160 * 1024 * 1024:
        raise ValueError("private frozen checkpoint size exceeds v3 bound")
    return hashlib.sha256(checkpoint.read_bytes()).hexdigest()


def make_record(*, checkpoint_sha256, model_version, source_revision,
                learner_revision, generator_sha256, source_bundle_sha256,
                sources, train_count, tuning_count, steps_by_role,
                aligned_by_role) -> dict:
    return {
        "schema": SCHEMA, "candidate_model_version": model_version,
        "checkpoint_sha256": checkpoint_sha256,
        "source_revision": source_revision, "learner_revision": learner_revision,
        "native_generator_sha256": generator_sha256,
        "source_manifest_bundle_sha256": source_bundle_sha256,
        "verified_behavior_sources": sorted(sources),
        "source_model_versions": [1], "rules_version": 1,
        "objective": "win-sign", "training_complete_games": train_count,
        "tuning_complete_games": tuning_count,
        "selected_validation_step_by_role": dict(steps_by_role),
        "validation_aligned_mse_by_role": {
            role: {"mse": aligned_by_role[role]["mse"],
                   "zero_q_mse": aligned_by_role[role]["zero_q_mse"]}
            for role in ROLES},
        "model_promoted": False, "provenance_signed": False,
    }


def verify_record(checkpoint: str | Path, *, expected_model_version: int,
                  expected_bundle_sha256: str | None = None):
    model = Path(checkpoint).absolute()
    _private(model.parent, directory=True)
    actual_checkpoint_sha = _model_sha(model)
    marker = record_path(model)
    item = _private(marker, directory=False)
    if not 0 < item.st_size <= 16384:
        raise ValueError("private frozen-model source record exceeds size limit")
    raw = marker.read_bytes()
    record = json.loads(raw.decode("utf-8"),
                        parse_constant=lambda _: (_ for _ in ()).throw(
                            ValueError("nonfinite source manifest")))
    _admit_content(record)
    if (record["checkpoint_sha256"] != actual_checkpoint_sha or
            record["candidate_model_version"] != expected_model_version or
            (expected_bundle_sha256 is not None and
             record["source_manifest_bundle_sha256"] != expected_bundle_sha256)):
        raise ValueError("immutable checkpoint and verified policy sources disagree")
    return record


def publish_record(checkpoint: str | Path, record: dict) -> dict:
    model = Path(checkpoint).absolute()
    _private(model.parent, directory=True)
    _admit_content(record)
    if record["checkpoint_sha256"] != _model_sha(model):
        raise ValueError("frozen model digest does not match source record")
    marker = record_path(model)
    if marker.exists() or marker.is_symlink():
        raise FileExistsError("source evidence already exists; immutable no-clobber")
    raw = (json.dumps(record, sort_keys=True, separators=(",", ":"),
                      allow_nan=False) + "\n").encode("utf-8")
    if len(raw) > 16384:
        raise ValueError("source record length exceeds bound")
    fd, temporary = tempfile.mkstemp(dir=model.parent, prefix=".dmc-source-")
    try:
        with os.fdopen(fd, "wb") as dest:
            dest.write(raw)
            dest.flush()
            os.fsync(dest.fileno())
        os.link(temporary, marker)  # publish the complete record, no overwrite
        dirfd = os.open(model.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dirfd)
        finally:
            os.close(dirfd)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    checked = verify_record(model,
                            expected_model_version=record["candidate_model_version"],
                            expected_bundle_sha256=record[
                                "source_manifest_bundle_sha256"])
    if checked != record:
        raise RuntimeError("source record differs after atomic publication")
    return {
        "source_record_sha256": hashlib.sha256(raw).hexdigest(),
        "source_record_verified": True,
        "qualified_as_production_bot": False,
    }
