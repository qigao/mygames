#!/usr/bin/env python3
"""Run real idle-expiry qualification against the exact pinned TurboDB SDK."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: ci_idle_expiry_smoke.py gateway sqlite_plugin")
    server = str(pathlib.Path(sys.argv[1]).resolve())
    plugin = str(pathlib.Path(sys.argv[2]).resolve())
    with tempfile.TemporaryDirectory(prefix="mygames-idle-expiry-") as base:
        db = str(pathlib.Path(base) / "idle.sqlite")
        audit = str(pathlib.Path(base) / "audit.json")
        env = os.environ.copy()
        env["MYGAMES_SQLITE_PLUGIN"] = plugin
        env["MYGAMES_DB_PATH"] = db
        # CI-only and only when MYGAMES_GATEWAY_STORE_FAULT_TESTING is defined.
        env["MYGAMES_TEST_SESSION_GRACE_MS"] = "700"
        env["MYGAMES_IDLE_AUDIT_OUTPUT"] = audit
        with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as output:
            proc = subprocess.Popen(
                [server], stdin=subprocess.PIPE, stdout=output,
                stderr=subprocess.STDOUT, env=env)
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError("idle gateway quit before ready")
                    try:
                        with urllib.request.urlopen(
                            "http://127.0.0.1:1780/health", timeout=1) as reply:
                            if reply.status == 200: break
                    except (OSError, urllib.error.URLError):
                        time.sleep(0.12)
                else:
                    raise RuntimeError("idle gateway ready timeout")
                subprocess.run(
                    ["node", "scripts/room_idle_expiry_smoke.mjs"],
                    check=True,timeout=18,env=env)
            finally:
                if proc.poll() is None:
                    try:
                        proc.communicate(input=b"\n",timeout=8)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                        raise RuntimeError("idle gateway shutdown timed out")
                output.seek(0)
                print("==== idle owner log ====",flush=True)
                print(output.read()[-3000:],flush=True)
            if proc.returncode != 0:
                raise RuntimeError(f"idle gateway exited {proc.returncode}")
        details = json.loads(pathlib.Path(audit).read_text(encoding="utf-8"))
        subprocess.run(
            [str(pathlib.Path("build/gateway/mygames_idle_expiry_probe").resolve()),
             plugin,db,details["expired_player"]],
            check=True,timeout=15,env=env)
        print("mygames.idle_autonomous_expiry: PASS",flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
