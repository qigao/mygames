"""Opt-in local Torch↔C11 Q parity on SYNTHETIC feature inputs only.

Requires explicitly configured PyTorch, NumPy and a C11 compiler. The
C11 reference module is compiled directly for numeric parity; no installer,
consumer CMake project, second DDZ rules runtime or model download.
"""
from __future__ import annotations

import ctypes as C
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai/dmc"))
from model import DouZeroQ, ROLE_DIMENSIONS, ROLES, MODEL_FORMAT, FEATURE_CONTRACT
from train_dmc import CHECKPOINT_FORMAT
from export_q_cpu import export_checkpoint


class NativeModel(C.Structure):
    _fields_ = [
        ("weights", C.POINTER(C.c_float)), ("weight_count", C.c_size_t),
        ("model_version", C.c_uint64), ("rules_version", C.c_uint32),
        ("objective", C.c_uint32), ("role", C.c_uint32), ("x_count", C.c_uint32),
    ]


def load_native_lib(path):
    lib = C.CDLL(str(path))
    lib.mygames_ddz_q_cpu_load.argtypes = [
        C.c_void_p, C.c_size_t, C.c_uint64, C.c_uint32, C.c_uint32, C.c_uint32,
        C.POINTER(C.c_float), C.c_size_t, C.POINTER(NativeModel),
        C.POINTER(C.c_size_t)]
    lib.mygames_ddz_q_cpu_load.restype = C.c_int
    lib.mygames_ddz_q_cpu_score.argtypes = [
        C.POINTER(NativeModel), C.POINTER(C.c_int8), C.c_size_t,
        C.POINTER(C.c_int8), C.POINTER(C.c_int8), C.c_size_t,
        C.POINTER(C.c_float)]
    lib.mygames_ddz_q_cpu_score.restype = C.c_int
    return lib


def main():
    torch.manual_seed(42)
    torch.set_num_threads(2)
    np.random.seed(43)
    with tempfile.TemporaryDirectory(prefix="mygames-q-cpu-") as private:
        root = Path(private)
        os.chmod(root, 0o700)
        weights = root / "weights"
        weights.mkdir(mode=0o700)
        models = {r: DouZeroQ(r).to("cpu") for r in range(3)}
        # Actual optimizer update in each role, but with SYNTHETIC features.
        for role, model in models.items():
            optim = torch.optim.RMSprop(model.parameters(), lr=0.0001)
            z = torch.randint(0, 2, (3, 5, 162)).float()
            x = torch.randint(0, 2, (3, ROLE_DIMENSIONS[role] + 54)).float()
            targets = torch.tensor([1., -1., 1.])
            loss = ((model(z, x) - targets) ** 2).mean()
            optim.zero_grad()
            loss.backward()
            optim.step()
            model.eval()
        checkpoint = root / "synthetic-source-v3.pt"
        torch.save({
            "format": CHECKPOINT_FORMAT,
            "model_architecture": MODEL_FORMAT,
            "feature_contract": FEATURE_CONTRACT,
            "rules_version": 1, "objective": "win-sign",
            "source_model_versions": [1], "candidate_model_version": 2,
            "train_episode_ids": [100], "validation_episode_ids": [101],
            "training_steps_per_role": 1, "seed": 42,
            "role_state_dicts": {ROLES[r]: models[r].state_dict() for r in range(3)},
        }, checkpoint)
        export_checkpoint(checkpoint, weights, rules_version=1,
                          objective="win-sign", model_version=2)
        shared = root / "libmygames_q_cpu.so"
        subprocess.run([
            "cc", "-std=c11", "-O2", "-fPIC", "-shared",
            "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "-I" + str(ROOT / "ai/dmc/include"),
            str(ROOT / "ai/dmc/src/ddz_dmc_q_cpu.c"),
            "-lm", "-o", str(shared)
        ], check=True)
        lib = load_native_lib(shared)
        report = {}
        for role, model in models.items():
            data = (weights / f"q-f32-role-{role}-v2.bin").read_bytes()
            blob = C.create_string_buffer(data)
            required = C.c_size_t(0)
            assert lib.mygames_ddz_q_cpu_load(
                blob, len(data), 2, 1, 1, role,
                None, 0, None, C.byref(required)) == 0
            arena = (C.c_float * required.value)()
            native = NativeModel()
            assert lib.mygames_ddz_q_cpu_load(
                blob, len(data), 2, 1, 1, role, arena,
                required.value, C.byref(native), C.byref(required)) == 0
            z = np.random.randint(0, 2, size=810, dtype=np.int8)
            x = np.random.randint(0, 2, size=ROLE_DIMENSIONS[role], dtype=np.int8)
            actions = np.random.randint(0, 2, size=(4, 54), dtype=np.int8)
            found = (C.c_float * len(actions))()
            assert lib.mygames_ddz_q_cpu_score(
                C.byref(native),
                x.ctypes.data_as(C.POINTER(C.c_int8)), len(x),
                z.ctypes.data_as(C.POINTER(C.c_int8)),
                actions.ctypes.data_as(C.POINTER(C.c_int8)),
                len(actions), found) == 0
            with torch.inference_mode():
                tz = torch.tensor(z, dtype=torch.float32).reshape(
                    1, 5, 162).expand(4, -1, -1)
                tx = torch.tensor(
                    np.concatenate([np.tile(x, (4, 1)), actions], axis=1),
                    dtype=torch.float32)
                expected = model(tz, tx).detach().numpy()
            actual = np.array(list(found), dtype=np.float32)
            difference = np.abs(expected - actual)
            report[ROLES[role]] = {
                "worst_abs": float(difference.max()),
                "candidate_actions": len(actions)}
            np.testing.assert_allclose(actual, expected, rtol=4e-3, atol=5e-4)
            corrupted = bytearray(data)
            corrupted[-12] ^= 1
            wrong = C.create_string_buffer(bytes(corrupted))
            assert lib.mygames_ddz_q_cpu_load(
                wrong, len(corrupted), 2, 1, 1, role, arena,
                required.value, C.byref(native), C.byref(required)) == 2
            assert native.model_version == 2
            assert lib.mygames_ddz_q_cpu_load(
                blob, len(data), 3, 1, 1, role, arena,
                required.value, C.byref(native), C.byref(required)) == 4
        print("LOCAL SYNTHETIC PyTorch/C11 Q parity:", json.dumps(report))


if __name__ == "__main__":
    main()
