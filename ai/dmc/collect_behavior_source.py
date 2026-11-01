#!/usr/bin/env python3
"""Collect one audited, immutable, single-behavior C11 DDZ Replay cohort.

Offline operator only. True OS-CSPRNG match/settlement is owned by the single
native MyGames generator. This script never implements rules or selects moves.
The SHA and checkout checks record provenance, not signed build attestation.
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
import tempfile

from replay import OBJECTIVE_GAME_POINTS, OBJECTIVE_WIN_SIGN, read_episode
from verify_behavior_sources import (
    BEHAVIORS, HEX40, HEX64, MANIFEST, SCHEMA, SPLITS,
    require_private, verify_cohort,
)

COMPLETE = re.compile(
    r"MYGAMES_PRIVATE_CORPUS_V2 behavior=(mixed-v1|shed-most|legal-index)"
    r" episodes=([0-9]+) steps=([0-9]+) landlord=([0-9]+)"
    r" landlord_up=([0-9]+) landlord_down=([0-9]+)"
    r" objective=(win-sign|game-points)\Z"
)


def git_text(source: Path, *tokens: str) -> str:
    proc = subprocess.run(["git", "-C", str(source), *tokens], check=True,
                          capture_output=True, text=True, timeout=15)
    return proc.stdout.strip()


def no_replace_private_json(root: Path, data: dict):
    destination = root / MANIFEST
    if destination.exists() or destination.is_symlink():
        raise FileExistsError("source completion marker already exists")
    tmp_fd, tmp_name = tempfile.mkstemp(prefix=".behavior-", dir=root)
    try:
        with os.fdopen(tmp_fd, "w", encoding="utf-8") as stream:
            json.dump(data, stream, sort_keys=True, separators=(",", ":"))
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.link(tmp_name, destination)  # never overwrite any existing manifest
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)


def collect(args):
    if (not 1 <= args.episodes <= 96 or
            not 1 <= args.first_id <= (2**63 - args.episodes) or
            args.behavior not in BEHAVIORS or args.split not in SPLITS or
            args.objective not in ("win-sign", "game-points") or
            not isinstance(args.source_revision, str) or
            HEX40.fullmatch(args.source_revision) is None or
            not isinstance(args.generator_sha256, str) or
            HEX64.fullmatch(args.generator_sha256) is None):
        raise ValueError("invalid bounded source/behavior/episode admission")

    checkout = args.source_checkout.absolute()
    if checkout.is_symlink() or not checkout.is_dir():
        raise ValueError("source checkout must be an existing non-symlink Git directory")
    checkout = checkout.resolve(strict=True)
    if git_text(checkout, "rev-parse", "HEAD") != args.source_revision:
        raise ValueError("source checkout HEAD does not equal pinned source revision")
    if git_text(checkout, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("source checkout tracked files are dirty")

    exe = args.native_generator.absolute()
    if exe.is_symlink() or not exe.is_file() or not os.access(exe, os.X_OK):
        raise ValueError("native generator must be an executable regular file")
    exe = exe.resolve(strict=True)
    if not stat.S_ISREG(exe.stat().st_mode):
        raise ValueError("native generator must be regular")
    actual_sha = hashlib.sha256(exe.read_bytes()).hexdigest()
    if actual_sha != args.generator_sha256:
        raise ValueError("native generator binary SHA256 mismatch")

    root = args.private_dir.absolute()
    require_private(root, directory=True)
    # Never generate private game trajectories into a Git checkout/worktree.
    if os.path.commonpath((str(root.resolve(strict=True)), str(checkout))) == str(checkout):
        raise ValueError("private cohort must be stored outside source checkout")
    if any(root.iterdir()):
        raise FileExistsError("private source directory must start EMPTY")

    cmd = [
        str(exe), "--private-dir", str(root),
        "--episodes", str(args.episodes), "--first-id", str(args.first_id),
        "--objective", args.objective, "--behavior", args.behavior,
    ]
    result = subprocess.run(cmd, cwd=checkout, capture_output=True,
                            text=True, check=True, timeout=1800)
    match = COMPLETE.fullmatch(result.stdout.strip())
    if (match is None or match[1] != args.behavior or
            int(match[2]) != args.episodes or match[7] != args.objective):
        raise ValueError("native C11 policy/episode/score completion mismatch")

    objective_id = (OBJECTIVE_WIN_SIGN if args.objective == "win-sign"
                    else OBJECTIVE_GAME_POINTS)
    records, role_totals, move_total = [], [0, 0, 0], 0
    expected = {f"episode-{eid}.mgdmc" for eid in
                range(args.first_id, args.first_id + args.episodes)}
    if {p.name for p in root.iterdir()} != expected:
        raise ValueError("native producer left missing or extra/partial source files")
    hashes = set()
    for eid in range(args.first_id, args.first_id + args.episodes):
        name = f"episode-{eid}.mgdmc"
        path = root / name
        st = require_private(path, directory=False)
        if not 0 < st.st_size <= 215_000:
            raise ValueError("native Replay exceeds versioned capacity")
        content = path.read_bytes()
        sha = hashlib.sha256(content).hexdigest()
        if sha in hashes:
            raise ValueError("duplicate Replay bytes")
        hashes.add(sha)
        episode = read_episode(path)
        roles = [sum(s.role == role for s in episode.samples) for role in range(3)]
        if (episode.episode_id != eid or episode.model_version != 1 or
                episode.rules_version != 1 or episode.objective != objective_id or
                episode.landlord_seat != (eid - 1) % 3 or not all(roles)):
            raise ValueError("native Replay behavior/turn/role/settlement mismatch")
        records.append({
            "file": name, "episode_id": eid, "sha256": sha,
            "landlord_seat": episode.landlord_seat,
            "winner_seat": episode.winner_seat,
            "accepted_actions": len(episode.samples),
            "role_decisions": roles,
        })
        move_total += len(episode.samples)
        for role in range(3):
            role_totals[role] += roles[role]
    if ([move_total, *role_totals] != [int(match[i]) for i in range(3, 7)] or
            not all(role_totals)):
        raise ValueError("native aggregate differs from sealed private Replay")
    manifest = {
        "schema": SCHEMA, "behavior": args.behavior, "split": args.split,
        "source_revision": args.source_revision,
        "generator_sha256": actual_sha,
        "model_version": 1, "rules_version": 1,
        "objective": args.objective, "shuffle": "OS-CSPRNG-c11",
        "episode_count": args.episodes, "total_decisions": move_total,
        "role_decisions": role_totals, "episodes": records,
    }
    no_replace_private_json(root, manifest)  # completion marker published LAST
    behavior, split, ids, _, summary = verify_cohort(
        root, source_revision=args.source_revision,
        generator_sha256=actual_sha, objective=args.objective)
    if behavior != args.behavior or split != args.split or len(ids) != args.episodes:
        raise ValueError("independent cohort re-read rejected source identity")
    return {
        "status": "sealed-native-C11-cohort",
        "behavior": behavior, "split": split, "episodes": summary["episodes"],
        "total_decisions": summary["decisions"],
        "role_decisions": summary["role_decisions"],
        "provenance_is_not_cryptographic_attestation": True,
        "ready_for_multisource_optimization": False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-generator", required=True, type=Path)
    parser.add_argument("--generator-sha256", required=True)
    parser.add_argument("--source-checkout", required=True, type=Path)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--private-dir", required=True, type=Path)
    parser.add_argument("--first-id", required=True, type=int)
    parser.add_argument("--episodes", required=True, type=int)
    parser.add_argument("--behavior", required=True, choices=sorted(BEHAVIORS))
    parser.add_argument("--split", required=True, choices=sorted(SPLITS))
    parser.add_argument("--objective", required=True,
                        choices=("win-sign", "game-points"))
    print(json.dumps(collect(parser.parse_args()), sort_keys=True))


if __name__ == "__main__":
    main()
