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
import json
import sqlite3
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

def audit_immutable_results(db, audit_file, stage):
    # A truly separate READONLY sqlite connection: no Game Owner fixture
    # or application callback is permitted to synthesize ledger rows.
    info = json.loads(audit_file.read_text(encoding="utf-8"))
    first_game = int(info["game_id"])
    players = [int(x["player_id"]) for x in info["players"]]
    if len(players) != 3 or len(set(players)) != 3:
        raise AssertionError("Invalid independently recorded player seats")
    uri = "file:" + str(db) + "?mode=ro"
    with sqlite3.connect(uri, uri=True) as connection:
        rows = connection.execute(
            "select room_id,game_id,finish_seq,p0,p1,p2,base,bombs,"
            "spring,landlord_seat,winner_seat,multiplier,unit,d0,d1,d2 "
            "from mg_game_results order by finish_seq"
        ).fetchall()
        if len(rows) != 1:
            raise AssertionError(
                "Expected EXACTLY one credited finished game at " + stage)
        row = rows[0]
        if row[0] != info["room_id"] or row[1] != first_game:
            raise AssertionError("Wrong immutable first game identity")
        if tuple(row[3:6]) != tuple(players):
            raise AssertionError("Historical settlement attributed to wrong players")
        if tuple(row[6:]) != (3, 0, 1, 0, 0, 2, 6, 12, -6, -6):
            raise AssertionError("Incorrect real landlord spring ledger result")
        if row[2] != int(info["finalAck"]["seq"]):
            raise AssertionError("Ledger finish_sequence != durable Game ACK")
        for player, points in zip(players, (12, -6, -6)):
            total, count = connection.execute(
                "select coalesce(sum(case when p0=? then d0 "
                "when p1=? then d1 when p2=? then d2 end),0),"
                "count(*) from mg_game_results "
                "where room_id=? and (p0=? or p1=? or p2=?)",
                (player,player,player,info["room_id"],player,player,player)
            ).fetchone()
            if (total, count) != (points, 1):
                raise AssertionError("Duplicate or lost per-player game credit")
        if stage != "first":
            game_two = int(info["rematch"]["game_id"])
            if game_two <= first_game:
                raise AssertionError("Rematch ID reused finished game")
            if connection.execute(
                "select count(*) from mg_game_results where game_id=?",
                (game_two,)
            ).fetchone()[0] != 0:
                raise AssertionError("Unfinished rematch received unauthorized score")
        # No secret deck/private_image field is copied into the public ledger.
        names = {item[1] for item in connection.execute("pragma table_info(mg_game_results)")}
        if {"private_image","secret_deck","hands","kitty"} & names:
            raise AssertionError("Settlement ledger leaked a private game field")
    print("m5h real read-only ledger " + stage + ": PASS"
          " (one credited game, original terminal sequence, three zero-sum seats)")


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
        print("mygames.m5f.full_game: PASS"
              " (20 physical cards, ~60 real commits, winner,"
              " rematch with a different landlord,"
              " two cold epoch takeovers, byte-identical Game ACK replay)")

if __name__ == "__main__":
    main()
