#!/usr/bin/env python3
"""Qualify real CFlow WebSocket -> TurboDB commit-before-ACK path.
The original database is owned by the TurboDB/SQLite plugin, not this script.
Tests operate only on an isolated temporary directory.
"""
import os
import json
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


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
        audit_path = str(pathlib.Path(root) / "gateway-audit.json")
        env["MYGAMES_AUDIT_OUTPUT"] = audit_path

        subprocess.run(
            [sys.executable, "scripts/ci/ci_gateway_smoke.py", server],
            env=env, timeout=80, check=True,
        )
        if not pathlib.Path(database).is_file():
            raise AssertionError("WebSocket operations did not create a TurboDB database")
        subprocess.run(
            [str(pathlib.Path("build/gateway/mygames_room_gateway_probe").resolve()),
             plugin, database, str(json.loads(pathlib.Path(audit_path).read_text(
                 encoding="utf-8"))["creator_player_id"])],
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
        # A test-build-only fault after checkpoint UPDATE must not produce
        # a room ACK, room state, command result, or an Outbox event.
        fault_db = str(pathlib.Path(root) / "room-fault.sqlite")
        fault_env = env.copy()
        fault_env["MYGAMES_DB_PATH"] = fault_db
        fault_env["MYGAMES_TEST_FAIL_AFTER_CHECKPOINT_ONCE"] = "1"
        fault_env.pop("MYGAMES_AUDIT_OUTPUT", None)
        proc = subprocess.Popen([server], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=fault_env)
        try:
            deadline = time.monotonic() + 12.0
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError("fault gateway quit before health response")
                try:
                    with urllib.request.urlopen(
                        "http://127.0.0.1:1780/health", timeout=1) as response:
                        if response.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.15)
            else:
                raise RuntimeError("fault gateway health deadline exceeded")
            subprocess.run(["node", "scripts/room_storage_fault_smoke.mjs"],
                           check=True, timeout=20, env=fault_env)
        finally:
            try:
                output, _ = proc.communicate(input=b"\n", timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                output, _ = proc.communicate(timeout=5)
                raise RuntimeError("fault gateway did not stop gracefully")
        if proc.returncode != 0:
            raise RuntimeError("fault gateway exited unexpectedly: "
                               + output[-2500:].decode(errors="replace"))
        subprocess.run(
            [str(pathlib.Path("build/gateway/mygames_room_gateway_probe").resolve()),
             plugin, fault_db, "--expect-empty"],
            env=fault_env, timeout=25, check=True,
        )
        print("mygames.gateway_fault: PASS (no ACK before durable COMMIT)",
              flush=True)

        print("mygames.gateway_persistent: PASS "
              "(ACK-on-commit, reopened facts and fail-closed restart)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
