#!/usr/bin/env python3
"""Real Flowie broker ACL qualification, strictly loopback and test-secret only.

Authenticate individual service principals; grant publishing solely to
svc-outbox and subscribing solely to svc-consumer. A player principal may
CONNECT but cannot access the internal room-event topic. Passwords are
generated per CI run and are never checked in, printed or uploaded.
"""
import os
import pathlib
import secrets
import subprocess
import sys
import tempfile
import time

def run(args, env, expected=0, timeout=15):
    proc = subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout, check=False)
    if proc.returncode != expected:
        raise AssertionError(
            f"ACL command {args[0]} expected={expected} actual={proc.returncode}\n"
            f"stdout={proc.stdout[-1200:]}\nstderr={proc.stderr[-1600:]}")
    return proc

def check_ready(proc, handle, needle, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            handle.seek(0)
            raise RuntimeError("Flowie process ended early: " + handle.read()[-1600:])
        handle.seek(0)
        if needle in handle.read():
            return
        time.sleep(0.08)
    raise RuntimeError("Flowie readiness timeout: " + needle)

def main() -> int:
    if len(sys.argv) != 7:
        raise SystemExit("usage: ci_flowie_acl_smoke.py broker client publisher seed audit sqlite-plugin")
    broker, client, publisher, seed, audit, plugin = [
        str(pathlib.Path(p).resolve()) for p in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-acl-") as temp:
        root = pathlib.Path(temp)
        database = str(root / "acl-events.sqlite")
        env = os.environ.copy()
        env["MYGAMES_TEST_BROKER_SECURE"] = "1"
        env["MYGAMES_TEST_PUBLISHER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_CONSUMER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_PLAYER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_MQTT_USER"] = "svc-outbox"
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        env["MYGAMES_MQTT_PORT"] = "18893"
        with (root / "broker.log").open("w+t", encoding="utf-8") as log:
            server = subprocess.Popen(
                [broker],stdin=subprocess.PIPE,stdout=log,
                stderr=subprocess.STDOUT,env=env)
            listener = None
            try:
                check_ready(server, log, "FLOWIE_SECURE_BROKER_READY")
                for mode in ("bad-secret", "publisher-subscribe-denied",
                             "player-subscribe-denied", "player-publish-denied",
                             "subscriber"):
                    run([client,mode],env)
                # None of these rejected role actions can reach the broker
                # application-level PUBLISH ingress callback.
                log.flush()
                log.seek(0)
                assert log.read().count("BROKER_EVENT_ACCEPTED") == 0

                run([seed,plugin,database,"--seed"],env)
                run([audit,plugin,database,"1"],env)
                with (root / "subscriber.log").open("w+t", encoding="utf-8") as sublog:
                    listener = subprocess.Popen(
                        [client,"subscriber-listen"],
                        stdout=sublog,stderr=subprocess.STDOUT,env=env)
                    check_ready(listener, sublog, "FLOWIE_ACL_SUBSCRIBER_READY")
                    run([publisher,plugin,database,"--once"],env)
                    deadline=time.monotonic()+8
                    while time.monotonic()<deadline and listener.poll() is None:
                        time.sleep(0.1)
                    if listener.poll()!=0:
                        sublog.seek(0)
                        raise AssertionError(
                            "Only authorized service Subscriber should get event: "+
                            sublog.read()[-1600:])
                    run([audit,plugin,database,"2"],env)
                run([publisher,plugin,database,"--drain"],env)
                run([audit,plugin,database,"0"],env)
                log.flush()
                log.seek(0)
                events=log.read().count("BROKER_EVENT_ACCEPTED")
                assert events == 2, f"secure broker accepted {events}, expected 2"
            finally:
                if listener is not None and listener.poll() is None:
                    listener.kill()
                    listener.wait(timeout=5)
                if server.poll() is None:
                    try:
                        server.communicate(input=b"\n",timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)
                        raise RuntimeError("Flowie secured broker did not stop")
                if server.returncode != 0:
                    log.seek(0)
                    raise RuntimeError("Flowie secured broker failure: "+log.read()[-1800:])
        print("mygames.flowie_acl: PASS "
              "(authenticated service roles, player deny, subscriber receive, "
              "internal Topic isolation)",flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
