#!/usr/bin/env python3
"""Real local CHttp/WebSocket -> CFlow -> SQLite Game ACK integration.

Isolated sqlite datastore and 0600 recovery token manifest. No private hand,
CSPRNG shuffle, or bearer ticket is included in public logs/artifacts.
"""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def websocket_handshake_status(origins):
    """One real HTTP Upgrade, no bearer/private packet and no mock server."""
    request = (
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1:1780\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        + "".join("Origin: " + origin + "\r\n" for origin in origins)
        + "\r\n"
    ).encode("ascii")
    with socket.create_connection(("127.0.0.1", 1780), timeout=5) as connection:
        connection.settimeout(5)
        connection.sendall(request)
        first = bytearray()
        while b"\r\n" not in first:
            chunk = connection.recv(512)
            if not chunk or len(first) + len(chunk) > 4096:
                raise RuntimeError("WebSocket handshake response missing or oversized")
            first.extend(chunk)
        line = bytes(first).split(b"\r\n", 1)[0].split(b" ")
        if len(line) < 2 or not line[0].startswith(b"HTTP/"):
            raise RuntimeError("Invalid native CHttp WebSocket handshake status")
        return int(line[1])


def check_browser_origin_admission(phase):
    # Source-only parsing is not enough: require actual HTTP response before
    # any CFlow room/session issue. No secret values are ever sent or logged.
    for origins in (
        ["https://attacker.example"],
        ["http://localhost.attacker.example"],
        ["http://127.0.0.1.evil.example"],
        ["null"],
        ["http://localhost:65536"],
        ["http://localhost", "https://attacker.example"],
    ):
        result = websocket_handshake_status(origins)
        if result != 403:
            raise AssertionError(
                f"Untrusted browser Origin admitted unexpectedly: HTTP {result}"
            )
    if phase == "restart":
        # Run an accepted browser origin after the durable gameplay audit so
        # the native admission's provisional guest cannot perturb game IDs.
        result = websocket_handshake_status(["http://127.0.0.1:38888"])
        if result != 101:
            raise AssertionError(
                f"Valid local browser Origin was rejected: HTTP {result}"
            )
    print("mygames.m5d.browser_origin: PASS (real CHttp HTTP 403/101)", flush=True)


def run_server_stage(executable, environment, phase):
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as log:
        proc = subprocess.Popen(
            [executable],
            stdin=subprocess.PIPE,
            stdout=log,
            stderr=subprocess.STDOUT,
            env=environment,
        )
        failure = None
        try:
            ready = False
            for _ in range(100):
                if proc.poll() is not None:
                    raise RuntimeError("CFlow server exited before health admission")
                try:
                    with urllib.request.urlopen(
                        "http://127.0.0.1:1780/health", timeout=1
                    ) as response:
                        if response.status == 200:
                            ready = True
                            break
                except (OSError, urllib.error.URLError):
                    time.sleep(0.12)
            if not ready:
                raise RuntimeError("CFlow game WebSocket health probe expired")
            if phase == "first":
                check_browser_origin_admission(phase)
            subprocess.run(
                ["node", "scripts/ddz_m5d_ws_smoke.mjs", phase],
                env=environment, timeout=90, check=True,
            )
            if phase == "restart":
                check_browser_origin_admission(phase)
        except Exception as exc:
            failure = exc
        finally:
            if proc.poll() is None:
                try:
                    proc.communicate(input=b"\n", timeout=12)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=4)
            log.seek(0)
            # The C server does not log private game state or credentials.
            # Show only a short server diagnostic on failure.
            if failure or proc.returncode != 0:
                print("===== Native CHttp/CFlow debug tail =====", flush=True)
                print(log.read()[-9000:], flush=True)
        if failure:
            raise failure
        if proc.returncode != 0:
            raise RuntimeError(f"CFlow room/game server exited {proc.returncode}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: ci_ddz_m5d_websocket.py server sqlite_driver")
    server = str(pathlib.Path(sys.argv[1]).resolve())
    driver = str(pathlib.Path(sys.argv[2]).resolve())
    if not pathlib.Path(server).is_file() or not pathlib.Path(driver).is_file():
        raise RuntimeError("Installed CHttp/TurboDB exact SDK inputs missing")
    with tempfile.TemporaryDirectory(prefix="mygames-ddz-m5d-private-") as root:
        directory = pathlib.Path(root)
        database = directory / "game-round.sqlite"
        manifest = directory / "session-recovery.json"
        env = os.environ.copy()
        env["MYGAMES_SQLITE_PLUGIN"] = driver
        env["MYGAMES_DB_PATH"] = str(database)
        env["MYGAMES_GAME_M5D_AUDIT"] = str(manifest)
        run_server_stage(server, env, "first")
        if not database.is_file() or not manifest.is_file():
            raise AssertionError("No actual persisted game and private recovery manifest")
        if (manifest.stat().st_mode & 0o077) != 0:
            raise AssertionError("Recovery manifest is not private")
        # Separate CHttp/CFlow/ORM process reopens the exact physical DB.
        # It must atomically claim a newer Room Owner Epoch and continue
        # original per-player receipt/hand recovery without a stale Game ACK.
        run_server_stage(server, env, "restart")
        print("mygames.m5d.game_websocket: PASS (CFlow, actual SQLite,"
              " Game ACK after COMMIT, epoch takeover, restricted hand)")


if __name__ == "__main__":
    main()
