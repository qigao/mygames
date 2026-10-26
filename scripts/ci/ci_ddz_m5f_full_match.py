#!/usr/bin/env python3
"""Qualify an ACTUAL three-player completed match/rematch on real CHttp,
single CFlow Owner and persistent TurboDB SQLite WAL/FULL.

The original game, winner, rematch, private card state and command receipts
must survive two independent server process restarts and epoch claims.
Only a mode-0600 temporary file holds rotating bearer identity. No private
hand, bearer ticket, or shuffled deck is emitted into GitHub CI artifacts.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

STAGES = ("first", "restart", "second-restart")

def with_native_server(executable, env, stage):
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as log:
        proc = subprocess.Popen([executable], stdin=subprocess.PIPE,
                                stdout=log, stderr=subprocess.STDOUT, env=env)
        failure = None
        try:
            for _ in range(120):
                if proc.poll() is not None:
                    raise RuntimeError("Native game process exited before health check")
                try:
                    with urllib.request.urlopen(
                            "http://127.0.0.1:1780/health", timeout=1) as resp:
                        if resp.status == 200:
                            break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.12)
            else:
                raise RuntimeError("Native game health check deadline exceeded")
            subprocess.run(
                ["node", "scripts/ddz_m5f_full_match.mjs", stage],
                timeout=140, check=True, env=env,
            )
        except Exception as error:
            failure = error
        finally:
            if proc.poll() is None:
                try:
                    proc.communicate(input=b"\n", timeout=12)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=6)
            if failure or proc.returncode != 0:
                # Native server never prints secret hands, member tokens or
                # full database images. Preserve only bounded diagnostic tail.
                log.seek(0)
                print("===== M5f native server diagnostic tail =====", flush=True)
                print(log.read()[-8000:], flush=True)
        if failure:
            raise failure
        if proc.returncode != 0:
            raise RuntimeError("Native game service exit %d" % proc.returncode)

def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ci_ddz_m5f_full_match.py server sqlite_driver")
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    driver = pathlib.Path(sys.argv[2]).resolve()
    if not pathlib.Path(executable).is_file() or not driver.is_file():
        raise RuntimeError("Real SDK game server/SQLite driver missing")
    with tempfile.TemporaryDirectory(prefix="mygames-m5f-private-") as scratch:
        root = pathlib.Path(scratch)
        db = root / "completed-match.sqlite"
        credentials = root / "rotating-bearers.json"
        env = os.environ.copy()
        env["MYGAMES_SQLITE_PLUGIN"] = str(driver)
        env["MYGAMES_DB_PATH"] = str(db)
        env["MYGAMES_M5F_PRIVATE_AUDIT"] = str(credentials)
        for stage in STAGES:
            with_native_server(executable, env, stage)
            if not db.is_file() or not credentials.is_file():
                raise AssertionError("Missing real durable game or recovery proof")
            if credentials.stat().st_mode & 0o077:
                raise AssertionError("Rotating capability manifest not private")
        print("mygames.m5f.full_game: PASS"
              " (20 physical cards, ~60 real commits, winner,"
              " rematch with a different landlord,"
              " two cold epoch takeovers, byte-identical Game ACK replay)")

if __name__ == "__main__":
    main()
