#!/usr/bin/env python3
"""Flowie #73 + MyGames real TurboDB after **Broker process** SIGKILL.

This is an independent, opt-in disk-backed Broker qualification. The real
Flowie protocol repository is a third database, distinct from the room
source and consumer ledger. After each crash, use ONLY its durable protocol
state; do not simulate redelivery by re-PUBLISH or historical catch-up.
"""
from __future__ import annotations

import os
import pathlib
import secrets
import sqlite3
import subprocess
import sys
import tempfile
import time

from ci_flowie_acl_smoke import check_ready, run
from ci_flowie_deferred_projection import (
    SOURCE_NAMESPACE, kill_at_boundary, successful_round,
)

PERSISTED_DELIVERIES = "mygames_ci_broker_deliveries"
PERSISTED_SESSIONS = "mygames_ci_broker_sessions"
CONSUMER_CLIENT_ID = "mygames-consumer-v1"


def durable_deliveries(db: pathlib.Path, expected: int) -> None:
    """Wait for a read-only SQLite observation of an actually committed row."""
    deadline = time.monotonic() + 8.0
    observed = None
    while time.monotonic() < deadline:
        try:
            # mode=ro never creates a missing database; read via a separate
            # SQLite connection rather than trusting the Broker's log.
            with sqlite3.connect(db.as_uri() + "?mode=ro", uri=True,
                                 timeout=0.5) as conn:
                conn.execute("PRAGMA query_only=ON")
                row = conn.execute(
                    f"SELECT COUNT(*) FROM {PERSISTED_DELIVERIES} "
                    "WHERE client_id=?", (CONSUMER_CLIENT_ID,)).fetchone()
                session = conn.execute(
                    f"SELECT expiry_interval,revision FROM {PERSISTED_SESSIONS} "
                    "WHERE client_id=?", (CONSUMER_CLIENT_ID,)).fetchone()
            observed = (int(row[0]) if row else -1, session)
            if observed[0] == expected and session and int(session[0]) == 3600:
                return
        except sqlite3.Error:
            pass
        time.sleep(0.04)
    raise AssertionError(
        f"Broker durable session delivery count expected={expected}, observed={observed}")


def start_broker(broker: str, env: dict[str, str],
                 evidence: pathlib.Path, epoch: int):
    log = (evidence / f"broker-{epoch}.log").open("w+t", encoding="utf-8")
    proc = subprocess.Popen(
        [broker], stdin=subprocess.PIPE, stdout=log,
        stderr=subprocess.STDOUT, env=env)
    try:
        check_ready(proc, log, "FLOWIE_SECURE_BROKER_READY")
    except BaseException:
        proc.kill()
        proc.wait(timeout=5)
        log.close()
        raise
    return proc, log


def crash_broker(proc: subprocess.Popen, log) -> None:
    if proc.poll() is not None:
        log.seek(0)
        raise AssertionError("Broker unexpectedly exited:\n" + log.read()[-1800:])
    proc.kill()  # Broker crash, not a graceful stop/flush
    proc.wait(timeout=7)
    log.close()
    if proc.returncode != -9:
        raise AssertionError("Broker process did not terminate with SIGKILL")


def finish_broker(proc: subprocess.Popen, log) -> None:
    if proc.poll() is None:
        assert proc.stdin is not None
        try:
            proc.stdin.write(b"\n")
            proc.stdin.flush()
            proc.wait(timeout=10)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            proc.kill()
            proc.wait(timeout=5)
    log.flush()
    log.seek(0)
    summary = log.read()
    log.close()
    if proc.returncode != 0:
        raise AssertionError("Broker final graceful shutdown failed: " + summary[-1600:])


