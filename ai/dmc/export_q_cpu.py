#!/usr/bin/env python3
"""Export a trusted, immutable MyGames DMC v3 PyTorch Q checkpoint to C11 ABI.

One separate role-weight image per role; pure float32 little-endian bytes,
explicit dimensions, finite checks, CRC32 for accidental corruption and
SHA256 for provenance (neither constitutes authentication). Never overwrite
or publish a public model/service. This is an OFFLINE reference CPU backend.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import tempfile
import zlib

from model import ROLE_DIMENSIONS, ROLES
from train_dmc import load_model_checkpoint, torch_runtime

HEADER_SIZE = 64
MAGIC = b"MGQF0001"
FORMAT_VERSION = 1
ARCH = "mygames-douzero-q-v1-reference-cpu-f32"


def weight_order() -> tuple[str, ...]:
    names = [f"lstm.{n}_l0" for n in
             ("weight_ih", "weight_hh", "bias_ih", "bias_hh")]
    for layer in range(1, 7):
        names += [f"fc{layer}.weight", f"fc{layer}.bias"]
    return tuple(names)


def shapes_for_role(role: int) -> tuple[tuple[int, ...], ...]:
    width = ROLE_DIMENSIONS[role] + 54 + 128
    return ((512, 162), (512, 128), (512,), (512,),
            (512, width), (512,),
            *sum((((512, 512), (512,)) for _ in range(4)), ()),
            (1, 512), (1,))


def serialize_role(model, role: int, version: int,
                   rules: int, objective_id: int) -> bytes:
    import torch
    if role not in ROLES or version < 1 or rules < 1 or objective_id not in (1, 2):
        raise ValueError("invalid frozen model role/version/rules/objective")
    state = model.state_dict()
    names = weight_order()
    shapes = shapes_for_role(role)
    if set(state) != set(names) or len(names) != len(shapes):
        raise ValueError("unsupported frozen network state dictionary")
    chunks = []
    count = 0
    for name, shape in zip(names, shapes):
        tensor = state[name]
        if tensor.dtype != torch.float32 or tuple(tensor.shape) != shape or \
                not torch.isfinite(tensor).all().item():
            raise ValueError(f"unsupported/nonfinite model tensor: {name}")
        cpu = tensor.detach().to(device="cpu").contiguous()
        # Torch's configured NumPy provider supplies explicit little-endian
        # float32 bytes, independent of the trainer host's machine order.
        chunk = cpu.numpy().astype("<f4", copy=False).tobytes(order="C")
        chunks.append(chunk)
        count += tensor.numel()
    payload = b"".join(chunks)
    assert len(payload) == 4 * count
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<8sIIQIIIIII", header, 0, MAGIC, FORMAT_VERSION, 0,
                     version, rules, objective_id, role, ROLE_DIMENSIONS[role],
                     count, len(payload))
    checksum = zlib.crc32(header + payload) & 0xFFFFFFFF
    struct.pack_into("<I", header, 48, checksum)
    return bytes(header) + payload


def _private_directory(path: Path) -> None:
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or \
            (stat.S_IMODE(info.st_mode) & 0o077):
        raise PermissionError("existing output directory must be owner-only 0700 and not a symlink")


def _no_clobber(path: Path, payload: bytes) -> None:
    if path.exists() or path.is_symlink():
        raise FileExistsError("immutable model already exists")
    fd, staging = tempfile.mkstemp(prefix=".q-cpu-partial-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.link(staging, path)  # atomic NO-CLOBBER
    finally:
        if os.path.exists(staging):
            os.unlink(staging)
    dirfd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(dirfd)
    finally:
        os.close(dirfd)


def export_checkpoint(checkpoint: Path, root: Path, *,
                      rules_version: int, objective: str, model_version: int):
    _private_directory(root)
    if (root / "q_cpu_manifest_v1.json").exists():
        raise FileExistsError("model snapshot already admitted")
    torch, device = torch_runtime("cpu", seed=1)
    models, _ = load_model_checkpoint(
        checkpoint, expected_rules=rules_version,
        expected_objective=objective, expected_model_version=model_version,
        torch=torch, device=device)
    ids = {"win-sign": 1, "game-points": 2}
    if objective not in ids:
        raise ValueError("unknown objective")
    manifest = {"schema": ARCH, "format_version": FORMAT_VERSION,
                "model_version": model_version, "rules_version": rules_version,
                "objective": objective,
                "source_checkpoint_sha256": hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                "role_images": {}}
    staged = []
    for role in ROLES:
        name = f"q-f32-role-{role}-v{model_version}.bin"
        blob = serialize_role(models[role], role, model_version,
                              rules_version, ids[objective])
        staged.append((name, blob))
        manifest["role_images"][ROLES[role]] = {
            "file": name, "sha256": hashlib.sha256(blob).hexdigest(),
            "bytes": len(blob)}
    if any((root / name).exists() for name, _ in staged):
        raise FileExistsError("one or more immutable role images already exist")
    for name, blob in staged:
        _no_clobber(root / name, blob)
    _no_clobber(root / "q_cpu_manifest_v1.json",
                (json.dumps(manifest, sort_keys=True) + "\n").encode())
    return {"model_version": model_version, "roles": sorted(manifest["role_images"]),
            "bytes": sum(len(b) for _, b in staged)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--checkpoint", required=True)
    p.add_argument("--private-dir", required=True)
    p.add_argument("--model-version", type=int, required=True)
    p.add_argument("--rules-version", type=int, required=True)
    p.add_argument("--objective", choices=("win-sign", "game-points"), required=True)
    args = p.parse_args()
    result = export_checkpoint(Path(args.checkpoint), Path(args.private_dir),
                               rules_version=args.rules_version,
                               objective=args.objective,
                               model_version=args.model_version)
    print(json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    main()
