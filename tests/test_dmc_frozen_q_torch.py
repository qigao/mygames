"""Manual configured-PyTorch smoke on SYNTHETIC numeric features, NOT DDZ skill."""
import os
import sys
import tempfile
import math
from pathlib import Path
from types import SimpleNamespace

import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ai/dmc"))
import train_dmc
from model import batch_tensors
from frozen_q import FrozenQScorer
from replay import Episode, Sample


def make(ep_id, winner):
    steps = []
    for seat, role in ((0, 0), (1, 2), (2, 1)):
        dim = 319 if role == 0 else 430
        x = bytearray(dim)
        x[(ep_id + seat) % dim] = 1
        z = bytearray(810)
        z[(ep_id + 3 * seat) % 810] = 1
        action = bytearray(54)
        action[(ep_id + 2 * seat) % 54] = 1
        team = (seat == 0) == (winner == 0)
        steps.append(Sample(
            seat + 1, 1 << ((ep_id + 2 * seat) % 54), seat, role,
            bytes(x), bytes(z), bytes(action),
            1.0 if team else -1.0,
        ))
    return Episode(ep_id, 1, 1, 1, 0, winner, tuple(steps))


def by_role(episodes):
    return {role: [s for e in episodes for s in e.samples if s.role == role]
            for role in (0, 1, 2)}


def main():
    training = [make(101, 0), make(102, 1)]
    validation = [make(103, 1), make(104, 0)]
    with tempfile.TemporaryDirectory() as private:
        os.chmod(private, 0o700)
        checkpoint = Path(private) / "test-frozen-v3.pt"
        args = SimpleNamespace(
            device="cpu", seed=7, steps=2, batch_size=3,
            learning_rate=0.0001, rules_version=1, objective="win-sign",
            model_version=2, output=str(checkpoint),
        )
        metrics = train_dmc.train(
            training, by_role(training), validation, by_role(validation), args
        )
        assert metrics["candidate_model_version"] == 2
        provider = FrozenQScorer(
            checkpoint, rules_version=1, objective="win-sign", model_version=2
        )
        for role in (0, 1, 2):
            row = by_role(validation)[role][0]
            predicted = provider.score_batch(role, row.x, row.z, [row.action])
            z, x, _ = batch_tensors([row], torch.device("cpu"))
            with torch.inference_mode():
                direct = provider._models[role](z, x).tolist()
            assert predicted == direct, (role, predicted, direct)
            assert all(math.isfinite(value) for value in predicted)
            try:
                provider.score_batch(role, row.x, row.z, [row.action] * 65)
            except ValueError:
                pass
            else:
                raise AssertionError("accepted an oversized Q batch")

        for model_version, rules_version, objective in (
            (1, 1, "win-sign"), (2, 2, "win-sign"), (2, 1, "game-points")
        ):
            try:
                FrozenQScorer(
                    checkpoint, rules_version=rules_version,
                    objective=objective, model_version=model_version
                )
            except ValueError:
                pass
            else:
                raise AssertionError("accepted incompatible immutable snapshot")
        print("mygames.dmc_torch_local: 3/3 roles, 2 real CPU gradient "
              "updates per role, bit-exact reload/Q parity; "
              "SYNTHETIC FEATURES ONLY")


if __name__ == "__main__":
    main()
