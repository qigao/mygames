#!/usr/bin/env python3
"""Export an ACTUAL original kwai/DouZero role .ckpt to KFCore C++ ONNX.

The original training/feature/model implementation is imported unchanged.
This is an explicit trusted OFFLINE operator action: not a CI installer,
auto-downloader, model trainer, new neural architecture, or model promotion.

Required runtime: operator-installed torch, numpy, onnx, onnxruntime.
The input must be a direct ORIGINAL <role>_weights_<frames>.ckpt state_dict
and the caller must supply its EXPECTED full SHA256 from a trusted source.
Never accept the retired custom MyGames DMC checkpoint/model.tar by accident.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / "third_party" / "DouZero"
LOCK = ROOT / "ai/douzero/upstream.lock.json"
UPSTREAM_COMMIT = "718a5c920bf3361e34178a38f3b80458e176b351"
MODEL_TYPES = {"landlord": 319, "landlord_up": 430, "landlord_down": 430}
PROFILE = "douzero.original.wp"
TOLERANCE = 5e-4


def sha256(path: Path) -> str:
    with path.open("rb") as f:
        digest = hashlib.sha256()
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verified_source() -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if (lock.get("repository") != "https://github.com/kwai/DouZero" or
            lock.get("commit") != UPSTREAM_COMMIT or
            lock.get("schema") != "mygames-pinned-douzero-upstream-v1"):
        raise ValueError("not the pinned original DouZero source")
    model_path = UPSTREAM / "douzero/dmc/models.py"
    data = model_path.read_bytes()
    digest = hashlib.sha1(
        b"blob " + str(len(data)).encode("ascii") + b"\0" + data
    ).hexdigest()
    if lock["files"].get("douzero/dmc/models.py") != digest:
        raise ValueError("original DouZero model.py source was modified")


def owned_existing_input(path: Path) -> Path:
    path = path.absolute()
    if path.is_symlink() or not path.is_file():
        raise ValueError("expected regular non-symlink ORIGINAL role checkpoint")
    st = path.stat()
    if not 0 < st.st_size <= 256 * 1024 * 1024:
        raise ValueError("role checkpoint outside bounded 256MiB limit")
    if stat.S_IMODE(st.st_mode) & 0o077:
        raise PermissionError("original model checkpoint must be owner-private (0600)")
    return path


def export(args: argparse.Namespace) -> dict[str, Any]:
    verified_source()
    if args.role not in MODEL_TYPES:
        raise ValueError("unsupported original role")
    if type(args.model_version) is not int or not 1 <= args.model_version < 2**64:
        raise ValueError("explicit positive model version required")
    if (len(args.expected_checkpoint_sha256) != 64 or
        any(ch not in "0123456789abcdef"
            for ch in args.expected_checkpoint_sha256)):
        raise ValueError("canonical trusted checkpoint SHA256 required")
    checkpoint = owned_existing_input(args.checkpoint)
    if sha256(checkpoint) != args.expected_checkpoint_sha256:
        raise ValueError("checkpoint differs from independently pinned SHA256")
    target = args.out_dir.absolute()
    parent = target.parent
    if target.exists() or target.is_symlink() or not parent.is_dir() or parent.is_symlink():
        raise FileExistsError("output package destination already exists or is untrusted")
    if stat.S_IMODE(parent.stat().st_mode) & 0o077:
        raise PermissionError("model package parent must be private (0700)")
    import numpy as np
    import torch
    import onnx
    import onnxruntime as ort

    # Execute the exact locked upstream models.py DIRECTLY: Python package
    # initializers import the training scheduler and GitPython, neither of
    # which belongs in a standalone model-conversion process.
    model_source = UPSTREAM / "douzero/dmc/models.py"
    spec = importlib.util.spec_from_file_location(
        "_mygames_pinned_original_douzero_models", model_source)
    if spec is None or spec.loader is None:
        raise ValueError("original model source module cannot be loaded")
    original = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(original)
    model = (original.LandlordLstmModel() if args.role == "landlord"
             else original.FarmerLstmModel())
    state = torch.load(checkpoint, map_location="cpu", weights_only=True)
    expected = model.state_dict()
    if (not isinstance(state, dict) or set(state) != set(expected) or
            any(not isinstance(state[k], torch.Tensor) or
                state[k].shape != expected[k].shape or
                state[k].dtype != expected[k].dtype or
                not torch.isfinite(state[k]).all()
                for k in expected)):
        raise ValueError("not an exact finite ORIGINAL role model state_dict")
    model.load_state_dict(state, strict=True)
    model.eval()

    class QValues(torch.nn.Module):
        def __init__(self, original_model):
            super().__init__()
            self.original_model = original_model

        def forward(self, z, xa):
            return self.original_model(z, xa, return_value=True)["values"].squeeze(-1)

    qmodel = QValues(model).eval()
    torch.set_num_threads(1)
    width = MODEL_TYPES[args.role] + 54
    # Only PUBLIC synthetic BINARY values for topology/Q numerical parity.
    def inputs(batch):
        rng = np.random.default_rng(17311 + batch + width)
        z = rng.integers(0, 2, size=(batch, 5, 162), dtype=np.int8).astype(np.float32)
        xa = rng.integers(0, 2, size=(batch, width), dtype=np.int8).astype(np.float32)
        return z, xa

    staging = Path(tempfile.mkdtemp(prefix=".douzero-original-", dir=parent))
    try:
        os.chmod(staging, 0o700)
        onnx_path = staging / "model.onnx"
        z, xa = inputs(7)
        with torch.inference_mode():
            torch.onnx.export(
                qmodel,
                (torch.from_numpy(z), torch.from_numpy(xa)),
                str(onnx_path),
                input_names=["z", "xa"], output_names=["q"],
                dynamic_axes={"z": {0: "batch"}, "xa": {0: "batch"},
                              "q": {0: "batch"}},
                opset_version=17, export_params=True, do_constant_folding=True,
                dynamo=False
            )
        if not onnx_path.is_file() or onnx_path.is_symlink():
            raise RuntimeError("original model ONNX export was incomplete")
        onnx.checker.check_model(str(onnx_path))
        so = ort.SessionOptions()
        so.intra_op_num_threads = 1
        so.inter_op_num_threads = 1
        session = ort.InferenceSession(
            str(onnx_path), sess_options=so, providers=["CPUExecutionProvider"])
        if session.get_providers() != ["CPUExecutionProvider"]:
            raise RuntimeError("explicit ORT CPU provider not obtained")
        input_desc = {i.name: (i.type, i.shape) for i in session.get_inputs()}
        output_desc = {i.name: (i.type, i.shape) for i in session.get_outputs()}
        if (set(input_desc) != {"z", "xa"} or set(output_desc) != {"q"} or
                any(dtype != "tensor(float)" for dtype, _ in input_desc.values()) or
                output_desc["q"][0] != "tensor(float)"):
            raise ValueError("wrong original DouZero ONNX IO tensors")
        errors = {}
        with torch.inference_mode():
            for n in (1, 7, 64):
                z, xa = inputs(n)
                expected_q = qmodel(torch.from_numpy(z), torch.from_numpy(xa))
                actual_q = session.run(["q"], {"z": z, "xa": xa})[0]
                expected_np = expected_q.detach().numpy()
                if (actual_q.shape != (n,) or expected_np.shape != (n,) or
                        not np.isfinite(actual_q).all() or
                        not np.isfinite(expected_np).all()):
                    raise ValueError("original Torch/ORT batch incomplete or nonfinite")
                error = float(np.max(np.abs(actual_q - expected_np)))
                if error >= TOLERANCE:
                    raise ValueError(
                        f"real original checkpoint Torch/ORT Q mismatch n={n}: {error}")
                errors[str(n)] = error

        model_sha = sha256(onnx_path)
        metadata = {
            "schema": "kfcore.model/1",
            "id": f"mygames-original-douzero-{args.role}-{args.model_version}",
            "version": str(args.model_version),
            "model_type": "douzero-original-q",
            "variant": args.role,
            "semantic_contract": PROFILE,
            "semantic_version": "1",
            "artifacts": [{
                "id": "onnx-cpu", "format": "onnx",
                "flavor": "douzero-original-q-v1", "path": "model.onnx",
                "sha256": model_sha, "backend": "onnxruntime", "device": "cpu"
            }]
        }
        provenance = {
            "schema": "mygames.douzero.original-onnx-provenance/1",
            "upstream_repo": "https://github.com/kwai/DouZero",
            "upstream_commit": UPSTREAM_COMMIT,
            "objective": "wp",
            "role": args.role,
            "model_version": args.model_version,
            "original_checkpoint_sha256": args.expected_checkpoint_sha256,
            "onnx_sha256": model_sha,
            "numeric_parity_max_abs_by_batch": errors,
            "qualification": "torch-onnx-numeric-only",
            "gameplay_skill_qualified": False,
            "production_owner_qualified": False
        }
        for filename, data in (("model.json", metadata),
                               ("provenance.json", provenance)):
            p = staging / filename
            p.write_text(
                json.dumps(data, sort_keys=True, separators=(",", ":"), allow_nan=False)
                + "\n", encoding="utf-8")
        for p in staging.iterdir():
            if not p.is_file():
                raise RuntimeError("unexpected generated ONNX external data file")
            os.chmod(p, 0o600)
        # Atomic no-overwrite publication: parent remains PRIVATE; staged
        # weights never reach public GitHub/CI artifacts.
        os.rename(staging, target)
        return {
            "role": args.role,
            "model_version": args.model_version,
            "original_checkpoint_sha256": args.expected_checkpoint_sha256,
            "onnx_sha256": model_sha,
            "numeric_parity_max_abs_by_batch": errors,
            "kfcore_semantic_contract": PROFILE,
            "full_native_game_qualified": False
        }
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--role", choices=sorted(MODEL_TYPES), required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--expected-checkpoint-sha256", required=True)
    parser.add_argument("--model-version", type=int, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(export(args), sort_keys=True))


if __name__ == "__main__":
    main()
