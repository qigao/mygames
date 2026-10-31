#!/usr/bin/env python3
"""Offline frozen-DMC checkpoint-v3 -> exact KFCore ONNX Runtime packages.

Three actor-role-specific, owner-only 0700 packages with ONNX float32
z[N,5,162], xa[N,373/484], q[N]. A configured torch/onnx/onnxruntime/numpy
installation is REQUIRED: never download, auto-install or substitute a
backend. Real 1/7/64-batch Torch-vs-ORT parity runs BEFORE publication.
No public weights, Replay, Match, opponent hands or game rules enter export.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import shutil

ROLES = {0: "landlord", 1: "landlord_up", 2: "landlord_down"}
ROLE_DIMS = {0: 319, 1: 430, 2: 430}
CONTRACT = "ddz-dmc-replay-v1"
ARCH = "douzero-q-v1"
BATCHES = (1, 7, 64)
ABS_TOL = 5e-4


def check_private_directory(directory: Path) -> None:
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or \
            (stat.S_IMODE(info.st_mode) & 0o077):
        raise PermissionError("export directory must be existing owner-only 0700, no symlink")


def no_clobber_json(directory: Path, name: str, body: dict) -> None:
    target = directory / name
    if target.exists() or target.is_symlink():
        raise FileExistsError("immutable DMC metadata already exists")
    payload = (json.dumps(body, sort_keys=True, separators=(",", ":")) + "\n").encode()
    fd, partial = tempfile.mkstemp(prefix=".dmc-json-", dir=directory)
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.link(partial, target)
    finally:
        if os.path.exists(partial):
            os.unlink(partial)
    sync_directory(directory)


def sync_directory(directory: Path) -> None:
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def verify_onnx_signature(inputs, outputs, role: int) -> None:
    """Pure-stdlib shape contract, also checked by KFCore's native loader."""
    if role not in ROLES or len(inputs) != 2 or len(outputs) != 1:
        raise ValueError("DMC ONNX model must expose exactly two inputs and one output")
    items = {item.name: item for item in inputs}
    if set(items) != {"z", "xa"} or outputs[0].name != "q":
        raise ValueError("DMC model tensor names differ from the C11 callback ABI")
    for item, tail in ((items["z"], [5, 162]),
                       (items["xa"], [ROLE_DIMS[role] + 54]),
                       (outputs[0], [])):
        dims = list(item.shape)
        if item.type != "tensor(float)" or len(dims) != len(tail) + 1 or \
                not isinstance(dims[0], str) or not dims[0] or \
                dims[1:] != tail:
            raise ValueError("DMC ONNX model lacks dynamic float32 batch ABI")


def publish_verified_roles(stage: Path, private_root: Path, report: dict) -> None:
    """Publish ONLY a COMPLETE verified three-role set, root manifest last.

    All role ONNX files and their package metadata live under private
    staging until EVERY role passed real Torch/ORT parity. A Python
    exception rolls back private publication; a process crash leaves
    no final completion marker, so no model set is admitted.
    """
    # Once the FINAL marker is published, staging hard-links have nlink=2
    # until cleanup. Reject immutable re-publication by identity BEFORE
    # inspecting provisional stage-file link counts, not as a false ACL error.
    check_private_directory(private_root)
    completed = private_root / "q_onnx_manifest_v1.json"
    if completed.exists() or completed.is_symlink():
        raise FileExistsError("immutable three-role ONNX model set already published")
    if set(report["role_packages"]) != set(ROLES.values()):
        raise ValueError("cannot publish incomplete three-role ONNX set")
    check_private_directory(stage)
    if {p.name for p in stage.iterdir()} != set(ROLES.values()):
        raise ValueError("three-role ONNX staging directory contains incomplete/extra entries")
    for name in ROLES.values():
        role_stage = stage / name
        check_private_directory(role_stage)
        if {p.name for p in role_stage.iterdir()} != {"model.onnx", "model.json"}:
            raise ValueError("missing or extra staged ONNX role artifact")
        model = role_stage / "model.onnx"
        info = model.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or \
                (stat.S_IMODE(info.st_mode) & 0o077) or info.st_nlink != 1:
            raise PermissionError("staged ONNX artifact must be private regular file")
        metadata = role_stage / "model.json"
        mi = metadata.lstat()
        if not stat.S_ISREG(mi.st_mode) or mi.st_uid != os.geteuid() or \
                (stat.S_IMODE(mi.st_mode) & 0o077) or mi.st_nlink != 1:
            raise PermissionError("staged model manifest must be private regular file")
        digest = hashlib.sha256(model.read_bytes()).hexdigest()
        if digest != report["role_packages"][name]["onnx_sha256"]:
            raise ValueError("role ONNX bytes changed after parity qualification")
        meta = json.loads((role_stage / "model.json").read_text(encoding="utf-8"))
        if (meta["artifacts"][0]["sha256"] != digest or
                meta["variant"] != name or
                meta["version"] != str(report["candidate_model_version"])):
            raise ValueError("staged role model package does not match verified weights")
        with model.open("rb") as source:
            os.fsync(source.fileno())
        if (private_root / name).exists() or (private_root / name).is_symlink():
            raise FileExistsError("immutable role package already exists")
    published: list[Path] = []
    try:
        for name in ROLES.values():
            target = private_root / name
            target.mkdir(mode=0o700, exist_ok=False)
            published.append(target)
            source = stage / name
            os.link(source / "model.onnx", target / "model.onnx")
            os.link(source / "model.json", target / "model.json")
            sync_directory(target)
        # This is the ONLY publication/acceptance marker for the complete set.
        no_clobber_json(private_root, "q_onnx_manifest_v1.json", report)
    except BaseException:
        # Do not erase committed packages after an unexpected final-marker
        # creation. Otherwise safely discard this attempt's private children.
        if not (private_root / "q_onnx_manifest_v1.json").exists():
            for target in reversed(published):
                if target.exists() and not target.is_symlink():
                    shutil.rmtree(target)
        raise


