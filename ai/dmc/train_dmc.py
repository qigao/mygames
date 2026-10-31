#!/usr/bin/env python3
"""Offline three-role DouZero-style Deep Monte Carlo learner for MyGames.

Authoritative cards, actions and full-terminal rewards originate in native C11
MyGames. This trainer has NO independent game rules or HTTP/Room Owner access.
Private Replay inputs must come from an explicitly authorized offline source.
Requires PyTorch only for --train. Never silently selects a fallback engine.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import random
import stat
import tempfile
from typing import Sequence

from replay import (
    OBJECTIVE_GAME_POINTS, OBJECTIVE_WIN_SIGN,
    Episode, read_episode,
)

ROLES = {0: "landlord", 1: "landlord_up", 2: "landlord_down"}
OBJECTIVES = {
    "win-sign": OBJECTIVE_WIN_SIGN,
    "game-points": OBJECTIVE_GAME_POINTS,
}


def load_checked(paths: Sequence[str], rules_version: int,
                 objective: int) -> tuple[list[Episode], dict[int, list]]:
    episodes: list[Episode] = []
    by_role: dict[int, list] = {0: [], 1: [], 2: []}
    seen: set[int] = set()
    for path in paths:
        episode = read_episode(path)
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


def train(episodes: list[Episode], by_role: dict[int, list], args) -> dict:
    try:
        import torch
        from torch import nn
        from torch.nn import functional as F
    except ImportError as exc:
        raise RuntimeError("--train requires the configured PyTorch backend") from exc

    if args.device.startswith("cuda") and not torch.cuda.is_available():
        raise RuntimeError("requested CUDA backend unavailable; refusing CPU fallback")
    if args.device not in ("cpu", "cuda") and not args.device.startswith("cuda:"):
        raise ValueError("device must be cpu, cuda or cuda:<index>")
    torch.manual_seed(args.seed)
    if args.device == "cpu":
        torch.set_num_threads(min(4, os.cpu_count() or 1))
    device = torch.device(args.device)

    class DouZeroQ(nn.Module):
        def __init__(self, obs_dim: int) -> None:
            super().__init__()
            self.lstm = nn.LSTM(162, 128, batch_first=True)
            self.fc1 = nn.Linear(obs_dim + 54 + 128, 512)
            self.fc2 = nn.Linear(512, 512)
            self.fc3 = nn.Linear(512, 512)
            self.fc4 = nn.Linear(512, 512)
            self.fc5 = nn.Linear(512, 512)
            self.fc6 = nn.Linear(512, 1)

        def forward(self, z, xa):
            context = self.lstm(z)[0][:, -1, :]
            h = torch.cat((context, xa), dim=-1)
            for layer in (self.fc1, self.fc2, self.fc3, self.fc4, self.fc5):
                h = torch.relu(layer(h))
            return self.fc6(h).squeeze(-1)

    rng = random.Random(args.seed)
    models = {}
    final_losses = {}
    for role, rows in by_role.items():
        if not rows:
            raise ValueError(f"no training samples for role {ROLES[role]}")
        models[role] = DouZeroQ(319 if role == 0 else 430).to(device)
    # Explicit optimization for each of three independently conditioned
    # positions, following DouZero's per-role DMC target rather than an
    # actor-mixed scalar reward. No rollouts or fake gameplay here.
    for role in range(3):
        model = models[role]
        optimizer = torch.optim.RMSprop(model.parameters(),
                                        lr=args.learning_rate,
                                        alpha=0.99, eps=1e-5)
        rows = by_role[role]
        for _ in range(args.steps):
            sampled = [rows[rng.randrange(len(rows))]
                       for _i in range(args.batch_size)]
            x = torch.tensor(
                [list(step.x) + list(step.action) for step in sampled],
                dtype=torch.float32, device=device
            )
            z = torch.tensor(
                [list(step.z) for step in sampled],
                dtype=torch.float32, device=device
            ).reshape(args.batch_size, 5, 162)
            targets = torch.tensor(
                [step.target for step in sampled],
                dtype=torch.float32, device=device
            )
            predicted = model(z, x)
            loss = F.mse_loss(predicted, targets)
            if not torch.isfinite(loss).item():
                raise RuntimeError(f"nonfinite DMC loss for role {ROLES[role]}")
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), max_norm=40.0)
            optimizer.step()
        model.eval()
        final_losses[ROLES[role]] = float(loss.detach().cpu())

    if not args.output:
        raise ValueError("--output is required for --train")
    destination = Path(args.output)
    if destination.exists():
        raise FileExistsError("checkpoint already exists; refuse overwrite")
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if os.name == "posix" and stat.S_IMODE(destination.parent.stat().st_mode) & 0o077:
        raise PermissionError("checkpoint directory must not be group/world accessible")
    fd, temp_name = tempfile.mkstemp(prefix=".mygames-dmc-", dir=destination.parent)
    try:
        with os.fdopen(fd, "wb") as file:
            torch.save({
                "format": "mygames-douzero-dmc-v1",
                "rules_version": args.rules_version,
                "objective": args.objective,
                "feature_contract": "ddz-dmc-replay-v1",
                "training_steps_per_role": args.steps,
                "source_model_versions": sorted({e.model_version for e in episodes}),
                "state_dicts": {ROLES[role]: models[role].state_dict()
                                for role in range(3)},
            }, file)
        os.replace(temp_name, destination)
    finally:
        if os.path.exists(temp_name):
            os.unlink(temp_name)
    return final_losses


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--verify-only", action="store_true")
    mode.add_argument("--train", action="store_true")
    parser.add_argument("--replay", nargs="+", required=True,
                        help="private, completed C11 DMC replay v1 files")
    parser.add_argument("--rules-version", type=int, default=1)
    parser.add_argument("--objective", choices=sorted(OBJECTIVES), required=True)
    parser.add_argument("--output", type=str, help="new private checkpoint file")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--device", type=str, default="cpu")
    args = parser.parse_args()
    if args.rules_version <= 0 or args.steps <= 0 or args.batch_size <= 0 or \
            not (0.0 < args.learning_rate <= 1.0):
        parser.error("positive rules-version/steps/batch-size and 0<lr<=1 required")
    episodes, by_role = load_checked(
        args.replay, args.rules_version, OBJECTIVES[args.objective]
    )
    report = {
        "episodes": len(episodes),
        "completed_steps": sum(len(e.samples) for e in episodes),
        "rules_version": args.rules_version,
        "objective": args.objective,
        "role_samples": {ROLES[r]: len(by_role[r]) for r in range(3)},
        "source_model_versions": sorted({e.model_version for e in episodes}),
    }
    if args.train:
        report["loss_mse"] = train(episodes, by_role, args)
        report["checkpoint_created"] = True
    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
