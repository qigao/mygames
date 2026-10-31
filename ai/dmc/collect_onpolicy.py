#!/usr/bin/env python3
"""PRIVATE offline on-policy C11 Replay collection with one frozen Q snapshot.

The ONLY game authority is native MyGames::DouDizhuMatch/DMCQSelector.
This uses OS CSPRNG for each entire game, not the tournament TEST-only
shuffle. All three roles use the selected checkpoint's exact Q weights.
No Torch, native replay, hidden feature vector or weights are uploaded.
"""
from __future__ import annotations

import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile

from replay import MAX_BYTES, parse_episode
from tournament import SCORE_FN, frozen_scorer


class OnPolicyConfig(ct.Structure):
    _fields_ = [
        ("episode_id", ct.c_uint64),
        ("frozen_model_version", ct.c_uint64),
        ("rules_version", ct.c_uint32),
        ("objective", ct.c_int),
        ("offline_landlord_seat", ct.c_uint8),
    ]


def private_dir(path: Path) -> None:
    st = path.lstat()  # do not follow a symlink
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid() or \
            stat.S_IMODE(st.st_mode) & 0o077:
        raise PermissionError("existing output directory must be owner-only 0700")


def private_checkpoint(path: Path) -> None:
    st = path.lstat()
    if not stat.S_ISREG(st.st_mode) or st.st_uid != os.geteuid() or \
            stat.S_IMODE(st.st_mode) & 0o077 or st.st_size > 160 * 1024 * 1024:
        raise PermissionError("frozen checkpoint must be owned private regular file <=160MiB")


def immutable_private_write(directory: Path, name: str, data: bytes) -> None:
    # No-clobber publication with fsync; no successful Replay can be a partial
    # file. A process crash may leave .onpolicy-*, never admitted as Replay.
    if not re.fullmatch(r"(?:episode-[1-9][0-9]*\.mgdmc|onpolicy\.v1\.json)", name):
        raise ValueError("invalid private file name")
    dst = directory / name
    if dst.exists() or dst.is_symlink():
        raise FileExistsError("private Replay/manifest already published")
    fd, staging = tempfile.mkstemp(dir=directory, prefix=".onpolicy-")
    try:
        with os.fdopen(fd, "wb") as target:
            target.write(data)
            target.flush()
            os.fsync(target.fileno())
        os.link(staging, dst)  # atomic NO-CLOBBER, no os.replace
    finally:
        if os.path.exists(staging):
            os.unlink(staging)
    dirfd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)


