"""Read-only, exact upstream DouZero byte identity and source contract.

No Torch/rlcard installation, alternate model, second game engine, generated
data, model weights, or installer/consumer project is involved in this gate.
"""
from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
LOCK = REPO / "ai/douzero/upstream.lock.json"
VENDOR = REPO / "third_party/DouZero"


def git_sha1(data: bytes) -> str:
    prefix = ("blob " + str(len(data))).encode("ascii") + b"\0"
    return hashlib.sha1(prefix + data).hexdigest()


def main() -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    if (not isinstance(lock, dict) or set(lock) != {
            "schema", "repository", "commit", "license", "source_root",
            "excluded_upstream_paths", "files"} or
            lock["schema"] != "mygames-pinned-douzero-upstream-v1" or
            lock["repository"] != "https://github.com/kwai/DouZero" or
            lock["commit"] != "718a5c920bf3361e34178a38f3b80458e176b351" or
            lock["license"] != "Apache-2.0" or
            lock["source_root"] != "third_party/DouZero" or
            lock["excluded_upstream_paths"] !=
                [".github/workflows/python-package.yml"]):
        raise ValueError("upstream source identity changed; re-review required")
    original = lock["files"]
    if not isinstance(original, dict) or len(original) != 32:
        raise ValueError("expected exactly the pinned complete upstream files")
    actual_files = {
        p.relative_to(VENDOR).as_posix()
        for p in VENDOR.rglob("*") if p.is_file()
    }
    if set(original) != actual_files:
        raise ValueError("DouZero upstream has missing or extra files")
    python_count = 0
    for relative, expected in original.items():
        relative_path = Path(relative)
        if (relative_path.is_absolute() or ".." in relative_path.parts or
                not isinstance(expected, str) or len(expected) != 40 or
                any(ch not in "0123456789abcdef" for ch in expected)):
            raise ValueError("invalid upstream blob reference")
        f = VENDOR / relative_path
        if f.is_symlink() or not f.is_file():
            raise ValueError("unexpected symlink or missing upstream module")
        data = f.read_bytes()
        if git_sha1(data) != expected:
            raise ValueError("upstream source differs from pristine Git blob")
        if relative.endswith(".py"):
            ast.parse(data, filename=relative)
            python_count += 1

    expected_modules = {
        "douzero/dmc/dmc.py": ("train", "learn"),
        "douzero/dmc/utils.py": ("act", "get_batch", "create_buffers"),
        "douzero/dmc/models.py": ("LandlordLstmModel", "FarmerLstmModel", "Model"),
        "douzero/env/env.py": ("Env",),
    }
    for path, declarations in expected_modules.items():
        body = ast.parse((VENDOR / path).read_bytes(), filename=path).body
        names = {n.name for n in body if isinstance(n, (ast.FunctionDef, ast.ClassDef))}
        if not set(declarations).issubset(names):
            raise ValueError("missing upstream actor/learner/model/env entry point")

    # Older home-grown DMC/heuristic trainers must not become a second RL
    # provider on this clean restart branch. Original C11 rules are preserved.
    if (REPO / "ai/dmc").exists() or (REPO / "ai/core").exists():
        raise ValueError("retired custom DMC trainer leaked into DouZero baseline")
    if not (REPO / "games/doudizhu/src/ddz_round.c").is_file():
        raise ValueError("authoritative existing MyGames C11 round was lost")
    print("mygames.douzero_pristine_upstream_source: PASS")
    print(f"upstream_commit={lock['commit']} pristine_files={len(original)} "
          f"python_modules={python_count} custom_dmc_removed=true")


if __name__ == "__main__":
    main()
