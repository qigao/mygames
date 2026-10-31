"""Strict, OFFLINE frozen PyTorch score-provider for MyGames C11 Q ABI.

Accepts ONLY role-specific canonical numeric feature arrays, not game state,
private match handles or raw player/replay files. A native integration must
first validate the COMPLETE C11 legal action list with DMCQSelector and fence
the selected model/rules/turn independently. No fallback model or rules engine.
"""
from __future__ import annotations

import math
from pathlib import Path
from typing import Sequence

from model import ROLE_DIMENSIONS
from train_dmc import load_model_checkpoint, torch_runtime

BATCH_LIMIT = 64
HISTORY_DIM = 810
ACTION_DIM = 54


def _binary_features(value: bytes, required: int, name: str) -> bytes:
    if type(value) is not bytes or len(value) != required or any(
            v not in (0, 1) for v in value):
        raise ValueError(f"{name} must be exactly {required} binary feature bytes")
    return value


class FrozenQScorer:
    """One immutable three-role checkpoint. Never mutate or retrain models."""

    def __init__(self, checkpoint: str | Path, *, rules_version: int,
                 objective: str, model_version: int, device: str = "cpu") -> None:
        if type(model_version) is not int or not 0 < model_version < 2**64:
            raise ValueError("required model_version must be a positive uint64")
        if type(rules_version) is not int or rules_version <= 0:
            raise ValueError("required rules_version must be positive")
        torch, resolved = torch_runtime(device, seed=1)
        self._torch = torch
        self._device = resolved
        self._models, self._provenance = load_model_checkpoint(
            checkpoint, expected_rules=rules_version,
            expected_objective=objective,
            expected_model_version=model_version,
            torch=torch, device=resolved,
        )
        self.model_version = model_version
        self.rules_version = rules_version
        self.objective = objective

    def score_batch(self, role: int, x: bytes, z: bytes,
                    actions: Sequence[bytes]) -> list[float]:
        if type(role) is not int or role not in ROLE_DIMENSIONS:
            raise ValueError("unknown DMC role")
        _binary_features(x, ROLE_DIMENSIONS[role], "state")
        _binary_features(z, HISTORY_DIM, "history")
        if not isinstance(actions, (list, tuple)) or not 1 <= len(actions) <= BATCH_LIMIT:
            raise ValueError("Q callback requires 1..64 explicit actions")
        for action in actions:
            _binary_features(action, ACTION_DIM, "action")
        torch = self._torch
        count = len(actions)
        with torch.inference_mode():
            history = torch.tensor(list(z), dtype=torch.float32,
                                   device=self._device).reshape(1, 5, 162)
            history = history.expand(count, -1, -1).contiguous()
            state_action = torch.tensor(
                [list(x) + list(a) for a in actions], dtype=torch.float32,
                device=self._device,
            )
            values = self._models[role](history, state_action)
            if values.shape != (count,) or not torch.isfinite(values).all().item():
                raise RuntimeError("frozen Q model returned incomplete/nonfinite scores")
            result = [float(q) for q in values.cpu().tolist()]
        if not all(math.isfinite(q) for q in result):
            raise RuntimeError("nonfinite frozen Q output")
        return result
