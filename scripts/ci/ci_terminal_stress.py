#!/usr/bin/env python3
"""Stress exact CHttp CLOSE reserve while CFlow normal admission is FULL."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error

def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: ci_terminal_stress.py /path/to/mygames_server")
    server = str(pathlib.Path(sys.argv[1]).resolve())
    env = os.environ.copy()
    env.pop("MYGAMES_DB_PATH", None)
    env.pop("MYGAMES_SQLITE_PLUGIN", None)
    env["MYGAMES_TEST_ROOM_QUEUE_CAPACITY"] = "4"
    env["MYGAMES_TEST_PAUSE_REQUEST_ID"] = "stall-owner"
    env["MYGAMES_TEST_PAUSE_OWNER_MS"] = "900"
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as out:
        proc = subprocess.Popen([server],stdin=subprocess.PIPE,
                                stdout=out,stderr=subprocess.STDOUT,env=env)
        try:
            deadline=time.monotonic()+12
            while time.monotonic()<deadline:
                if proc.poll() is not None:
                    raise RuntimeError("terminal server died at startup")
                try:
                    with urllib.request.urlopen(
                        "http://127.0.0.1:1780/health",timeout=1) as reply:
                        if reply.status==200: break
                except (OSError,urllib.error.URLError):
                    time.sleep(0.12)
            else: raise RuntimeError("terminal server health timeout")
            subprocess.run(
                ["node","scripts/room_terminal_stress.mjs"],
                timeout=22,check=True,env=env)
        finally:
            if proc.poll() is None:
                try: proc.communicate(input=b"\n",timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                    raise RuntimeError("terminal server teardown timed out")
            out.seek(0)
            transcript=out.read()
            print("==== terminal server log ====",flush=True)
            print(transcript[-2500:],flush=True)
        if proc.returncode != 0:
            raise RuntimeError("terminal server failed")
        if "room owner queue FULL" not in transcript:
            raise AssertionError("no evidence of ordinary mailbox exhaustion")
        print("mygames.close_reserved_when_full: PASS",flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
