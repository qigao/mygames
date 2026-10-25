#!/usr/bin/env python3
"""Run real gateway and both Node smoke clients with bounded process lifetime."""
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: ci_gateway_smoke.py /path/to/mygames_server")
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as output:
        proc = subprocess.Popen(
            [executable], stdin=subprocess.PIPE, stdout=output,
            stderr=subprocess.STDOUT, env=os.environ.copy())
        result = 1
        try:
            deadline = time.monotonic() + 15.0
            while time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError("gateway quit before ready")
                try:
                    with urllib.request.urlopen("http://127.0.0.1:1780/health", timeout=1) as response:
                        if response.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.15)
            else:
                raise RuntimeError("CHttp gateway health deadline exceeded")
            for script in ("ws_smoke.mjs", "room_ws_smoke.mjs"):
                subprocess.run(["node", "scripts/" + script], timeout=25, check=True)
            result = 0
        finally:
            if proc.poll() is None and proc.stdin is not None:
                try:
                    proc.stdin.write(b"\n")
                    proc.stdin.flush()
                except (BrokenPipeError, OSError):
                    pass
                try:
                    proc.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    try:
                        proc.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
            output.seek(0)
            print("===== CHttp gateway log =====", flush=True)
            print(output.read()[-12000:], flush=True)
        return result

if __name__ == "__main__":
    raise SystemExit(main())
