#!/usr/bin/env python3
"""OFFLINE learned DMC v3 model versus native C11 matched-deal baselines.

The ONLY game engine, legal actions, bid, turn transitions and terminal
nonmonetary scores are the existing MyGames C11 M5a/M5b/M5g. Native
DMCQSelector rechecks the full canonical legal list and fences the immutable
model. TEST-ONLY deterministic entropy creates a SAME shuffled 54-card deal
under landlord seat rotations and swapped candidate/baseline teams. No
online Game Owner, secret deck, other player's hand or Python DDZ rules.

Needs an explicitly configured private v3 checkpoint and locally compiled
TEST-only native tournament shared library; never downloads model weights,
logs private observation rows or uploads a Replay/weights artifact.
"""
from __future__ import annotations

import argparse
import ctypes as ct
import hashlib
import json
import random
from pathlib import Path

import numpy as np

from train_dmc import load_model_checkpoint, torch_runtime


class TournamentConfig(ct.Structure):
    _fields_ = [
        ("shuffled_seed", ct.c_uint32),
        ("landlord_seat", ct.c_uint8),
        ("candidate_landlord", ct.c_uint8),
        ("baseline_type", ct.c_uint8),
        ("identity_null", ct.c_uint8),
        ("candidate_model_version", ct.c_uint64),
        ("rules_version", ct.c_uint32),
    ]


class TournamentResult(ct.Structure):
    _fields_ = [
        ("candidate_game_points", ct.c_int32),
        ("candidate_win_sign", ct.c_int32),
        ("moves", ct.c_uint32),
        ("candidate_decisions", ct.c_uint32),
        ("baseline_decisions", ct.c_uint32),
        ("candidate_score_batches", ct.c_uint32),
        ("winner_seat", ct.c_uint8),
        ("landlord_seat", ct.c_uint8),
    ]


SCORE_FN = ct.CFUNCTYPE(
    ct.c_bool, ct.c_void_p, ct.c_uint32,
    ct.POINTER(ct.c_int8), ct.c_size_t,
    ct.POINTER(ct.c_int8), ct.c_size_t,
    ct.POINTER(ct.c_int8), ct.c_size_t,
    ct.POINTER(ct.c_float),
)


def load_native(path: Path):
    lib = ct.CDLL(str(path.resolve(strict=True)))
    lib.mygames_ddz_tournament_play.argtypes = [
        ct.POINTER(TournamentConfig), SCORE_FN, ct.c_void_p,
        ct.POINTER(TournamentResult),
    ]
    lib.mygames_ddz_tournament_play.restype = ct.c_int
    return lib


def frozen_scorer(checkpoint: Path, model_version: int):
    import torch
    torch, device = torch_runtime("cpu", seed=1)
    models, metadata = load_model_checkpoint(
        checkpoint, expected_rules=1, expected_objective="win-sign",
        expected_model_version=model_version, torch=torch, device=device)
    calls = [0, 0, 0]
    failures = []
    # One observed actor state per role. Cache only the portion of the
    # frozen LSTM+FC1 independent of the candidate legal action. This
    # preserves exact Q architecture, not a substitute/heuristic model.
    actor_state_cache = {}

    def score(_context, role, x_ptr, x_count, z_ptr, z_count,
              actions_ptr, count, output_ptr):
        try:
            role = int(role)
            if role not in (0, 1, 2) or x_count != (
                    319 if role == 0 else 430) or z_count != 810 or \
                    not 1 <= count <= 64:
                raise ValueError("frozen native Q ABI role/shape violation")
            x = np.ctypeslib.as_array(
                x_ptr, shape=(x_count,)).astype(np.float32)
            z = np.ctypeslib.as_array(
                z_ptr, shape=(810,)).astype(np.float32)
            actions = np.ctypeslib.as_array(
                actions_ptr, shape=(count * 54,)
            ).reshape(count, 54).astype(np.float32)
            if any(np.any((arr != 0) & (arr != 1))
                   for arr in (x, z, actions)):
                raise ValueError("nonbinary actor/legal-action features")
            model = models[role]
            key = (x.tobytes(), z.tobytes())
            with torch.inference_mode():
                state = actor_state_cache.get(role)
                if state is None or state[0] != key:
                    history = model.lstm(
                        torch.from_numpy(z).reshape(1, 5, 162)
                    )[0][:, -1, :].reshape(-1)
                    fixed = torch.cat((history, torch.from_numpy(x)))
                    width = fixed.numel()
                    projected = model.fc1.bias + torch.mv(
                        model.fc1.weight[:, :width], fixed)
                    state = (key, projected, width)
                    actor_state_cache[role] = state
                _, projected, width = state
                action_tensor = torch.from_numpy(actions)
                hidden = torch.relu(
                    projected.unsqueeze(0) + torch.nn.functional.linear(
                        action_tensor, model.fc1.weight[:, width:], None))
                for layer in (model.fc2, model.fc3, model.fc4, model.fc5):
                    hidden = torch.relu(layer(hidden))
                predicted = model.fc6(hidden).reshape(-1).cpu().numpy()
                # Sample direct Torch forward as a numerical parity gate,
                # including all three acting roles and legal batch sizes.
                if calls[role] % 128 == 0:
                    whole = np.concatenate([
                        np.broadcast_to(x, (count, x_count)), actions
                    ], axis=1)
                    direct = model(
                        torch.from_numpy(z).reshape(1, 5, 162)
                            .expand(count, -1, -1),
                        torch.from_numpy(whole)
                    ).detach().cpu().numpy()
                    if not np.allclose(predicted, direct,
                                       atol=2e-5, rtol=0):
                        raise RuntimeError("cached and direct frozen Q disagree")
                if predicted.shape != (count,) or \
                        not np.isfinite(predicted).all():
                    raise RuntimeError("incomplete/nonfinite native Q scores")
                np.ctypeslib.as_array(
                    output_ptr, shape=(count,))[:] = predicted
            calls[role] += 1
            return True
        except Exception as exc:
            failures.append(type(exc).__name__)
            return False

    return SCORE_FN(score), calls, failures, metadata


