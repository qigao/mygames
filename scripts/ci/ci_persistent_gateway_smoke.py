#!/usr/bin/env python3
"""Qualify real CFlow WebSocket -> TurboDB commit-before-ACK path.
The original database is owned by the TurboDB/SQLite plugin, not this script.
Tests operate only on an isolated temporary directory.
"""
import os
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: ci_persistent_gateway_smoke.py server sqlite_driver")
    server = str(pathlib.Path(sys.argv[1]).resolve())
    plugin = str(pathlib.Path(sys.argv[2]).resolve())
    if not pathlib.Path(plugin).is_file():
        raise RuntimeError("Exact TurboDB SQLite plugin missing: " + plugin)

    with tempfile.TemporaryDirectory(prefix="mygames-turbodb-ws-") as root:
        database = str(pathlib.Path(root) / "room-live.sqlite")
        env = os.environ.copy()
        env["MYGAMES_SQLITE_PLUGIN"] = plugin
        env["MYGAMES_DB_PATH"] = database

        subprocess.run(
            [sys.executable, "scripts/ci/ci_gateway_smoke.py", server],
            env=env, timeout=80, check=True,
        )
        if not pathlib.Path(database).is_file():
            raise AssertionError("WebSocket operations did not create a TurboDB database")
        subprocess.run(
            [str(pathlib.Path("build/gateway/mygames_room_gateway_probe").resolve()),
             plugin, database],
            env=env, timeout=25, check=True,
        )

        # Session identity/tickets remain ephemeral. Nonempty persisted rooms
        # must NOT be silently rebound to freshly numbered unauthenticated users.
        reopened = subprocess.run(
            [server], input=b"\n", env=env, timeout=20,
            capture_output=True, check=False,
        )
        combined = reopened.stdout + reopened.stderr
        if reopened.returncode == 0 or b"orphaned seats" not in combined:
            raise AssertionError("Restart must fail closed on orphaned sessions: "
                                 + combined[-2500:].decode(errors="replace"))
        print("mygames.gateway_persistent: PASS "
              "(ACK-on-commit, reopened facts and fail-closed restart)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
