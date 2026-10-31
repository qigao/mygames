#!/usr/bin/env python3
"""Offline trained-v3 PyTorch vs C11 float32 parity on genuine native Replay.

This OPTIONAL test needs already configured PyTorch/NumPy, an independently
compiled exact-source C11 reference library, a PRIVATE checkpoint/export,
and held-out, sealed native MyGames Replay v1. Never uploads raw observations,
physical cards, hidden player states, weights, or Q scores. Not a win-rate.
"""
from __future__ import annotations

import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path
import random
import sys

import numpy as np
import torch

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "ai" / "dmc"))
from replay import read_episode
from model import ROLE_DIMENSIONS, ROLES
from train_dmc import load_model_checkpoint, torch_runtime


class NativeModel(ct.Structure):
    _fields_ = (
        ("weights", ct.POINTER(ct.c_float)),
        ("weight_count", ct.c_size_t),
        ("model_version", ct.c_uint64),
        ("rules_version", ct.c_uint32),
        ("objective", ct.c_uint32),
        ("role", ct.c_uint32),
        ("x_count", ct.c_uint32),
    )


def native_api(path: Path):
    lib = ct.CDLL(str(path.resolve(strict=True)))
    lib.mygames_ddz_q_cpu_load.argtypes = [
        ct.c_void_p, ct.c_size_t, ct.c_uint64, ct.c_uint32, ct.c_uint32,
        ct.c_uint32, ct.POINTER(ct.c_float), ct.c_size_t,
        ct.POINTER(NativeModel), ct.POINTER(ct.c_size_t),
    ]
    lib.mygames_ddz_q_cpu_load.restype = ct.c_int
    lib.mygames_ddz_q_cpu_score.argtypes = [
        ct.POINTER(NativeModel), ct.POINTER(ct.c_int8), ct.c_size_t,
        ct.POINTER(ct.c_int8), ct.POINTER(ct.c_int8), ct.c_size_t,
        ct.POINTER(ct.c_float),
    ]
    lib.mygames_ddz_q_cpu_score.restype = ct.c_int
    return lib


def run(args):
    torch.set_num_threads(2)
    torch_runtime("cpu", seed=1)
    models, metadata = load_model_checkpoint(
        args.checkpoint, expected_rules=args.rules_version,
        expected_objective=args.objective,
        expected_model_version=args.model_version,
        torch=torch, device=torch.device("cpu"))
    manifest = json.loads((args.export_dir / "q_cpu_manifest_v1.json").read_text())
    assert manifest["model_version"] == args.model_version
    assert manifest["rules_version"] == args.rules_version
    assert manifest["objective"] == args.objective
    assert manifest["source_checkpoint_sha256"] == hashlib.sha256(
        args.checkpoint.read_bytes()).hexdigest()

    paths = sorted(args.replay_dir.glob("*.mgdmc"))
    if len(paths) < 3:
        raise ValueError("need three or more independent complete native test episodes")
    episodes = [read_episode(p) for p in paths]
    excluded = set(metadata["train_episode_ids"]) | set(
        metadata["validation_episode_ids"])
    if excluded & {e.episode_id for e in episodes}:
        raise ValueError("held-out test overlaps training/tuning validation games")
    expected_objective = {"win-sign": 1, "game-points": 2}[args.objective]
    if any(e.model_version >= args.model_version or
           e.objective != expected_objective or
           e.rules_version != args.rules_version for e in episodes):
        raise ValueError("native Replay source versions/objective mismatch")

    lib = native_api(args.native_library)
    rng = random.Random(1027)
    details = {}
    for role in range(3):
        info = manifest["role_images"][ROLES[role]]
        raw = (args.export_dir / info["file"]).read_bytes()
        if hashlib.sha256(raw).hexdigest() != info["sha256"]:
            raise ValueError("frozen native weight digest mismatch")
        backing = ct.create_string_buffer(raw)
        count = ct.c_size_t()
        if lib.mygames_ddz_q_cpu_load(
                backing, len(raw), args.model_version,
                args.rules_version, expected_objective, role,
                None, 0, None, ct.byref(count)) != 0:
            raise ValueError("native model weight-size query rejected")
        weights = (ct.c_float * count.value)()
        model = NativeModel()
        if lib.mygames_ddz_q_cpu_load(
                backing, len(raw), args.model_version, args.rules_version,
                expected_objective, role, weights, count.value,
                ct.byref(model), ct.byref(count)) != 0:
            raise ValueError("native frozen model admission rejected")
        pool = [s for ep in episodes for s in ep.samples if s.role == role]
        if not pool:
            raise ValueError("held-out native Replay missing a role")
        samples = rng.sample(pool, min(args.max_per_role, len(pool)))
        largest = 0.0
        for step in samples:
            x = np.frombuffer(step.x, dtype=np.int8).copy()
            z = np.frombuffer(step.z, dtype=np.int8).copy()
            action = np.frombuffer(step.action, dtype=np.int8).copy()
            if (len(x) != ROLE_DIMENSIONS[role] or len(z) != 810 or
                    len(action) != 54):
                raise ValueError("real Replay action Q feature shape mismatch")
            actual = (ct.c_float * 1)()
            status = lib.mygames_ddz_q_cpu_score(
                ct.byref(model),
                x.ctypes.data_as(ct.POINTER(ct.c_int8)), len(x),
                z.ctypes.data_as(ct.POINTER(ct.c_int8)),
                action.ctypes.data_as(ct.POINTER(ct.c_int8)), 1, actual)
            if status != 0:
                raise RuntimeError("C11 scorer rejected valid native Replay features")
            with torch.inference_mode():
                tx = torch.tensor(np.concatenate((x, action)).astype(
                    np.float32)).reshape(1, -1)
                tz = torch.tensor(z.astype(np.float32)).reshape(1, 5, 162)
                expected = float(models[role](tz, tx).item())
            error = abs(expected - float(actual[0]))
            largest = max(largest, error)
            if error >= args.abs_tolerance:
                raise AssertionError("trained Torch/C11 Replay Q values disagree")
        details[ROLES[role]] = {
            "real_heldout_actions": len(samples),
            "worst_absolute_q_error": largest,
        }
    return {
        "actual_native_replay": True,
        "heldout_complete_games": len(episodes),
        "trained_model_version": args.model_version,
        "roles": details,
        "qualified_as_competitive_skill": False,
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--checkpoint", type=Path, required=True)
    p.add_argument("--export-dir", type=Path, required=True)
    p.add_argument("--native-library", type=Path, required=True)
    p.add_argument("--replay-dir", type=Path, required=True)
    p.add_argument("--model-version", type=int, required=True)
    p.add_argument("--rules-version", type=int, default=1)
    p.add_argument("--objective", choices=("win-sign", "game-points"), required=True)
    p.add_argument("--max-per-role", type=int, default=24)
    p.add_argument("--abs-tolerance", type=float, default=5e-4)
    args = p.parse_args()
    if not (1 <= args.max_per_role <= 128 and 0 < args.abs_tolerance < 0.01):
        p.error("max-per-role must be 1..128, abs-tolerance must be (0,0.01)")
    print(json.dumps(run(args), sort_keys=True))


if __name__ == "__main__":
    main()
