#!/usr/bin/env python3
"""Real authenticated Flowie Broker + Subscriber transactional projection tests.

Uses test-only loopback ACL, per-run random credentials, a distinct TurboDB
consumer DB, and a stable source namespace. Validates the concrete MQTT
duplicate caused by publisher crash after PUBACK but before Outbox ACK.
The subscriber projection update is done outside Flowie's callback.
"""
import os
import pathlib
import secrets
import subprocess
import sys
import tempfile
import time

def run(args, env, expected=0, timeout=18):
    proc = subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout, check=False)
    if proc.returncode != expected:
        raise AssertionError(
            f"ACL command {pathlib.Path(args[0]).name} {args[-1]} "
            f"expected={expected} actual={proc.returncode}\n"
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
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: ci_flowie_acl_smoke.py "
            "broker client publisher seed outbox_audit consumer_audit sqlite-plugin"
        )
    broker, client, publisher, seed, audit, consumer_audit, plugin = [
        str(pathlib.Path(p).resolve()) for p in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-acl-") as temp:
        root = pathlib.Path(temp)
        database = str(root / "source-events.sqlite")
        consumer_db = str(root / "subscriber-projection.sqlite")
        namespace = "mygames-ci-source-v1"
        env = os.environ.copy()
        env["MYGAMES_TEST_BROKER_SECURE"] = "1"
        env["MYGAMES_TEST_PUBLISHER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_CONSUMER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_PLAYER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_MQTT_USER"] = "svc-outbox"
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        env["MYGAMES_MQTT_PORT"] = "18893"
        env["MYGAMES_TEST_PROJECTION_DB"] = consumer_db
        env["MYGAMES_TEST_PROJECTION_DRIVER"] = plugin
        env["MYGAMES_TEST_SOURCE_NAMESPACE"] = namespace
        with (root / "broker.log").open("w+t", encoding="utf-8") as log:
            server = subprocess.Popen(
                [broker], stdin=subprocess.PIPE, stdout=log,
                stderr=subprocess.STDOUT, env=env)
            try:
                check_ready(server, log, "FLOWIE_SECURE_BROKER_READY")
                for mode in ("bad-secret", "publisher-subscribe-denied",
                             "player-subscribe-denied", "player-publish-denied",
                             "subscriber"):
                    run([client, mode], env)
                log.flush()
                log.seek(0)
                assert log.read().count("BROKER_EVENT_ACCEPTED") == 0

                run([seed, plugin, database, "--seed"], env)
                run([audit, plugin, database, "1"], env)

                def durable_audit(expected_seq):
                    run([consumer_audit, plugin, consumer_db, namespace,
                         str(expected_seq), str(expected_seq)], env)

                def subscriber_round(label, publish_mode, expected_result,
                                     expected_seq, crash=False):
                    listener = None
                    with (root / f"subscriber-{label}.log").open(
                            "w+t", encoding="utf-8") as sublog:
                        listen_env = env.copy()
                        listen_env["MYGAMES_TEST_EXPECT_PROJECTION"] = expected_result
                        listener = subprocess.Popen(
                            [client,"subscriber-listen"],stdout=sublog,
                            stderr=subprocess.STDOUT,env=listen_env)
                        try:
                            check_ready(listener, sublog, "FLOWIE_ACL_SUBSCRIBER_READY")
                            publish_env = env.copy()
                            if crash:
                                publish_env["MYGAMES_TEST_CRASH_AFTER_PUBACK"] = "1"
                            run([publisher, plugin, database, publish_mode],
                                publish_env, expected=77 if crash else 0)
                            try:
                                listener.wait(timeout=9)
                            except subprocess.TimeoutExpired:
                                listener.kill()
                                listener.wait(timeout=5)
                                raise AssertionError("Authenticated subscriber hung")
                            sublog.seek(0)
                            output = sublog.read()
                            if listener.returncode != 0 or \
                               f"FLOWIE_ACL_PROJECTION_{expected_result.upper()}" not in output:
                                raise AssertionError(
                                    "Authenticated subscriber projection failed: "+
                                    output[-1700:])
                            durable_audit(expected_seq)
                        finally:
                            if listener.poll() is None:
                                listener.kill()
                                listener.wait(timeout=5)

                # Broker delivered event #1, publisher exits deliberately
                # before acknowledging the Outbox row in SQLite.
                subscriber_round("initial", "--once", "applied", 1, crash=True)
                run([audit,plugin,database,"1"],env)
                log.flush()
                log.seek(0)
                assert log.read().count("BROKER_EVENT_ACCEPTED") == 1

                # Publisher restart sends event #1 again. Subscriber restart
                # must REPLAY the durable receipt without a second effect.
                subscriber_round("duplicate", "--once", "replay", 1)
                run([audit,plugin,database,"2"],env)

                # Event #2 is new and advances durable projection to seq=2.
                subscriber_round("second", "--drain", "applied", 2)
                run([audit,plugin,database,"0"],env)
                run([publisher,plugin,database,"--drain"],env)
                log.flush()
                log.seek(0)
                events = log.read().count("BROKER_EVENT_ACCEPTED")
                assert events == 3, f"secure broker accepted {events}, expected 3"
            finally:
                if server.poll() is None:
                    try:
                        server.communicate(input=b"\n", timeout=8)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)
                        raise RuntimeError("Flowie secured broker did not stop")
                if server.returncode != 0:
                    log.seek(0)
                    raise RuntimeError("Flowie secured broker failure: "+
                                       log.read()[-1800:])
        print("mygames.flowie_acl_and_projection: PASS "
              "(service ACL, player deny, durable MQTT duplicate replay, "
              "atomic consumer cursor/effect after subscriber restart)",flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
