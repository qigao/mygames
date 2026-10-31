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
    if sampling == "episode-source-outcome":
        from train_policy import make_role_sampler
        if {e.model_version for e in episodes} != {e.model_version for e in validation}:
            raise ValueError("train/validation source-policy cohorts differ")
        # Entire episodes must include WIN and LOSS for EVERY role/source
        # in both independent pools before allocating any role model.
        for role in ROLES:
            samplers[role] = make_role_sampler(episodes, role)
            make_role_sampler(validation, role)
    models = {role: DouZeroQ(role).to(device) for role in ROLES}
    before = {ROLES[r]: evaluate_rows(models[r], val_roles[r], device,
                                       args.batch_size) for r in ROLES}
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
                metric = evaluate_rows(model, val_roles[role], device,
                                       args.batch_size)
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
    if strict_baseline:
        from train_policy import require_validation_advantage
        require_validation_advantage(after)
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
    return {"checkpoint_sha256": sha256, "candidate_model_version": args.model_version,
            "train_last_batch_mse": final_train_loss,
            "validation_before": before, "validation_after": after,
            "sampling": sampling, "checkpoint_policy": selection,
            "selected_validation_step_by_role": best_step,
            "validation_zero_baseline_gate": bool(strict_baseline)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--verify-only", action="store_true")
    mode.add_argument("--train", action="store_true")
    mode.add_argument("--evaluate-only", action="store_true")
    parser.add_argument("--replay", nargs="+", required=True,
                        help="private, complete native C11 DMC Replay v1 files")
    parser.add_argument("--validation-replay", nargs="+",
                        help="entire validation episodes, disjoint from training")
    parser.add_argument("--rules-version", type=int, default=1)
    parser.add_argument("--objective", choices=sorted(OBJECTIVES), required=True)
    parser.add_argument("--output", help="new immutable private model checkpoint")
    parser.add_argument("--model-version", type=int,
                        help="new frozen model ID, strictly newer than all replay source models")
    parser.add_argument("--checkpoint", help="trusted immutable checkpoint for evaluation")
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
    args = parser.parse_args()
    if args.rules_version <= 0 or args.steps <= 0 or args.batch_size <= 0 or \
            not (0.0 < args.learning_rate <= 1.0) or args.validation_interval < 1:
        parser.error("positive version/steps/batch-size/interval and 0<lr<=1 required")
    if not args.train and (args.sampling != "transition" or
                           args.checkpoint_policy != "final" or
                           args.require_validation_advantage):
        parser.error("optimizer sampling and checkpoint-selection options require --train")
    if args.train and (not args.validation_replay or not args.output or args.checkpoint
                       or args.model_version is None):
        parser.error("--train requires --validation-replay, --output and --model-version")
    if args.evaluate_only and (not args.checkpoint or args.output or args.validation_replay
                               or args.model_version is None):
        parser.error("--evaluate-only requires --checkpoint and --model-version, no output/validation-replay")
    if args.model_version is not None and not 0 < args.model_version < 2**64:
        parser.error("model-version must fit a positive uint64")
    if args.verify_only and (args.output or args.checkpoint or
                             args.model_version is not None):
        parser.error("--verify-only does not accept model checkpoint paths or model version")
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
    if args.validation_replay:
        val, roles = load_checked(args.validation_replay, args.rules_version,
                                  OBJECTIVES[args.objective])
        require_disjoint(episodes, val)
        report["validation_episodes"] = len(val)
        report["validation_steps"] = sum(len(e.samples) for e in val)
        if args.train:
            report["optimizer"] = train(episodes, by_role, val, roles, args)
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
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
