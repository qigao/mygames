"""Versioned DouZero-style action-conditioned Q model for offline MyGames DMC.

Only the trusted offline learner/inference adapter imports torch. Pure C11
rules, Replay verification and production Room/Game Owners do not depend on it.
"""
from __future__ import annotations

import torch
from torch import nn

MODEL_FORMAT = "mygames-douzero-q-v1"
FEATURE_CONTRACT = "ddz-dmc-replay-v1"
ROLES = {0: "landlord", 1: "landlord_up", 2: "landlord_down"}
ROLE_DIMENSIONS = {0: 319, 1: 430, 2: 430}


class DouZeroQ(nn.Module):
    """LSTM(5x162 ->128) plus role-conditioned action-Q MLP."""

    def __init__(self, role: int) -> None:
        super().__init__()
        if role not in ROLE_DIMENSIONS:
            raise ValueError(f"unknown role: {role}")
        self.role = role
        self.lstm = nn.LSTM(162, 128, batch_first=True)
        self.fc1 = nn.Linear(ROLE_DIMENSIONS[role] + 54 + 128, 512)
        self.fc2 = nn.Linear(512, 512)
        self.fc3 = nn.Linear(512, 512)
        self.fc4 = nn.Linear(512, 512)
        self.fc5 = nn.Linear(512, 512)
        self.fc6 = nn.Linear(512, 1)

    def forward(self, z: torch.Tensor, xa: torch.Tensor) -> torch.Tensor:
        if z.ndim != 3 or z.shape[1:] != (5, 162):
            raise ValueError("history input must be batch x 5 x 162")
        if xa.ndim != 2 or xa.shape != (z.shape[0], ROLE_DIMENSIONS[self.role] + 54):
            raise ValueError("candidate state/action feature dimension mismatch")
        context = self.lstm(z)[0][:, -1, :]
        h = torch.cat((context, xa), dim=-1)
        for layer in (self.fc1, self.fc2, self.fc3, self.fc4, self.fc5):
            h = torch.relu(layer(h))
        return self.fc6(h).squeeze(-1)


def batch_tensors(rows, device: torch.device):
    """Memory-bounded, no extra numeric dependency for verified Replay samples."""
    if not rows:
        raise ValueError("empty DMC batch")
    role = rows[0].role
    if role not in ROLE_DIMENSIONS or any(r.role != role for r in rows):
        raise ValueError("mixing role-specific Q input dimensions")
    dim = ROLE_DIMENSIONS[role]
    if any(len(r.x) != dim or len(r.z) != 810 or len(r.action) != 54 for r in rows):
        raise ValueError("invalid DMC batch feature lengths")
    x = torch.tensor([list(r.x) + list(r.action) for r in rows],
                     dtype=torch.float32, device=device)
    z = torch.tensor([list(r.z) for r in rows], dtype=torch.float32,
                     device=device).reshape(len(rows), 5, 162)
    target = torch.tensor([r.target for r in rows], dtype=torch.float32, device=device)
    return z, x, target
