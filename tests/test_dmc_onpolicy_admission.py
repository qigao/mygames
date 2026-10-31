"""No-Torch private on-policy source/publisher admission acceptance."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ai" / "dmc"))
from collect_onpolicy import collect, immutable_private_write, private_dir


def reject(fn, error):
    try:
        fn()
    except error:
        return
    raise AssertionError(f"invalid data accepted by {fn}")


def main():
    with tempfile.TemporaryDirectory() as base:
        parent = Path(base)
        source = parent / "source"
        source.mkdir(mode=0o700)
        output = parent / "output"
        output.mkdir(mode=0o700)
        private_dir(output)
        data = b"PRIVATE_REPLAY_FIXTURE_NOT_A_GAME"
        immutable_private_write(output, "episode-123.mgdmc", data)
        result = output / "episode-123.mgdmc"
        assert result.read_bytes() == data
        assert stat.S_IMODE(result.stat().st_mode) == 0o600
        assert result.stat().st_nlink == 1
        reject(lambda: immutable_private_write(output, "episode-123.mgdmc", b"overwrite"), FileExistsError)
        reject(lambda: immutable_private_write(output, "../unsafe", b"file"), ValueError)
        assert result.read_bytes() == data
        bad = parent / "bad"
        bad.mkdir(mode=0o755)
        reject(lambda: private_dir(bad), PermissionError)
        link = parent / "link"
        link.symlink_to(output, target_is_directory=True)
        reject(lambda: private_dir(link), PermissionError)
        # Full admission rejects a changed checkpoint digest BEFORE
        # allocating Torch, opening native engine or publishing Replay.
        fresh = parent / "fresh"
        fresh.mkdir(mode=0o700)
        chk = source / "frozen.pt"
        chk.write_bytes(b"placeholder")
        chk.chmod(0o600)
        lib = source / "native.so"
        lib.write_bytes(b"placeholder-native")
        lib.chmod(0o600)
        args = SimpleNamespace(
            private_dir=fresh, checkpoint=chk, native_library=lib,
            checkpoint_sha256="0" * 64,
            native_library_sha256=hashlib.sha256(lib.read_bytes()).hexdigest(),
            source_revision="1" * 40, model_version=3, first_id=60001,
            episodes=3, objective="win-sign",
        )
        reject(lambda: collect(args), ValueError)
        assert not list(fresh.iterdir())
        args.checkpoint_sha256 = hashlib.sha256(chk.read_bytes()).hexdigest()
        args.native_library_sha256 = "1" * 64
        reject(lambda: collect(args), ValueError)
        assert not list(fresh.iterdir())
    print("mygames.dmc_onpolicy_private_admission: PASS")


if __name__ == "__main__":
    main()
