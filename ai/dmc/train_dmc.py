#!/usr/bin/env python3
"""Offline MyGames three-role DMC learner, with *episode-disjoint* evaluation.

The genuine C11 rules engine is the ONLY source of legal moves and rewards.
Requires PyTorch for --train/--evaluate-only; no fallback model/runtime.
--verify-only checks private Replay provenance without importing PyTorch.
A validation episode must NEVER overlap a training episode ID.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import stat
import tempfile
from typing import Sequence

from replay import OBJECTIVE_GAME_POINTS, OBJECTIVE_WIN_SIGN, Episode, read_episode

ROLES = {0: "landlord", 1: "landlord_up", 2: "landlord_down"}
OBJECTIVES = {"win-sign": OBJECTIVE_WIN_SIGN, "game-points": OBJECTIVE_GAME_POINTS}
CHECKPOINT_FORMAT = "mygames-douzero-dmc-v3"


def load_checked(paths: Sequence[str], rules_version: int,
                 objective: int) -> tuple[list[Episode], dict[int, list]]:
    episodes: list[Episode] = []
    by_role: dict[int, list] = {0: [], 1: [], 2: []}
    seen: set[int] = set()
    for path in paths:
        episode = read_episode(path)  # strict v1 source validation before a GPU allocation
        if episode.rules_version != rules_version or episode.objective != objective:
            raise ValueError("replay rules_version/objective mismatch: " + path)
        if episode.episode_id in seen:
            raise ValueError("duplicate episode_id: " + str(episode.episode_id))
        seen.add(episode.episode_id)
        episodes.append(episode)
        for step in episode.samples:
            by_role[step.role].append(step)
    if not episodes:
        raise ValueError("no complete replay episodes")
    return episodes, by_role


def require_disjoint(training: list[Episode], validation: list[Episode]) -> None:
    overlap = {e.episode_id for e in training} & {e.episode_id for e in validation}
    if overlap:
        raise ValueError("training/validation episode_id overlap; reject leakage")
    for name, data in (("training", training), ("validation", validation)):
        counts = {role: sum(s.role == role for e in data for s in e.samples)
                  for role in ROLES}
        if not all(counts.values()):
            raise ValueError(f"{name} missing a landlord/up/down role: {counts}")


def torch_runtime(device_name: str, seed: int):
    try:
        import torch
    except ImportError as exc:
        raise RuntimeError("training/evaluation requires the configured PyTorch backend") from exc
    if device_name != "cpu" and device_name != "cuda" and not (
        device_name.startswith("cuda:") and device_name[5:].isdigit()
    ):
        raise ValueError("device must be cpu, cuda or cuda:<index>")
    if device_name.startswith("cuda") and not torch.cuda.is_available():
        raise RuntimeError("requested CUDA backend unavailable; no CPU fallback")
    torch.manual_seed(seed)
    if device_name == "cpu":
        torch.set_num_threads(min(4, os.cpu_count() or 1))
    return torch, torch.device(device_name)


def evaluate_rows(model, rows, device, batch_size: int) -> dict:
    import torch
    from model import batch_tensors
    if not rows:
        raise ValueError("cannot measure empty evaluation role")
    sq_error = abs_error = zero_sq = 0.0
    model.eval()
    with torch.no_grad():
        for start in range(0, len(rows), batch_size):
            z, xa, targets = batch_tensors(rows[start:start + batch_size], device)
            predicted = model(z, xa)
            if not torch.isfinite(predicted).all().item():
                raise RuntimeError("nonfinite validation prediction")
            sq_error += torch.square(predicted - targets).sum().item()
            abs_error += torch.abs(predicted - targets).sum().item()
            zero_sq += torch.square(targets).sum().item()
    return {"count": len(rows), "mse": sq_error / len(rows),
            "mae": abs_error / len(rows), "zero_q_mse": zero_sq / len(rows)}


def evaluate_aligned_episodes(model, episodes: list[Episode], role: int,
                              sampler, device, batch_size: int) -> dict:
    """Measure complete-game validation with the exact stratified batch weights.

    One batched Torch forward pass yields per-decision squared/absolute errors.
    Only the aggregation changes: move -> game -> outcome -> source. Never
    select a balanced checkpoint using the legacy transition-weighted metric.
    """
    import torch
    from model import batch_tensors
    if batch_size <= 0:
        raise ValueError("validation batch size must be positive")
    rows = []
    episode_ids = []
    for episode in episodes:
        for sample in episode.samples:
            if sample.role == role:
                rows.append(sample)
                episode_ids.append(episode.episode_id)
    if not rows:
        raise ValueError("balanced validation has no acting-role decisions")
    per_game = {}
    model.eval()
    with torch.no_grad():
        for start in range(0, len(rows), batch_size):
            z, xa, targets = batch_tensors(rows[start:start + batch_size], device)
            predicted = model(z, xa)
            if not torch.isfinite(predicted).all().item():
                raise RuntimeError("nonfinite balanced-validation prediction")
            errors = predicted - targets
            components = torch.stack(
                (torch.square(errors), torch.abs(errors), torch.square(targets)),
                dim=1).detach().cpu().tolist()
            ids = episode_ids[start:start + batch_size]
            if len(components) != len(ids):
                raise RuntimeError("balanced-validation output shape mismatch")
            for episode_id, (squared, absolute, zero_squared) in zip(ids, components):
                stats = per_game.setdefault(episode_id, {
                    "count": 0, "mse": 0.0, "mae": 0.0, "zero_q_mse": 0.0})
                stats["count"] += 1
                stats["mse"] += squared
                stats["mae"] += absolute
                stats["zero_q_mse"] += zero_squared
    for stats in per_game.values():
        for key in ("mse", "mae", "zero_q_mse"):
            stats[key] /= stats["count"]
    return sampler.aggregate_validation(per_game)


def load_model_checkpoint(path: str | Path, *, expected_rules: int,
                          expected_objective: str, expected_model_version: int,
                          torch, device):
    from model import DouZeroQ, FEATURE_CONTRACT, MODEL_FORMAT
    p = Path(path)
    if not p.is_file() or p.stat().st_size > 160 * 1024 * 1024:
        raise ValueError("checkpoint missing or exceeds 160 MiB private limit")
    # Strict weights-only mode: never unpickle arbitrary model code.
    payload = torch.load(p, weights_only=True, map_location=device)
    if not isinstance(payload, dict) or set(payload) != {
        "format", "model_architecture", "feature_contract", "rules_version",
        "objective", "source_model_versions", "candidate_model_version",
        "train_episode_ids",
        "validation_episode_ids", "training_steps_per_role", "seed",
        "role_state_dicts",
    } or payload["format"] != CHECKPOINT_FORMAT or \
            payload["model_architecture"] != MODEL_FORMAT or \
            payload["feature_contract"] != FEATURE_CONTRACT or \
            payload["rules_version"] != expected_rules or \
            payload["objective"] != expected_objective or \
            type(payload["candidate_model_version"]) is not int or \
            payload["candidate_model_version"] != expected_model_version or \
            not (0 < expected_model_version < 2**64) or \
            not isinstance(payload["source_model_versions"], list) or \
            not payload["source_model_versions"] or \
            not all(type(v) is int and 0 < v < expected_model_version
                    for v in payload["source_model_versions"]) or \
            not isinstance(payload["role_state_dicts"], dict) or \
            set(payload["role_state_dicts"]) != set(ROLES.values()):
        raise ValueError("checkpoint ABI/rules/objective/roles mismatch")
    if set(payload["train_episode_ids"]) & set(payload["validation_episode_ids"]):
        raise ValueError("checkpoint contains training/validation ID overlap")
    models = {}
    for role in ROLES:
        model = DouZeroQ(role).to(device)
        model.load_state_dict(payload["role_state_dicts"][ROLES[role]], strict=True)
        model.eval()
        models[role] = model
    return models, payload


def publish_private_checkpoint(destination: Path, payload: dict, torch,
                               device, validation_by_role: dict[int, list],
                               original_models: dict, batch_size: int) -> str:
    from model import batch_tensors
    if destination.exists():
        raise FileExistsError("checkpoint already exists; refusing overwrite")
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name == "posix" and stat.S_IMODE(destination.parent.stat().st_mode) & 0o077:
        raise PermissionError("checkpoint directory must not be group/world accessible")
    fd, tmp_name = tempfile.mkstemp(prefix=".dmc-candidate-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            torch.save(payload, output)
            output.flush()
            os.fsync(output.fileno())
        loaded, _ = load_model_checkpoint(
            tmp_name, expected_rules=payload["rules_version"],
            expected_objective=payload["objective"],
            expected_model_version=payload["candidate_model_version"],
            torch=torch, device=device)
        for role in ROLES:
            few = validation_by_role[role][:max(1, min(batch_size, 4))]
            z, xa, _ = batch_tensors(few, device)
            with torch.no_grad():
                before = original_models[role](z, xa)
                after = loaded[role](z, xa)
            if not torch.equal(before, after):
                raise RuntimeError("checkpoint reload produces different Q values")
        digest = hashlib.sha256(Path(tmp_name).read_bytes()).hexdigest()
        # Same-directory hard link is atomic NO-CLOBBER. os.replace can silently
        # overwrite a concurrently created immutable model; never use it.
        os.link(tmp_name, destination)
    finally:
        if os.path.exists(tmp_name):
            os.unlink(tmp_name)
    return digest


def train(episodes: list[Episode], by_role: dict[int, list],
          validation: list[Episode], val_roles: dict[int, list], args) -> dict:
    import torch
    from torch.nn import functional as F
    from model import DouZeroQ, MODEL_FORMAT, FEATURE_CONTRACT, batch_tensors
    runtime, device = torch_runtime(args.device, args.seed)
    assert runtime is torch
    if Path(args.output).exists() or Path(args.output).is_symlink():
        raise FileExistsError("immutable checkpoint already exists; refuse optimizer overwrite")
    rng = random.Random(args.seed)
    sampling = getattr(args, "sampling", "transition")
    selection = getattr(args, "checkpoint_policy", "final")
    interval = getattr(args, "validation_interval", 20)
    strict_baseline = getattr(args, "require_validation_advantage", False)
    if sampling not in ("transition", "episode-source-outcome") or \
            selection not in ("final", "best-validation") or interval < 1:
        raise ValueError("invalid DMC sampling/checkpoint policy or evaluation interval")
    samplers = {}
    validation_samplers = {}
    if sampling == "episode-source-outcome":
        from train_policy import make_role_sampler
        train_sources = getattr(args, "_verified_train_sources", None)
        val_sources = getattr(args, "_verified_validation_sources", None)
        if (train_sources is None) != (val_sources is None):
            raise ValueError("incomplete externally verified source mapping")
        if train_sources is None:
            if {e.model_version for e in episodes} != {
                    e.model_version for e in validation}:
                raise ValueError("train/validation source-policy cohorts differ")
        elif set(train_sources.values()) != set(val_sources.values()):
            raise ValueError("TRAIN/VALIDATION verified policy rosters differ")
        # Entire episodes must include WIN and LOSS for EVERY role/source
        # in both independent pools before allocating any role model.
        for role in ROLES:
            samplers[role] = make_role_sampler(
                episodes, role, verified_source_by_episode=train_sources)
            validation_samplers[role] = make_role_sampler(
                validation, role, verified_source_by_episode=val_sources)
    warm = getattr(args, "_verified_warmstart", None)
    if warm is not None:
        models, parent_meta = load_model_checkpoint(
            args.warm_start_checkpoint,
            expected_rules=args.rules_version,
            expected_objective=args.objective,
            expected_model_version=args.warm_start_model_version,
            torch=torch, device=device)
        # A warm start is the EXACT verified frozen source-Q weights, not an
        # independently initialized network trained on its own Replay.
        observed_ids = {e.episode_id for e in episodes + validation}
        old_ids = set(parent_meta["train_episode_ids"]) | set(
            parent_meta["validation_episode_ids"])
        if observed_ids & old_ids or any(
                e.model_version != args.warm_start_model_version
                for e in episodes + validation):
            raise ValueError("warm-start source episodes overlap parent training/tuning or use other Q")
    else:
        models = {role: DouZeroQ(role).to(device) for role in ROLES}
    before = {ROLES[r]: evaluate_rows(models[r], val_roles[r], device,
                                       args.batch_size) for r in ROLES}
    aligned_before = ({ROLES[r]: evaluate_aligned_episodes(
        models[r], validation, r, validation_samplers[r], device, args.batch_size)
                       for r in ROLES} if validation_samplers else None)
    final_train_loss = {}
    best_step = {}
    for role in ROLES:
        model = models[role]
        optimizer = torch.optim.RMSprop(model.parameters(), lr=args.learning_rate,
                                        alpha=0.99, eps=1e-5)
        rows = by_role[role]
        if not rows:
            raise ValueError(f"no training samples for role {ROLES[role]}")
        lowest_mse = float("inf")
        best_weights = None
        selected_at = 0
        for step in range(1, args.steps + 1):
            batch = (samplers[role].batch(rng, args.batch_size)
                     if sampling == "episode-source-outcome" else
                     [rows[rng.randrange(len(rows))] for _ in range(args.batch_size)])
            z, xa, target = batch_tensors(batch, device)
            model.train()
            predicted = model(z, xa)
            loss = F.mse_loss(predicted, target)
            if not torch.isfinite(loss).item():
                raise RuntimeError("nonfinite DMC training loss")
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=40.0,
                                           error_if_nonfinite=True)
            optimizer.step()
            if selection == "best-validation" and (step % interval == 0 or
                                                    step == args.steps):
                metric = (evaluate_aligned_episodes(
                    model, validation, role, validation_samplers[role],
                    device, args.batch_size) if validation_samplers else
                    evaluate_rows(model, val_roles[role], device,
                                  args.batch_size))
                if metric["mse"] < lowest_mse:
                    lowest_mse = metric["mse"]
                    selected_at = step
                    # Clone immutable tensors: state_dict otherwise aliases
                    # continuously updated live optimizer parameters.
                    best_weights = {key: value.detach().clone()
                                    for key, value in model.state_dict().items()}
        if selection == "best-validation":
            if best_weights is None:
                raise RuntimeError("no valid complete-episode validation snapshot")
            model.load_state_dict(best_weights, strict=True)
        else:
            selected_at = args.steps
        final_train_loss[ROLES[role]] = float(loss.detach().cpu())
        best_step[ROLES[role]] = selected_at
        model.eval()
    after = {ROLES[r]: evaluate_rows(models[r], val_roles[r], device,
                                      args.batch_size) for r in ROLES}
    aligned_after = ({ROLES[r]: evaluate_aligned_episodes(
        models[r], validation, r, validation_samplers[r], device, args.batch_size)
                      for r in ROLES} if validation_samplers else None)
    if strict_baseline:
        from train_policy import require_validation_advantage
        require_validation_advantage(aligned_after if aligned_after is not None else after)
    payload = {
        "format": CHECKPOINT_FORMAT,
        "model_architecture": MODEL_FORMAT,
        "feature_contract": FEATURE_CONTRACT,
        "rules_version": args.rules_version,
        "objective": args.objective,
        "source_model_versions": sorted({e.model_version for e in episodes}),
        "candidate_model_version": args.model_version,
        "train_episode_ids": sorted(e.episode_id for e in episodes),
        "validation_episode_ids": sorted(e.episode_id for e in validation),
        "training_steps_per_role": args.steps,
        "seed": args.seed,
        "role_state_dicts": {ROLES[role]: models[role].state_dict() for role in ROLES},
    }
    sha256 = publish_private_checkpoint(Path(args.output), payload, torch,
                                        device, val_roles, models, args.batch_size)
    report = {"checkpoint_sha256": sha256, "candidate_model_version": args.model_version,
              "train_last_batch_mse": final_train_loss,
              "validation_before": before, "validation_after": after,
              "sampling": sampling, "checkpoint_policy": selection,
              "validation_selection_weighting":
                  "episode-source-outcome" if validation_samplers else "transition",
              "selected_validation_step_by_role": best_step,
              "validation_zero_baseline_gate": bool(strict_baseline)}
    if aligned_after is not None:
        report["validation_aligned_before"] = aligned_before
        report["validation_aligned_after"] = aligned_after
    return report


def compare_frozen_complete_games(models_a, models_b,
                                  episodes: list[Episode], device,
                                  batch_size: int) -> dict:
    """Private paired native-Replay Q scoring, WITHOUT revealing game rows."""
    import torch
    from model import batch_tensors
    from train_policy import paired_complete_game_mse
    if batch_size <= 0 or batch_size > 65536:
        raise ValueError("paired scorer batch size out of bounds")
    records = []
    for episode in episodes:
        entry = {"episode_id": episode.episode_id, "roles": {}}
        for role in ROLES:
            rows = [step for step in episode.samples if step.role == role]
            if not rows:
                raise ValueError("paired heldout game missing acting role")
            sum_a = sum_b = sum_zero = 0.0
            for start in range(0, len(rows), batch_size):
                z, xa, targets = batch_tensors(
                    rows[start:start + batch_size], device)
                with torch.no_grad():
                    a = models_a[role](z, xa)
                    b = models_b[role](z, xa)
                    if not torch.isfinite(a).all().item() or \
                            not torch.isfinite(b).all().item():
                        raise ValueError("nonfinite paired Q prediction")
                    sum_a += torch.square(a - targets).sum().item()
                    sum_b += torch.square(b - targets).sum().item()
                    sum_zero += torch.square(targets).sum().item()
            entry["roles"][ROLES[role]] = {
                "count": len(rows),
                "a_mse": sum_a / len(rows),
                "b_mse": sum_b / len(rows),
                "zero_q_mse": sum_zero / len(rows),
            }
        records.append(entry)
    return paired_complete_game_mse(records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--verify-only", action="store_true")
    mode.add_argument("--train", action="store_true")
    mode.add_argument("--evaluate-only", action="store_true")
    mode.add_argument("--compare-frozen", action="store_true",
                      help="private paired held-out Q-MSE for TWO frozen candidates")
    parser.add_argument("--replay", nargs="+",
                        help="legacy direct Replay-v1 files (not accepted with verified R9 cohorts)")
    parser.add_argument("--validation-replay", nargs="+",
                        help="entire validation episodes, disjoint from training")
    parser.add_argument("--rules-version", type=int, default=1)
    parser.add_argument("--objective", choices=sorted(OBJECTIVES), required=True)
    parser.add_argument("--output", help="new immutable private model checkpoint")
    parser.add_argument("--model-version", type=int,
                        help="new frozen model ID, strictly newer than all replay source models")
    parser.add_argument("--checkpoint", help="trusted immutable checkpoint for evaluation")
    parser.add_argument("--compare-checkpoint", help="second distinct immutable checkpoint")
    parser.add_argument("--compare-model-version", type=int,
                        help="second candidate model ID (positive uint64)")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--sampling", choices=("transition", "episode-source-outcome"),
                        default="transition",
                        help="optional whole-game/behavior-source/outcome balanced sampling")
    parser.add_argument("--checkpoint-policy", choices=("final", "best-validation"),
                        default="final",
                        help="restore per-role best validation MSE snapshot before publication")
    parser.add_argument("--validation-interval", type=int, default=20)
    parser.add_argument("--require-validation-advantage", action="store_true",
                        help="reject checkpoint if ANY role fails validation zero-Q baseline")
    # R9b OPTIONAL private source-verifiable training. No TEST paths allowed.
    # Existing single-source transition/final entrypoints remain unchanged.
    parser.add_argument("--behavior-train-cohort", action="append", type=Path)
    parser.add_argument("--behavior-validation-cohort", action="append", type=Path)
    parser.add_argument("--behavior-required-source", action="append",
                        choices=("mixed-v1", "shed-most", "legal-index"))
    parser.add_argument("--behavior-source-revision")
    parser.add_argument("--behavior-generator-sha256")
    parser.add_argument("--behavior-learner-revision",
                        help="pinned exact clean Python learner Git HEAD (R9 only)")
    # R9d explicit TRUE warm-start from a frozen v3 parent. The source C11
    # Replay-v1 model_version MUST be that parent version for all seats.
    parser.add_argument("--warm-start-checkpoint", type=Path)
    parser.add_argument("--warm-start-model-version", type=int)
    parser.add_argument("--warm-train-dir", type=Path)
    parser.add_argument("--warm-validation-dir", type=Path)
    parser.add_argument("--warm-source-revision")
    parser.add_argument("--warm-native-library-sha256")
    args = parser.parse_args()
    source_requested = any((
        args.behavior_train_cohort, args.behavior_validation_cohort,
        args.behavior_required_source, args.behavior_source_revision,
        args.behavior_generator_sha256, args.behavior_learner_revision,
    ))
    warm_requested = any((
        args.warm_start_checkpoint is not None,
        args.warm_start_model_version is not None,
        args.warm_train_dir is not None,
        args.warm_validation_dir is not None,
        args.warm_source_revision,
        args.warm_native_library_sha256,
    ))
    if warm_requested:
        if (source_requested or not args.train or args.replay or
                args.validation_replay or not args.warm_start_checkpoint or
                args.warm_start_model_version is None or
                not args.warm_train_dir or not args.warm_validation_dir or
                not args.warm_source_revision or
                not args.warm_native_library_sha256 or
                args.objective != "win-sign" or args.rules_version != 1 or
                args.sampling != "episode-source-outcome" or
                args.checkpoint_policy != "best-validation" or
                not args.require_validation_advantage or
                args.model_version is None or
                type(args.warm_start_model_version) is not int or
                args.warm_start_model_version < 2 or
                args.model_version <= args.warm_start_model_version):
            parser.error("R9d warm-start requires ONLY verified private C11 "
                         "on-policy TRAIN/TUNING roots, exact parent v3 checkpoint, "
                         "newer candidate model, balanced source/validation and zero-Q gate")
    if not source_requested and not warm_requested and not args.replay:
        parser.error("legacy learner modes require explicit --replay files")
    if source_requested and (args.replay or args.validation_replay):
        parser.error("R9 verified source mode accepts cohort roots ONLY, not "
                     "separately supplied Replay file paths")
    if args.rules_version <= 0 or args.steps <= 0 or args.batch_size <= 0 or \
            not (0.0 < args.learning_rate <= 1.0) or args.validation_interval < 1:
        parser.error("positive version/steps/batch-size/interval and 0<lr<=1 required")
    if not args.train and (args.sampling != "transition" or
                           args.checkpoint_policy != "final" or
                           args.require_validation_advantage):
        parser.error("optimizer sampling and checkpoint-selection options require --train")
    if args.train and ((not args.validation_replay and not source_requested
                        and not warm_requested) or
                       not args.output or args.checkpoint or
                       args.model_version is None):
        parser.error("--train requires validated tuning data, --output and --model-version")
    if args.evaluate_only and (not args.checkpoint or args.output or args.validation_replay
                               or args.model_version is None):
        parser.error("--evaluate-only requires --checkpoint and --model-version, no output/validation-replay")
    if args.compare_frozen and (not args.checkpoint or not args.compare_checkpoint
                                or args.model_version is None or
                                args.compare_model_version is None or
                                args.model_version == args.compare_model_version or
                                args.output or args.validation_replay):
        parser.error("--compare-frozen needs TWO distinct frozen checkpoints/model IDs; no optimizer outputs")
    if not args.compare_frozen and (args.compare_checkpoint is not None or
                                    args.compare_model_version is not None):
        parser.error("second checkpoint/model version requires --compare-frozen")
    if args.model_version is not None and not 0 < args.model_version < 2**64:
        parser.error("model-version must fit a positive uint64")
    if args.compare_model_version is not None and not (
            0 < args.compare_model_version < 2**64):
        parser.error("compare-model-version must fit a positive uint64")
    if args.verify_only and (args.output or args.checkpoint or
                             args.model_version is not None):
        parser.error("--verify-only does not accept model checkpoint paths or model version")
    verified_behavior = None
    verified_warm = None
    if source_requested:
        if (not args.train or not args.behavior_train_cohort or
                not args.behavior_validation_cohort or
                not args.behavior_required_source or
                not args.behavior_source_revision or
                not args.behavior_generator_sha256 or
                not args.behavior_learner_revision or
                args.objective != "win-sign" or
                args.sampling != "episode-source-outcome" or
                args.checkpoint_policy != "best-validation" or
                not args.require_validation_advantage):
            parser.error("verified multi-source mode requires --train, complete "
                         "pinned TRAIN/VALIDATION cohorts, episode-source-outcome, "
                         "best-validation and strict zero-Q admission")
        # This source-to-policy mapping MUST come from actual sealed C11
        # private manifests. Never infer policy from numeric model_version=1.
        # Crucially, the virgin TEST files are NOT opened by this mode.
        from verified_behavior_training import (
            admit_training_sources, verify_exact_learner_revision,
        )
        verify_exact_learner_revision(
            args.behavior_learner_revision, Path(__file__).resolve().parents[2])
        verified_behavior = admit_training_sources(
            train_cohorts=args.behavior_train_cohort,
            validation_cohorts=args.behavior_validation_cohort,
            required_behaviors=args.behavior_required_source,
            source_revision=args.behavior_source_revision,
            generator_sha256=args.behavior_generator_sha256,
            objective=args.objective,
        )
        # Construct canonical file rosters exclusively from checked private
        # cohort manifests, not operator-provided per-episode CLI paths.
        # This also avoids OS ARG_MAX when a pool has thousands of games.
        args.replay = verified_behavior["train_files"]
        args.validation_replay = verified_behavior["validation_files"]
        args._verified_train_sources = verified_behavior["train_source_by_episode"]
        args._verified_validation_sources = (
            verified_behavior["validation_source_by_episode"])
    if warm_requested:
        from warmstart_source import admit_warmstart_source
        verified_warm = admit_warmstart_source(
            parent_checkpoint=args.warm_start_checkpoint,
            parent_model_version=args.warm_start_model_version,
            native_library_sha256=args.warm_native_library_sha256,
            source_revision=args.warm_source_revision,
            train_root=args.warm_train_dir,
            validation_root=args.warm_validation_dir,
        )
        if args.model_version <= verified_warm["parent_model_version"]:
            raise ValueError("new warm-start candidate model-version must be strictly newer")
        args._verified_warmstart = verified_warm
    if verified_behavior is not None:
        # Use precisely the in-memory snapshots that were SHA-validated by
        # the cohort verifier. Never reopen paths after admission: external
        # owner-side changes must not swap optimizer inputs between checks.
        episodes = list(verified_behavior["train_episodes"])
        by_role = {r: [s for ep in episodes for s in ep.samples if s.role == r]
                   for r in ROLES}
    elif verified_warm is not None:
        episodes = list(verified_warm["train_episodes"])
        by_role = {r: [s for ep in episodes for s in ep.samples if s.role == r]
                   for r in ROLES}
    else:
        episodes, by_role = load_checked(args.replay, args.rules_version,
                                         OBJECTIVES[args.objective])
    if args.train and args.model_version <= max(e.model_version for e in episodes):
        raise ValueError("candidate model-version must exceed every Replay source model version")
    report = {
        "episodes": len(episodes),
        "completed_steps": sum(len(e.samples) for e in episodes),
        "rules_version": args.rules_version,
        "objective": args.objective,
        "role_samples": {ROLES[r]: len(by_role[r]) for r in ROLES},
        "source_model_versions": sorted({e.model_version for e in episodes}),
    }
    if verified_behavior is not None:
        report["verified_behavior_sources"] = verified_behavior["sources"]
        report["source_manifest_bundle_sha256"] = (
            verified_behavior["manifest_bundle_sha256"])
        report["heldout_test_opened_for_training"] = False
        report["source_provenance_is_not_signed_attestation"] = True
        report["qualified_gameplay_skill"] = False
    if verified_warm is not None:
        report["warm_start_parent_model_version"] = verified_warm["parent_model_version"]
        report["warm_start_parent_checkpoint_sha256"] = verified_warm[
            "parent_checkpoint_sha256"]
        report["onpolicy_source_manifest_bundle_sha256"] = verified_warm[
            "onpolicy_source_manifest_bundle_sha256"]
        report["heldout_test_opened_for_training"] = False
        report["qualified_gameplay_skill"] = False
    if args.validation_replay or verified_warm is not None:
        if verified_behavior is not None:
            val = list(verified_behavior["validation_episodes"])
            roles = {r: [s for ep in val for s in ep.samples if s.role == r]
                     for r in ROLES}
        elif verified_warm is not None:
            val = list(verified_warm["validation_episodes"])
            roles = {r: [s for ep in val for s in ep.samples if s.role == r]
                     for r in ROLES}
        else:
            val, roles = load_checked(args.validation_replay, args.rules_version,
                                      OBJECTIVES[args.objective])
        require_disjoint(episodes, val)
        report["validation_episodes"] = len(val)
        report["validation_steps"] = sum(len(e.samples) for e in val)
        if args.train:
            report["optimizer"] = train(episodes, by_role, val, roles, args)
            if verified_warm is not None:
                from warmstart_source import publish_lineage
                report["warmstart_lineage"] = publish_lineage(
                    Path(args.output), args.warm_start_checkpoint,
                    verified_warm, args.model_version, report["optimizer"])
            if verified_behavior is not None:
                # Legacy checkpoint ABI v3 contains only numeric source IDs.
                # R9 needs separate, digest-bound actual behavior identities.
                from model_source_record import make_record, publish_record
                selected = report["optimizer"]
                record = make_record(
                    checkpoint_sha256=selected["checkpoint_sha256"],
                    model_version=args.model_version,
                    source_revision=args.behavior_source_revision,
                    learner_revision=args.behavior_learner_revision,
                    generator_sha256=args.behavior_generator_sha256,
                    source_bundle_sha256=verified_behavior[
                        "manifest_bundle_sha256"],
                    sources=verified_behavior["sources"],
                    train_count=verified_behavior["episodes_by_split"]["train"],
                    tuning_count=verified_behavior["episodes_by_split"]["validation"],
                    steps_by_role=selected["selected_validation_step_by_role"],
                    aligned_by_role=selected["validation_aligned_after"],
                )
                report["immutable_verified_source_record"] = publish_record(
                    args.output, record)
    elif args.evaluate_only:
        torch, device = torch_runtime(args.device, args.seed)
        models, payload = load_model_checkpoint(args.checkpoint,
                                expected_rules=args.rules_version,
                                expected_objective=args.objective,
                                expected_model_version=args.model_version,
                                torch=torch, device=device)
        for role in ROLES:
            if not by_role[role]:
                raise ValueError("evaluation missing one role")
        report["candidate_model_version"] = payload["candidate_model_version"]
        report["checkpoint_training_episode_ids"] = payload["train_episode_ids"]
        report["checkpoint_validation_episode_ids"] = payload["validation_episode_ids"]
        excluded = set(payload["train_episode_ids"]) | set(payload["validation_episode_ids"])
        if {e.episode_id for e in episodes} & excluded:
            raise ValueError("independent evaluation must exclude training and tuning-validation episodes")
        report["metrics"] = {ROLES[r]: evaluate_rows(models[r], by_role[r],
                             device, args.batch_size) for r in ROLES}
    elif args.compare_frozen:
        torch, device = torch_runtime(args.device, args.seed)
        first, one = load_model_checkpoint(
            args.checkpoint, expected_rules=args.rules_version,
            expected_objective=args.objective,
            expected_model_version=args.model_version,
            torch=torch, device=device)
        second, two = load_model_checkpoint(
            args.compare_checkpoint, expected_rules=args.rules_version,
            expected_objective=args.objective,
            expected_model_version=args.compare_model_version,
            torch=torch, device=device)
        if not all(by_role[r] for r in ROLES):
            raise ValueError("paired heldout missing an actor role")
        observed = {e.episode_id for e in episodes}
        for payload in (one, two):
            excluded = set(payload["train_episode_ids"]) | set(
                payload["validation_episode_ids"])
            if observed & excluded:
                raise ValueError("paired heldout leaks training/tuning episodes")
            if any(e.model_version >= payload["candidate_model_version"]
                   for e in episodes):
                raise ValueError("paired model not newer than Replay behavior source")
        report["candidate_model_versions"] = [
            one["candidate_model_version"], two["candidate_model_version"]]
        report["paired_complete_game_mse"] = compare_frozen_complete_games(
            first, second, episodes, device, args.batch_size)
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
