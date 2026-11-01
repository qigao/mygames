"""Explicit, source-verified warm-start from genuine frozen-Q on-policy Replay.

This source contract preserves the old v3 checkpoint ABI but binds parent Q
weights, native C11 episode source, and optimizer lineage in a second private
no-clobber sidecar. No TEST pool, Game Owner, network, or fallback.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace

from replay import parse_episode
from train_policy import make_role_sampler
from verify_onpolicy import private_owned, verified_split

LINEAGE_SCHEMA = "mygames-private-dmc-onpolicy-warmstart-v1"
ROLES = ("landlord", "landlord_up", "landlord_down")


def _sha256(path: Path) -> str:
    private_owned(path, is_dir=False)
    return hashlib.sha256(path.read_bytes()).hexdigest()


def admit_warmstart_source(*, parent_checkpoint: Path,
                           parent_model_version: int,
                           native_library_sha256: str,
                           source_revision: str,
                           train_root: Path, validation_root: Path):
    """Admit exact genuine on-policy C11 TRAIN and TUNING bytes BEFORE Torch."""
    if (type(parent_model_version) is not int or
            not 2 <= parent_model_version < 2**64):
        raise ValueError("real positive frozen parent model version required")
    parent = parent_checkpoint.absolute()
    if not 0 < private_owned(parent, is_dir=False).st_size <= 160 * 1024 * 1024:
        raise ValueError("frozen parent checkpoint exceeds size bound")
    parent_sha = _sha256(parent)
    args = SimpleNamespace(
        source_revision=source_revision, checkpoint_sha256=parent_sha,
        native_library_sha256=native_library_sha256,
        behavior_model_version=parent_model_version,
    )
    train_root = train_root.absolute()
    validation_root = validation_root.absolute()
    if train_root.resolve(strict=True) == validation_root.resolve(strict=True):
        raise ValueError("TRAIN and TUNING on-policy source paths cannot overlap")
    ids, hashes, episode_groups, manifest_hashes = set(), set(), {}, {}
    for split, root in (("train", train_root), ("validation", validation_root)):
        _, local_ids, local_hashes = verified_split(root, args)
        if ids & local_ids or hashes & local_hashes:
            raise ValueError("on-policy TRAIN/TUNING complete games or bytes overlap")
        ids.update(local_ids)
        hashes.update(local_hashes)
        manifest = root / "onpolicy.v1.json"
        private_owned(manifest, is_dir=False)
        raw_manifest = manifest.read_bytes()
        meta = json.loads(raw_manifest)
        manifest_hashes[split] = hashlib.sha256(raw_manifest).hexdigest()
        rows = []
        for entry in meta["episodes"]:
            path = root / entry["file"]
            private_owned(path, is_dir=False)
            raw = path.read_bytes()
            if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
                raise ValueError("frozen model Replay changed after native source verification")
            episode = parse_episode(raw)
            if (episode.episode_id != entry["episode_id"] or
                    episode.model_version != parent_model_version or
                    episode.rules_version != 1 or episode.objective != 1 or
                    episode.landlord_seat != entry["landlord_seat"] or
                    episode.winner_seat != entry["winner_seat"]):
                raise ValueError("on-policy native source replay/manifest mismatch")
            rows.append(episode)
        rows.sort(key=lambda e: e.episode_id)
        if len(rows) != len(local_ids) or not rows:
            raise ValueError("incomplete authenticated on-policy snapshot")
        # Complete game + role + win/loss semantics before any allocation of Q
        # weights. A data source label is never inferred from another version.
        for role in range(3):
            make_role_sampler(rows, role)
        episode_groups[split] = tuple(rows)
    bundle_sha = hashlib.sha256(
        (f"train:{manifest_hashes['train']}\n"
         f"validation:{manifest_hashes['validation']}\n").encode("ascii")
    ).hexdigest()
    return {
        "parent_checkpoint_sha256": parent_sha,
        "parent_model_version": parent_model_version,
        "onpolicy_source_manifest_bundle_sha256": bundle_sha,
        "train_episodes": episode_groups["train"],
        "validation_episodes": episode_groups["validation"],
        "train_count": len(episode_groups["train"]),
        "validation_count": len(episode_groups["validation"]),
        "source_revision_claim": source_revision,
        "native_library_sha256": native_library_sha256,
        "virgin_test_opened": False,
        "qualifies_gameplay_skill": False,
    }


def lineage_path(checkpoint: Path) -> Path:
    return checkpoint.with_name(checkpoint.name + ".warmstart.v1.json")


def _record_valid(record: dict):
    if (not isinstance(record, dict) or
            set(record) != {
                "schema", "candidate_model_version", "candidate_checkpoint_sha256",
                "parent_model_version", "parent_checkpoint_sha256",
                "source_revision_claim", "native_library_sha256",
                "onpolicy_source_manifest_bundle_sha256", "train_count",
                "validation_count", "selected_step_by_role",
                "aligned_validation_mse_by_role", "virgin_test_opened",
                "gameplay_skill_qualified"} or
            record["schema"] != LINEAGE_SCHEMA or
            type(record["parent_model_version"]) is not int or
            type(record["candidate_model_version"]) is not int or
            not 2 <= record["parent_model_version"] <
                    record["candidate_model_version"] < 2**64 or
            record["virgin_test_opened"] is not False or
            record["gameplay_skill_qualified"] is not False):
        raise ValueError("invalid frozen on-policy model lineage schema/versions")
    for key in ("candidate_checkpoint_sha256", "parent_checkpoint_sha256",
                "native_library_sha256", "onpolicy_source_manifest_bundle_sha256"):
        value = record[key]
        if not isinstance(value, str) or len(value) != 64 or any(
                c not in "0123456789abcdef" for c in value):
            raise ValueError("noncanonical frozen model/source SHA256")
    value = record["source_revision_claim"]
    if not isinstance(value, str) or len(value) != 40 or any(
            c not in "0123456789abcdef" for c in value):
        raise ValueError("invalid exact C11 source revision claim")
    for key in ("train_count", "validation_count"):
        count = record[key]
        if type(count) is not int or not 1 <= count <= 96:
            raise ValueError("invalid bounded on-policy cohort size")
    steps = record["selected_step_by_role"]
    metrics = record["aligned_validation_mse_by_role"]
    if not isinstance(steps, dict) or set(steps) != set(ROLES) or (
            not isinstance(metrics, dict) or set(metrics) != set(ROLES)):
        raise ValueError("missing role step/validation metrics")
    from math import isfinite
    for role in ROLES:
        if type(steps[role]) is not int or steps[role] < 1:
            raise ValueError("selected warm-start optimizer step invalid")
        row = metrics[role]
        if not isinstance(row, dict) or set(row) != {"mse", "zero_q_mse"}:
            raise ValueError("incomplete aligned warm-start validation")
        mse, base = row["mse"], row["zero_q_mse"]
        if (type(mse) not in (int, float) or type(base) not in (int, float)
                or not isfinite(mse) or not isfinite(base)
                or mse < 0 or base <= 0 or mse >= base):
            raise ValueError("warm-start frozen candidate failed zero-Q baseline")


def publish_lineage(checkpoint: Path, parent: Path, source: dict,
                    candidate_model_version: int, report: dict):
    """Attach one immutable candidate/parent source record only after success."""
    checkpoint = checkpoint.absolute()
    parent = parent.absolute()
    private_owned(checkpoint.parent, is_dir=True)
    candidate_sha, parent_sha = _sha256(checkpoint), _sha256(parent)
    if parent_sha != source["parent_checkpoint_sha256"]:
        raise ValueError("parent checkpoint changed after source admission")
    aligned = report["validation_aligned_after"]
    record = {
        "schema": LINEAGE_SCHEMA,
        "candidate_model_version": candidate_model_version,
        "candidate_checkpoint_sha256": candidate_sha,
        "parent_model_version": source["parent_model_version"],
        "parent_checkpoint_sha256": parent_sha,
        "source_revision_claim": source["source_revision_claim"],
        "native_library_sha256": source["native_library_sha256"],
        "onpolicy_source_manifest_bundle_sha256":
            source["onpolicy_source_manifest_bundle_sha256"],
        "train_count": source["train_count"],
        "validation_count": source["validation_count"],
        "selected_step_by_role": report["selected_validation_step_by_role"],
        "aligned_validation_mse_by_role": {
            role: {"mse": aligned[role]["mse"],
                   "zero_q_mse": aligned[role]["zero_q_mse"]}
            for role in ROLES},
        "virgin_test_opened": False,
        "gameplay_skill_qualified": False,
    }
    _record_valid(record)
    marker = lineage_path(checkpoint)
    if marker.exists() or marker.is_symlink():
        raise FileExistsError("warm-start model lineage already published")
    content = (json.dumps(record, sort_keys=True, separators=(",", ":"),
                          allow_nan=False) + "\n").encode("utf-8")
    fd, staging = tempfile.mkstemp(prefix=".dmc-warmstart-", dir=checkpoint.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(content)
            out.flush()
            os.fsync(out.fileno())
        os.link(staging, marker)
        dfd = os.open(checkpoint.parent,
                      os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    finally:
        if os.path.exists(staging):
            os.unlink(staging)
    if json.loads(marker.read_text(encoding="utf-8")) != record:
        raise RuntimeError("warm-start lineage changed during private publication")
    return {
        "candidate_model_version": candidate_model_version,
        "candidate_checkpoint_sha256": candidate_sha,
        "warmstart_parent_model_version": source["parent_model_version"],
        "warmstart_parent_checkpoint_sha256": parent_sha,
        "onpolicy_source_manifest_bundle_sha256":
            source["onpolicy_source_manifest_bundle_sha256"],
        "lineage_record_sha256": hashlib.sha256(content).hexdigest(),
        "virgin_test_opened": False,
        "gameplay_skill_qualified": False,
    }
