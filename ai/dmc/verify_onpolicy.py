#!/usr/bin/env python3
"""Trust-boundary verifier for private frozen-model C11 on-policy Replay.

This standard-library-only gate MUST run before the next DMC model optimizer.
It verifies full, sealed native episodes under a SINGLE frozen source model
and the exactly selected checkpoint/library SHA256 in three DISJOINT pools.
CRC32/SHA256 provide integrity/digests, NOT a signature or game attestation.
Never print/private-upload raw action, x/z, deck or terminal feature bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from replay import read_episode

SCHEMA = "mygames-private-onpolicy-v1"
ROLES = 3
SPLITS = ("train", "validation", "test")
ROOT_KEYS = {"schema", "behavior_policy", "source_revision_claim",
             "native_library_sha256", "source_checkpoint_sha256",
             "model_version", "rules_version", "objective", "shuffle",
             "episodes", "role_decisions", "total_decisions",
             "model_score_batches_by_role"}
ENTRY_KEYS = {"file", "episode_id", "sha256", "landlord_seat",
              "winner_seat", "decisions", "role_decisions"}


def private_owned(path: Path, is_dir: bool) -> os.stat_result:
    item = path.lstat()
    if (is_dir and not stat.S_ISDIR(item.st_mode)) or \
            (not is_dir and not stat.S_ISREG(item.st_mode)) or \
            item.st_uid != os.geteuid() or \
            stat.S_IMODE(item.st_mode) & 0o077 or \
            (not is_dir and item.st_nlink != 1):
        raise PermissionError("private on-policy source ownership/ACL/links invalid")
    return item


def verified_split(root: Path, args) -> tuple[dict, set[int], set[str]]:
    private_owned(root, is_dir=True)
    manifest_path = root / "onpolicy.v1.json"
    info = private_owned(manifest_path, is_dir=False)
    if not 1 <= info.st_size <= 256 * 1024:
        raise ValueError("on-policy manifest size invalid")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or set(manifest) != ROOT_KEYS or \
            manifest["schema"] != SCHEMA or \
            manifest["behavior_policy"] != "frozen-greedy-q-v3" or \
            manifest["source_revision_claim"] != args.source_revision or \
            manifest["native_library_sha256"] != args.native_library_sha256 or \
            manifest["source_checkpoint_sha256"] != args.checkpoint_sha256 or \
            manifest["model_version"] != args.behavior_model_version or \
            manifest["rules_version"] != 1 or \
            manifest["objective"] != "win-sign" or \
            manifest["shuffle"] != "OS-CSPRNG-c11":
        raise ValueError("on-policy model/source/rules/provenance mismatch")
    items = manifest["episodes"]
    if not isinstance(items, list) or not 1 <= len(items) <= 96:
        raise ValueError("invalid bounded completed on-policy cohort")
    found = set()
    hashes = set()
    roles = [0, 0, 0]
    ids = set()
    decisions = 0
    for entry in items:
        if not isinstance(entry, dict) or set(entry) != ENTRY_KEYS:
            raise ValueError("incomplete on-policy episode record")
        eid, name = entry["episode_id"], entry["file"]
        if type(eid) is not int or eid <= 0 or eid >= 2**63 or \
                not isinstance(name, str) or name != f"episode-{eid}.mgdmc" or \
                eid in ids:
            raise ValueError("on-policy Episode ID/filename duplicate or malformed")
        ids.add(eid)
        found.add(name)
        path = root / name
        meta = private_owned(path, is_dir=False)
        if not 0 < meta.st_size <= 215_000:
            raise ValueError("private native Replay exceeds canonical limit")
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("on-policy Replay SHA256 differs from manifest")
        if entry["sha256"] in hashes:
            raise ValueError("duplicate native Replay bytes")
        hashes.add(entry["sha256"])
        episode = read_episode(path)  # canonical terminal/role/target CRC audit
        if episode.episode_id != eid or \
                episode.model_version != args.behavior_model_version or \
                episode.rules_version != 1 or episode.objective != 1 or \
                episode.landlord_seat != entry["landlord_seat"] or \
                episode.winner_seat != entry["winner_seat"] or \
                len(episode.samples) != entry["decisions"]:
            raise ValueError("sealed on-policy native source/target mismatch")
        local_roles = [sum(s.role == role for s in episode.samples)
                       for role in range(ROLES)]
        if entry["role_decisions"] != local_roles or not all(local_roles):
            raise ValueError("on-policy role decision attribution mismatch")
        for role in range(ROLES):
            roles[role] += local_roles[role]
        decisions += len(episode.samples)
    if set(p.name for p in root.iterdir()) != found | {"onpolicy.v1.json"} or \
            manifest["total_decisions"] != decisions or \
            manifest["role_decisions"] != roles or \
            not isinstance(manifest["model_score_batches_by_role"], list) or \
            len(manifest["model_score_batches_by_role"]) != 3 or \
            any(type(c) is not int or c < n
                for c, n in zip(manifest["model_score_batches_by_role"], roles)):
        raise ValueError("on-policy corpus incomplete/partial/extra or aggregate mismatch")
    return {"episodes": len(ids), "decisions": decisions, "role_decisions": roles}, ids, hashes


def verify(args) -> dict:
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_revision) or \
            not re.fullmatch(r"[0-9a-f]{64}", args.checkpoint_sha256) or \
            not re.fullmatch(r"[0-9a-f]{64}", args.native_library_sha256) or \
            not 2 <= args.behavior_model_version < 2**64 or \
            not args.behavior_model_version < args.next_model_version < 2**64:
        raise ValueError("explicit immutable checkpoint/native/next-model admission required")
    source = args.checkpoint.absolute()
    checkpoint_info = private_owned(source, is_dir=False)
    if not 0 < checkpoint_info.st_size <= 160 * 1024 * 1024:
        raise ValueError("behavior checkpoint size outside frozen v3 private limit")
    if hashlib.sha256(source.read_bytes()).hexdigest() != args.checkpoint_sha256:
        raise ValueError("behavior checkpoint SHA256 changed")
    roots = {split: getattr(args, f"{split}_dir").absolute()
             for split in SPLITS}
    if len(set(roots.values())) != 3:
        raise ValueError("cannot reuse a source directory across pools")
    ids = set()
    hashes = set()
    report = {"schema": "mygames-private-onpolicy-three-way-audit-v1",
              "behavior_model_version": args.behavior_model_version,
              "next_model_version": args.next_model_version,
              "splits": {}, "ready_for_offline_training": True,
              "qualified_as_live_game_skill": False}
    for split, root in roots.items():
        summary, split_ids, split_hashes = verified_split(root, args)
        if ids & split_ids or hashes & split_hashes:
            raise ValueError("train/validation/test Episode ID or content leakage")
        ids |= split_ids
        hashes |= split_hashes
        report["splits"][split] = summary
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for split in SPLITS:
        p.add_argument(f"--{split}-dir", type=Path, required=True)
    p.add_argument("--checkpoint", type=Path, required=True)
    p.add_argument("--checkpoint-sha256", required=True)
    p.add_argument("--native-library-sha256", required=True)
    p.add_argument("--source-revision", required=True)
    p.add_argument("--behavior-model-version", type=int, required=True)
    p.add_argument("--next-model-version", type=int, required=True)
    print(json.dumps(verify(p.parse_args()), sort_keys=True))


if __name__ == "__main__":
    main()