def export_checkpoint(checkpoint: Path, private_root: Path, *,
                      model_version: int, rules_version: int,
                      objective: str) -> dict:
    if not 0 < model_version < 2**64 or rules_version != 1 or \
            objective != "win-sign":
        raise ValueError("KFCore DMC first slice requires win-sign and rules v1")
    check_private_directory(private_root)
    if any(private_root.iterdir()):
        raise FileExistsError("private ONNX export destination must start EMPTY")
    # Heavy dependencies remain strictly operator-provisioned and offline.
    import numpy as np
    import onnxruntime as ort
    import torch
    from train_dmc import load_model_checkpoint, torch_runtime
    torch, device = torch_runtime("cpu", seed=1)
    models, payload = load_model_checkpoint(
        checkpoint, expected_rules=rules_version,
        expected_objective=objective,
        expected_model_version=model_version,
        torch=torch, device=device)
    source_hash = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    report = {
        "schema": "mygames-dmc-kfcore-onnx-v1",
        "source_checkpoint_sha256": source_hash,
        "candidate_model_version": model_version,
        "rules_version": rules_version, "objective": objective,
        "torch_version": str(torch.__version__),
        "onnxruntime_version": str(ort.__version__),
        "role_packages": {}
    }
    # All 3 roles are staged together. No role-specific package is visible
    # before ALL three actual frozen Torch/ORT parity checks have passed.
    stage_root = Path(tempfile.mkdtemp(prefix=".dmc-onnx-stage-", dir=private_root))
    try:
        check_private_directory(stage_root)
        for role, name in ROLES.items():
            model = models[role].cpu().eval()
            stage = stage_root / name
            stage.mkdir(mode=0o700, exist_ok=False)
            check_private_directory(stage)
            staged_onnx = stage / "model.onnx"
            z = torch.zeros((2, 5, 162), dtype=torch.float32)
            xa = torch.zeros((2, ROLE_DIMS[role] + 54), dtype=torch.float32)
            torch.onnx.export(
                model, (z, xa), str(staged_onnx),
                input_names=["z", "xa"], output_names=["q"],
                dynamic_axes={"z": {0: "batch"}, "xa": {0: "batch"},
                              "q": {0: "batch"}},
                opset_version=17, dynamo=False)
            staged_onnx.chmod(0o600)
            session = ort.InferenceSession(
                str(staged_onnx), providers=["CPUExecutionProvider"])
            if session.get_providers() != ["CPUExecutionProvider"]:
                raise RuntimeError("explicit ONNX Runtime CPU provider unavailable")
            verify_onnx_signature(session.get_inputs(), session.get_outputs(), role)
            rng = np.random.default_rng(317 + role)
            worst = 0.0
            for batch in BATCHES:
                zv = rng.integers(0, 2, (batch, 5, 162), dtype=np.int8).astype(np.float32)
                xav = rng.integers(
                    0, 2, (batch, ROLE_DIMS[role] + 54),
                    dtype=np.int8).astype(np.float32)
                with torch.inference_mode():
                    expected = model(torch.from_numpy(zv), torch.from_numpy(xav))
                actual = session.run(["q"], {"z": zv, "xa": xav})[0]
                reference = expected.detach().numpy()
                if actual.shape != (batch,) or \
                        not np.isfinite(actual).all() or \
                        not np.isfinite(reference).all():
                    raise RuntimeError("ONNX output is incomplete/nonfinite")
                maximum = float(np.max(np.abs(actual - reference)))
                worst = max(worst, maximum)
                if maximum >= ABS_TOL:
                    raise RuntimeError("DMC ONNX vs frozen Torch Q differs")
            onnx_digest = hashlib.sha256(staged_onnx.read_bytes()).hexdigest()
            package = {
                "schema": "kfcore.model/1",
                "id": f"mygames-dmc-{name}-v{model_version}",
                "version": str(model_version),
                "model_type": "ddz-dmc-q",
                "variant": name,
                "semantic_contract": CONTRACT,
                "semantic_version": "1",
                "artifacts": [{
                    "id": "onnx-cpu", "format": "onnx",
                    "path": "model.onnx", "flavor": ARCH,
                    "sha256": onnx_digest,
                    "backend": "onnxruntime", "device": "cpu"
                }]
            }
            no_clobber_json(stage, "model.json", package)
            report["role_packages"][name] = {
                "model_path": f"{name}/model.json",
                "onnx_sha256": onnx_digest,
                "max_abs_f32_torch_ort": worst,
                "verified_batch_sizes": list(BATCHES),
            }
        publish_verified_roles(stage_root, private_root, report)
    finally:
        shutil.rmtree(stage_root)
    return {
        "model_version": model_version,
        "verified_roles": list(ROLES.values()),
        "verified_batch_sizes": list(BATCHES),
        "source_checkpoint_sha256": source_hash
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--private-dir", type=Path, required=True)
    parser.add_argument("--model-version", type=int, required=True)
    parser.add_argument("--rules-version", type=int, required=True)
    parser.add_argument("--objective", choices=("win-sign",), required=True)
    args = parser.parse_args()
    print(json.dumps(export_checkpoint(
        args.checkpoint, args.private_dir,
        model_version=args.model_version, rules_version=args.rules_version,
        objective=args.objective), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