def assert_single_publish(evidence: pathlib.Path) -> None:
    total = 0
    for path in sorted(evidence.glob("broker-*.log")):
        total += path.read_text(encoding="utf-8").count("BROKER_EVENT_ACCEPTED")
    if total != 1:
        raise AssertionError(
            f"Expected one real publisher admission across ALL 4 Broker lifetimes, saw {total}")


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: ci_flowie_deferred_broker_restart.py broker deferred-consumer "
            "publisher seed source-audit consumer-audit sqlite-driver")
    broker, consumer, publisher, seed, source_audit, consumer_audit, driver = [
        str(pathlib.Path(value).resolve()) for value in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-broker-restart-") as td:
        root = pathlib.Path(td)
        evidence_arg = os.environ.get("MYGAMES_DEFERRED_BROKER_EVIDENCE_DIR")
        evidence = pathlib.Path(evidence_arg).resolve() if evidence_arg else root
        evidence.mkdir(parents=True, exist_ok=True)
        db = root / "broker-protocol.sqlite"
        source = root / "source.sqlite"
        ledger = root / "consumer.sqlite"
        env = os.environ.copy()
        env.update({
            "MYGAMES_TEST_BROKER_SECURE": "1",
            "MYGAMES_TEST_BROKER_DB": str(db),
            "MYGAMES_TEST_BROKER_DRIVER": driver,
            "MYGAMES_TEST_PUBLISHER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_CONSUMER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_PLAYER_SECRET": secrets.token_hex(24),
            "MYGAMES_MQTT_USER": "svc-outbox",
            "MYGAMES_MQTT_PORT": "18893",
            "MYGAMES_DRIVER": driver,
            "MYGAMES_SOURCE_DB": str(source),
            "MYGAMES_PUBLISHER": publisher,
        })
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        args = [driver, str(ledger), SOURCE_NAMESPACE]
        run([seed, driver, str(source), "--seed"], env)
        run([source_audit, driver, str(source), "1"], env)

        broker_proc, broker_log = start_broker(broker, env, evidence, 1)
        try:
            kill_at_boundary(
                consumer, "before-commit", "MYGAMES_DEFERRED_BEFORE_COMMIT",
                args, env, evidence / "consumer-pre-commit.log")
            run([consumer_audit, driver, str(ledger), SOURCE_NAMESPACE, "0", "0"], env)
            durable_deliveries(db, 1)
            crash_broker(broker_proc, broker_log)

            # Broker process and its in-memory session state are gone. This
            # recovery must reload protocol PUBLISH from the exact SQLite DB.
            broker_proc, broker_log = start_broker(broker, env, evidence, 2)
            durable_deliveries(db, 1)
            kill_at_boundary(
                consumer, "after-commit-before-ack",
                "MYGAMES_DEFERRED_AFTER_COMMIT_BEFORE_ACK",
                args, env, evidence / "consumer-post-commit.log")
            run([consumer_audit, driver, str(ledger), SOURCE_NAMESPACE, "1", "1"], env)
            durable_deliveries(db, 1)
            crash_broker(broker_proc, broker_log)

            broker_proc, broker_log = start_broker(broker, env, evidence, 3)
            durable_deliveries(db, 1)
            successful_round(
                consumer, "replay-and-ack",
                "MYGAMES_DEFERRED_REPLAY_AND_NATIVE_PUBACK",
                args, env, evidence / "consumer-replay-ack.log")
            run([consumer_audit, driver, str(ledger), SOURCE_NAMESPACE, "1", "1"], env)
            durable_deliveries(db, 0)  # Broker-side ACK retirement persisted
            crash_broker(broker_proc, broker_log)

            broker_proc, broker_log = start_broker(broker, env, evidence, 4)
            durable_deliveries(db, 0)
            successful_round(
                consumer, "post-ack-no-redelivery",
                "MYGAMES_DEFERRED_AFTER_ACK_NO_REDELIVERY",
                args, env, evidence / "consumer-post-ack.log")
            run([consumer_audit, driver, str(ledger), SOURCE_NAMESPACE, "1", "1"], env)
            run([source_audit, driver, str(source), "2"], env)
            finish_broker(broker_proc, broker_log)
            broker_proc = None
            assert_single_publish(evidence)
            (evidence / "acceptance-broker-restart.txt").write_text(
                "result=PASS\nbroker_database=independent_turbodb_sqlite\n"
                "broker_sigkill=3\nbroker_startups=4\n"
                "consumer_sigkill_precommit=1\nconsumer_sigkill_postcommit=1\n"
                "broker_delivery_row=1_after_early_crashes\n"
                "broker_delivery_row=0_after_puback\n"
                "business_publish_count=1\nprojection_effect_count=1\n"
                "session_resume=same_client_id\n"
                "last_reconnect=no_redelivery\n"
                "cross_host_fencing=NOT_TESTED\nhardware_power_cut=NOT_TESTED\n",
                encoding="ascii",
            )
            print("mygames.flowie_deferred_broker_restart: PASS "
                  "three Broker SIGKILL, four persisted-SQLite Broker startups, "
                  "two Consumer SIGKILL windows, actual one-PUBLISH redelivery, "
                  "native ACK retirement and one durable ConsumerProjection effect",
                  flush=True)
        finally:
            if broker_proc is not None:
                if broker_proc.poll() is None:
                    broker_proc.kill()
                    broker_proc.wait(timeout=6)
                if not broker_log.closed:
                    broker_log.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