def collect(args):
    root = args.private_dir.absolute()
    private_dir(root)
    checkpoint = args.checkpoint.absolute()
    private_checkpoint(checkpoint)
    if args.native_library.is_symlink():
        raise ValueError("native on-policy library must not be a symlink")
    library = args.native_library.resolve(strict=True)
    if not library.is_file():
        raise ValueError("native on-policy C11 library must be a regular file")
    if args.objective != "win-sign":
        raise ValueError("current frozen scorer is win-sign only")
    if not re.fullmatch(r"[0-9a-f]{64}", args.checkpoint_sha256) or \
            hashlib.sha256(checkpoint.read_bytes()).hexdigest() != args.checkpoint_sha256:
        raise ValueError("immutable frozen checkpoint digest mismatch")
    if not re.fullmatch(r"[0-9a-f]{64}", args.native_library_sha256) or \
            hashlib.sha256(library.read_bytes()).hexdigest() != args.native_library_sha256:
        raise ValueError("native on-policy source binary digest mismatch")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_revision):
        raise ValueError("explicit 40-digit source commit SHA required")
    if not 1 <= args.episodes <= 96 or args.first_id < 1 or \
            args.first_id + args.episodes - 1 >= 2**63 or \
            args.model_version < 2 or args.model_version >= 2**64 or \
            args.objective not in ("win-sign", "game-points"):
        raise ValueError("invalid bounded episode/model/objective admission")
    if any(root.iterdir()):
        raise FileExistsError("on-policy source directory must start empty")

    # Checkpoint validation is entirely within frozen_scorer(); it must be
    # preconfigured Torch, not a runtime fallback or an untrusted pickle.
    scorer, calls, failures, info = frozen_scorer(checkpoint, args.model_version)
    if info.get("candidate_model_version") != args.model_version:
        raise ValueError("checkpoint behavior snapshot mismatch")
    if info.get("objective") != args.objective or info.get("rules_version") != 1:
        raise ValueError("checkpoint rules/objective mismatch")
    excluded = set(info["train_episode_ids"]) | set(info["validation_episode_ids"])
    ids = tuple(range(args.first_id, args.first_id + args.episodes))
    if excluded.intersection(ids):
        raise ValueError("on-policy episode IDs overlap source model train/validation IDs")
    lib = ct.CDLL(str(library))
    lib.mygames_ddz_dmc_onpolicy_collect.argtypes = [
        ct.POINTER(OnPolicyConfig), SCORE_FN, ct.c_void_p,
        ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_size_t),
        ct.POINTER(ct.c_uint32),
    ]
    lib.mygames_ddz_dmc_onpolicy_collect.restype = ct.c_int
    replay_buffer = (ct.c_uint8 * MAX_BYTES)()
    records = []
    decisions_by_role = [0, 0, 0]
    for eid in ids:
        cfg = OnPolicyConfig(
            eid, args.model_version, 1,
            1 if args.objective == "win-sign" else 2,
            (eid - 1) % 3,
        )
        replay_size = ct.c_size_t(0)
        moves = ct.c_uint32(0)
        status = lib.mygames_ddz_dmc_onpolicy_collect(
            ct.byref(cfg), scorer, None, replay_buffer, MAX_BYTES,
            ct.byref(replay_size), ct.byref(moves),
        )
        if status != 0:
            raise RuntimeError(f"native on-policy episode failed closed, status={status}")
        data = bytes(replay_buffer[:replay_size.value])
        episode = parse_episode(data)  # independent canonical reader
        if episode.episode_id != eid or episode.model_version != args.model_version or \
                episode.rules_version != 1 or episode.objective != cfg.objective or \
                episode.landlord_seat != cfg.offline_landlord_seat or \
                len(episode.samples) != moves.value:
            raise ValueError("native complete Replay metadata/target mismatch")
        roles = [0, 0, 0]
        for sample in episode.samples:
            roles[sample.role] += 1
        if not all(roles):
            raise ValueError("all three frozen roles must act during each completed game")
        for role in range(3):
            decisions_by_role[role] += roles[role]
        name = f"episode-{eid}.mgdmc"
        immutable_private_write(root, name, data)
        records.append({"file": name, "episode_id": eid,
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "landlord_seat": episode.landlord_seat,
                        "winner_seat": episode.winner_seat,
                        "decisions": moves.value, "role_decisions": roles})
    if failures or not all(calls) or not all(decisions_by_role):
        raise RuntimeError("frozen score model failed or missing required role")
    manifest = {
        "schema": "mygames-private-onpolicy-v1",
        "behavior_policy": "frozen-greedy-q-v3",
        "source_revision_claim": args.source_revision,
        "native_library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "source_checkpoint_sha256": hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        "model_version": args.model_version, "rules_version": 1,
        "objective": args.objective, "shuffle": "OS-CSPRNG-c11",
        "episodes": records, "role_decisions": decisions_by_role,
        "total_decisions": sum(decisions_by_role),
        "model_score_batches_by_role": calls,
    }
    immutable_private_write(root, "onpolicy.v1.json",
        (json.dumps(manifest, sort_keys=True) + "\n").encode())
    # stdout is PUBLIC AGGREGATE counts, NOT checkpoint/replay bytes/records.
    return {"completed_episodes": len(records),
            "completed_moves": sum(decisions_by_role),
            "role_moves": decisions_by_role,
            "source_behavior_model": args.model_version,
            "models_failed": len(failures),
            "qualified_as_trained_gameplay_superiority": False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--checkpoint", required=True, type=Path)
    p.add_argument("--checkpoint-sha256", required=True)
    p.add_argument("--native-library", required=True, type=Path)
    p.add_argument("--native-library-sha256", required=True)
    p.add_argument("--private-dir", required=True, type=Path)
    p.add_argument("--source-revision", required=True)
    p.add_argument("--model-version", type=int, required=True)
    p.add_argument("--first-id", type=int, required=True)
    p.add_argument("--episodes", type=int, required=True)
    p.add_argument("--objective", choices=("win-sign",), required=True)
    print(json.dumps(collect(p.parse_args()), sort_keys=True))


if __name__ == "__main__":
    main()
