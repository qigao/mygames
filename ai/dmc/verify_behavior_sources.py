#!/usr/bin/env python3
"""Independent admission of private, source-labeled C11 DDZ behavior cohorts.

Verifies exact sealed Replay bytes and their source/split metadata, not a
cryptographic build attestation or evidence that a learned model plays well.
Never print or upload private cards, trajectories or feature vectors.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from replay import OBJECTIVE_GAME_POINTS, OBJECTIVE_WIN_SIGN, read_episode

SCHEMA = "mygames-private-behavior-cohort-v1"
BEHAVIORS = frozenset(("mixed-v1", "shed-most", "legal-index"))
SPLITS = frozenset(("train", "validation", "test"))
MANIFEST = "behavior.v1.json"
MANIFEST_KEYS = frozenset((
    "schema", "behavior", "split", "source_revision", "generator_sha256",
    "model_version", "rules_version", "objective", "shuffle",
    "episode_count", "total_decisions", "role_decisions", "episodes",
))
EPISODE_KEYS = frozenset((
    "file", "episode_id", "sha256", "landlord_seat", "winner_seat",
    "accepted_actions", "role_decisions",
))
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


def require_private(path: Path, *, directory: bool) -> os.stat_result:
    info = path.lstat()  # reject links, including the terminal path
    if (not (stat.S_ISDIR(info.st_mode) if directory else
             stat.S_ISREG(info.st_mode)) or
            info.st_uid != os.geteuid() or
            stat.S_IMODE(info.st_mode) & 0o077 or
            (not directory and info.st_nlink != 1)):
        raise PermissionError("source cohort must be owned and private, without links")
    return info


def _integer(value, *, minimum: int = 0) -> bool:
    return type(value) is int and minimum <= value < 2**63


def _sha(value) -> bool:
    return isinstance(value, str) and HEX64.fullmatch(value) is not None


def verify_cohort(root: Path, *, source_revision: str,
                  generator_sha256: str, objective: str):
    require_private(root, directory=True)
    manifest_file = root / MANIFEST
    mstat = require_private(manifest_file, directory=False)
    if not 0 < mstat.st_size <= 128 * 1024:
        raise ValueError("private source manifest size out of bounds")
    manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
    if (not isinstance(manifest, dict) or
            set(manifest) != MANIFEST_KEYS or
            manifest["schema"] != SCHEMA or
            manifest["behavior"] not in BEHAVIORS or
            manifest["split"] not in SPLITS or
            manifest["source_revision"] != source_revision or
            manifest["generator_sha256"] != generator_sha256 or
            manifest["model_version"] != 1 or
            manifest["rules_version"] != 1 or
            manifest["objective"] != objective or
            manifest["shuffle"] != "OS-CSPRNG-c11"):
        raise ValueError("source/behavior/split/ABI manifest does not match")
    episodes = manifest["episodes"]
    if (not isinstance(episodes, list) or
            not 1 <= len(episodes) <= 96 or
            not _integer(manifest["episode_count"], minimum=1) or
            manifest["episode_count"] != len(episodes)):
        raise ValueError("incomplete/oversized native behavior cohort")

    model_objective = {
        "win-sign": OBJECTIVE_WIN_SIGN,
        "game-points": OBJECTIVE_GAME_POINTS,
    }[objective]
    seen_names, ids, hashes = set(), set(), set()
    decisions, role_totals = 0, [0, 0, 0]
    for entry in episodes:
        if not isinstance(entry, dict) or set(entry) != EPISODE_KEYS:
            raise ValueError("missing/extra native Replay metadata")
        eid = entry["episode_id"]
        name = entry["file"]
        if (not _integer(eid, minimum=1) or
                not isinstance(name, str) or name != f"episode-{eid}.mgdmc" or
                eid in ids or not _sha(entry["sha256"])):
            raise ValueError("duplicate/malformed native Replay identity")
        ids.add(eid)
        seen_names.add(name)
        path = root / name
        pstat = require_private(path, directory=False)
        if not 0 < pstat.st_size <= 215_000:
            raise ValueError("native Replay byte size outside v1 limits")
        if hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError("native Replay SHA256 differs from source manifest")
        if entry["sha256"] in hashes:
            raise ValueError("duplicate native Replay content")
        hashes.add(entry["sha256"])
        game = read_episode(path)  # independent strict parser; CRC/roles/returns
        role_counts = [sum(s.role == r for s in game.samples) for r in range(3)]
        if (game.episode_id != eid or game.model_version != 1 or
                game.rules_version != 1 or game.objective != model_objective or
                game.landlord_seat != (eid - 1) % 3 or
                game.landlord_seat != entry["landlord_seat"] or
                game.winner_seat != entry["winner_seat"] or
                not all(role_counts) or
                len(game.samples) != entry["accepted_actions"] or
                role_counts != entry["role_decisions"]):
            raise ValueError("unsealed/misattributed native behavior episode")
        decisions += len(game.samples)
        for r in range(3):
            role_totals[r] += role_counts[r]

    if (set(p.name for p in root.iterdir()) != seen_names | {MANIFEST} or
            manifest["total_decisions"] != decisions or
            manifest["role_decisions"] != role_totals or
            not _integer(decisions, minimum=1)):
        raise ValueError("incomplete/extra behavior files or inconsistent totals")
    return manifest["behavior"], manifest["split"], ids, hashes, {
        "episodes": len(ids), "decisions": decisions, "role_decisions": role_totals,
    }


def verify_matrix(args):
    if (not isinstance(args.source_revision, str) or
            HEX40.fullmatch(args.source_revision) is None or
            not _sha(args.generator_sha256) or
            args.objective not in ("win-sign", "game-points")):
        raise ValueError("explicit source SHA, binary SHA and objective are required")
    roots = [Path(p).absolute() for p in args.cohort]
    required = set(args.require_behavior or BEHAVIORS)
    if (not required or not required.issubset(BEHAVIORS) or
            len(roots) != len(required) * len(SPLITS) or
            len(set(roots)) != len(roots)):
        raise ValueError("expected one distinct sealed cohort per source and split")
    seen_pairs, seen_ids, seen_hashes = set(), set(), set()
    totals = {"train": 0, "validation": 0, "test": 0}
    for root in roots:
        behavior, split, ids, hashes, summary = verify_cohort(
            root, source_revision=args.source_revision,
            generator_sha256=args.generator_sha256, objective=args.objective)
        pair = (behavior, split)
        if (behavior not in required or pair in seen_pairs or
                ids & seen_ids or hashes & seen_hashes):
            raise ValueError("duplicate source/split or cross-cohort Replay leakage")
        seen_pairs.add(pair)
        seen_ids.update(ids)
        seen_hashes.update(hashes)
        totals[split] += summary["episodes"]
    if seen_pairs != {(b, s) for b in required for s in SPLITS}:
        raise ValueError("missing source/split cohort")
    return {
        "schema": "mygames-private-behavior-matrix-audit-v1",
        "cohorts_verified": len(roots), "sources_verified": sorted(required),
        "complete_episodes_by_split": totals,
        "cross_split_episode_ids_and_bytes_disjoint": True,
        "provenance_is_not_cryptographic_attestation": True,
        "ready_for_multisource_optimization": False,
        "qualified_gameplay_skill": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cohort", type=Path, action="append", required=True)
    parser.add_argument("--require-behavior", action="append", choices=sorted(BEHAVIORS))
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--generator-sha256", required=True)
    parser.add_argument("--objective", choices=("win-sign", "game-points"),
                        required=True)
    args = parser.parse_args()
    print(json.dumps(verify_matrix(args), sort_keys=True))


if __name__ == "__main__":
    main()