def play(lib, callback, *, seed, landlord, candidate_landlord,
         baseline, identity, model_version):
    cfg = TournamentConfig(
        int(seed), int(landlord), int(candidate_landlord),
        int(baseline), int(identity), int(model_version), 1)
    result = TournamentResult()
    result.candidate_win_sign = 777
    status = lib.mygames_ddz_tournament_play(
        ct.byref(cfg), callback, None, ct.byref(result))
    if status != 0:
        raise RuntimeError(
            f"native tournament aborted with explicit status {status}")
    if result.candidate_win_sign not in (-1, 1) or \
            result.moves == 0 or result.candidate_score_batches == 0:
        raise RuntimeError("native tournament terminal/score invalid")
    return result


def bootstrap(values: list[float], seed: int = 2048,
              replicates: int = 4000) -> list[float]:
    rng = random.Random(seed)
    n = len(values)
    samples = []
    for _ in range(replicates):
        samples.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    samples.sort()
    return [
        samples[int(0.025 * (replicates - 1))],
        samples[int(0.975 * (replicates - 1))],
    ]


def evaluate(args) -> dict:
    import torch
    torch.set_num_threads(2)
    native = load_native(args.native_library)
    callback, calls, failures, _ = frozen_scorer(
        args.checkpoint, args.model_version)
    rng = random.Random(0xBADCAC1)
    seeds = rng.sample(range(1, 2**32), args.seed_clusters)
    clusters = []
    for index, seed in enumerate(seeds):
        signed_win = 0
        nonmonetary_points = 0
        candidate_decisions = 0
        for seat in range(3):
            if index < args.identity_clusters:
                a = play(native, callback, seed=seed,
                         landlord=seat, candidate_landlord=1,
                         baseline=args.baseline_type, identity=True,
                         model_version=args.model_version)
                b = play(native, callback, seed=seed,
                         landlord=seat, candidate_landlord=0,
                         baseline=args.baseline_type, identity=True,
                         model_version=args.model_version)
                if a.winner_seat != b.winner_seat or \
                        a.candidate_win_sign + b.candidate_win_sign != 0 or \
                        a.candidate_game_points + b.candidate_game_points != 0:
                    raise AssertionError("same-deal A-vs-A identity null failed")
            for team in (1, 0):
                result = play(
                    native, callback, seed=seed, landlord=seat,
                    candidate_landlord=team, baseline=args.baseline_type,
                    identity=False, model_version=args.model_version)
                signed_win += result.candidate_win_sign
                nonmonetary_points += result.candidate_game_points
                candidate_decisions += result.candidate_decisions
        clusters.append({
            "cluster_index": index, "completed_matches": 6,
            "candidate_signed_win_sum": signed_win,
            "candidate_nonmonetary_points_sum": nonmonetary_points,
            "candidate_decisions": candidate_decisions,
        })

    wins = [c["candidate_signed_win_sum"] / 6.0 for c in clusters]
    points = [c["candidate_nonmonetary_points_sum"] / 6.0
              for c in clusters]
    sufficient = len(clusters) >= 30
    win_interval = bootstrap(wins) if sufficient else None
    points_interval = bootstrap(points) if sufficient else None
    score = {
        "schema": "mygames-private-dmc-learned-paired-v1",
        "checkpoint_sha256": hashlib.sha256(
            args.checkpoint.read_bytes()).hexdigest(),
        "model_version": args.model_version,
        "objective": "win-sign",
        "native_rules_and_terminal_settlement": True,
        "test_only_deterministic_shuffle": True,
        "identity_null_completed_matches":
            6 * min(args.identity_clusters, len(clusters)),
        "paired_complete_matches": 6 * len(clusters),
        "independent_shuffle_clusters": len(clusters),
        "baseline": args.baseline,
        "candidate_win_rate": (1.0 + sum(wins) / len(wins)) / 2.0,
        "candidate_mean_nonmonetary_points":
            sum(points) / len(points),
        "cluster_bootstrap_95_win_rate": [
            (1.0 + x) / 2.0 for x in win_interval
        ] if sufficient else None,
        "cluster_bootstrap_95_points": points_interval,
        "frozen_score_batches_by_role": calls,
        "score_failures": len(failures),
        # An exploratory, preselected 30-shuffle bootstrap diagnostic is
        # NOT deployment or proof of out-of-distribution generalization.
        "qualified_as_production_bot": False,
    }
    if args.include_aggregates:
        score["public_cluster_aggregates"] = clusters
    if failures:
        raise RuntimeError("frozen native scorer failed; no score admitted")
    return score


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--native-library", type=Path, required=True)
    parser.add_argument("--model-version", type=int, required=True)
    parser.add_argument("--seed-clusters", type=int, default=30)
    parser.add_argument("--baseline", choices=("shed-most", "legal-index"),
                        required=True)
    parser.add_argument("--identity-clusters", type=int, default=2)
    parser.add_argument("--include-aggregates", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.seed_clusters <= 256 or not \
            0 <= args.identity_clusters <= args.seed_clusters or \
            not 1 <= args.model_version < 2**64:
        parser.error("seed clusters 1..256; identity clusters 0..N; positive model uint64")
    args.baseline_type = 0 if args.baseline == "shed-most" else 1
    print(json.dumps(evaluate(args), sort_keys=True))


if __name__ == "__main__":
    main()
