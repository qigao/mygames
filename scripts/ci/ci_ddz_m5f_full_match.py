#!/usr/bin/env python3
"""Qualify TWO ACTUAL three-player completed matches on real CHttp,
single CFlow Owner and persistent TurboDB SQLite WAL/FULL.

The original game, winner, rematch, private card state and command receipts
must survive three independent server process restarts and epoch claims.
Only a mode-0600 temporary file holds rotating bearer identity. No private
hand, bearer ticket, or shuffled deck is emitted into GitHub CI artifacts.
"""
import os
import pathlib
import json
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

STAGES = ("first", "restart", "second-restart", "third-restart")

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

def audit_immutable_results(db, audit_file, stage):
    # Separate READ-ONLY physical SQLite connection AFTER each completed
    # service process. Never synthesize a result from a WebSocket response.
    info = json.loads(audit_file.read_text(encoding="utf-8"))
    first_game = int(info["game_id"])
    players = [int(x["player_id"]) for x in info["players"]]
    if len(players) != 3 or len(set(players)) != 3:
        raise AssertionError("Invalid independently recorded player seats")
    expected_rows = 1 if stage in ("first", "restart") else 2
    uri = "file:" + str(db) + "?mode=ro"
    with sqlite3.connect(uri, uri=True) as connection:
        rows = connection.execute(
            "select room_id,game_id,finish_seq,p0,p1,p2,base,bombs,"
            "spring,landlord_seat,winner_seat,multiplier,unit,d0,d1,d2 "
            "from mg_game_results order by finish_seq"
        ).fetchall()
        if len(rows) != expected_rows:
            raise AssertionError(
                f"Expected {expected_rows} immutable settled games at {stage},"
                f" got {len(rows)}")
        first = rows[0]
        if first[0] != info["room_id"] or first[1] != first_game:
            raise AssertionError("Wrong immutable first game identity")
        if tuple(first[3:6]) != tuple(players):
            raise AssertionError("Historical first settlement has wrong players")
        if tuple(first[6:]) != (3, 0, 1, 0, 0, 2, 6, 12, -6, -6):
            raise AssertionError("Incorrect first landlord-spring settlement")
        if first[2] != int(info["finalAck"]["seq"]):
            raise AssertionError("First finish_sequence != Game ACK sequence")
        if stage != "first":
            rematch_game = int(info["rematch"]["game_id"])
            if rematch_game <= first_game:
                raise AssertionError("Rematch ID reused finished game")
            if stage == "restart":
                # Game 2 has been dealt/awarded, but its last play has NOT
                # committed. The ledger must not credit even once.
                if any(row[1] == rematch_game for row in rows):
                    raise AssertionError("Unfinished rematch received a score")
            else:
                second = rows[1]
                if (second[0] != info["room_id"] or
                        second[1] != rematch_game or
                        tuple(second[3:6]) != tuple(players)):
                    raise AssertionError("Second settlement attributed incorrectly")
                if tuple(second[6:]) != (3, 0, 1, 2, 2, 2, 6, -6, -6, 12):
                    raise AssertionError("Incorrect second landlord-spring score")
                if (second[2] != int(info["rematchFinalAck"]["seq"]) or
                        second[2] <= first[2]):
                    raise AssertionError("Second terminal sequence not immutable")
                if first[1] == second[1] or first[2] == second[2]:
                    raise AssertionError("Duplicate finished game or sequence")
        expected_points = ((12, -6, -6) if expected_rows == 1
                           else (6, -12, 6))
        for player, points in zip(players, expected_points):
            total, count = connection.execute(
                "select coalesce(sum(case when p0=? then d0 "
                "when p1=? then d1 when p2=? then d2 end),0),"
                "count(*) from mg_game_results "
                "where room_id=? and (p0=? or p1=? or p2=?)",
                (player, player, player, info["room_id"],
                 player, player, player)
            ).fetchone()
            if (total, count) != (points, expected_rows):
                raise AssertionError("Wrong immutable per-player cumulative score")
        names = {col[1] for col in connection.execute(
            "pragma table_info(mg_game_results)")}
        if {"private_image", "secret_deck", "hands", "kitty"} & names:
            raise AssertionError("Result ledger leaked a private game column")
    print(f"m5h real read-only ledger {stage}: PASS "
          f"({expected_rows} distinct finished games, durable Game ACK "
          "sequence and cumulative zero-sum scores)")


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
            audit_immutable_results(db, credentials, stage)
        print("mygames.m5h.full_game: PASS"
              " (two different landlords, two complete 20-card games,"
              " ~120 physical commits, three cold epoch takeovers,"
              " two immutable results and byte-identical Game ACK replay)")

if __name__ == "__main__":
    main()
