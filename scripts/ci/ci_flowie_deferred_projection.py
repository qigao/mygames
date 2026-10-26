#!/usr/bin/env python3
"""RED/GREEN real Broker session + Flowie #73 deferred inbound PUBACK.

The Broker process stays alive across Consumer SIGKILL restarts; its
session/subscription/inflight state is held by the Broker's configured
repository. Do NOT mistake this for Broker-process restart durability.
No test-only reconnect PUBLISH/reconciliation is permitted.
"""
from __future__ import annotations

import os
import pathlib
import secrets
import subprocess
import sys
import tempfile
import time

from ci_flowie_acl_smoke import check_ready, run

SOURCE_NAMESPACE = "mygames-ci-source-v1"


def require_marker(proc: subprocess.Popen, log, marker: str) -> None:
    check_ready(proc, log, marker, seconds=12)
    if proc.poll() is not None:
        raise AssertionError(f"Consumer exited before crash boundary {marker}")


def kill_at_boundary(client, mode: str, marker: str, paths: list[str],
                     env: dict[str, str], log_path: pathlib.Path) -> None:
    with log_path.open("w+t", encoding="utf-8") as log:
        p = subprocess.Popen([client, mode, *paths], stdout=log,
                             stderr=subprocess.STDOUT, env=env)
        try:
            require_marker(p, log, "MYGAMES_DEFERRED_SESSION_READY")
            if mode == "before-commit":
                # One and ONLY one business PUBLISH ever enters the broker.
                run([env["MYGAMES_PUBLISHER"], env["MYGAMES_DRIVER"],
                     env["MYGAMES_SOURCE_DB"], "--once"], env)
            require_marker(p, log, marker)
            if p.poll() is not None:
                raise AssertionError(f"Unexpected process exit in {mode}")
        finally:
            if p.poll() is None:
                p.kill()  # SIGKILL; never graceful MQTT DISCONNECT/PUBACK
            p.wait(timeout=6)
            if p.returncode == 0:
                raise AssertionError(f"Consumer unexpectedly exited cleanly: {mode}")


def successful_round(client: str, mode: str, marker: str, paths: list[str],
                     env: dict[str, str], log_path: pathlib.Path) -> None:
    with log_path.open("w+t", encoding="utf-8") as log:
        p = subprocess.Popen([client, mode, *paths], stdout=log,
                             stderr=subprocess.STDOUT, env=env)
        try:
            # Unlike SIGKILL boundaries, a successful subscriber is
            # permitted to finish before the parent observes its marker.
            # Requiring poll() is None here creates a false negative.
            p.wait(timeout=12)
            log.seek(0)
            output = log.read()
            if p.returncode != 0 or marker not in output:
                raise AssertionError(
                    f"Consumer {mode} failed rc={p.returncode}\n{output[-2400:]}")
        finally:
            if p.poll() is None:
                p.kill()
                p.wait(timeout=5)


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: ci_flowie_deferred_projection.py broker deferred-consumer "
            "publisher seed source-audit consumer-audit sqlite-driver")
    broker, client, publisher, seed, source_audit, consumer_audit, driver = [
        str(pathlib.Path(s).resolve()) for s in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-deferred-") as td:
        root = pathlib.Path(td)
        # Persist exact callback/crash process evidence when CI requests it.
        # Databases stay isolated in the temporary root; logs never print
        # generated service passwords or the SQLite event payload.
        evidence_env = os.environ.get("MYGAMES_DEFERRED_EVIDENCE_DIR")
        evidence = pathlib.Path(evidence_env).resolve() if evidence_env else root
        evidence.mkdir(parents=True, exist_ok=True)
        source_db = root / "source.sqlite"
        ledger_db = root / "consumer.sqlite"
        env = os.environ.copy()
        env.update({
            "MYGAMES_TEST_BROKER_SECURE": "1",
            "MYGAMES_TEST_PUBLISHER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_CONSUMER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_PLAYER_SECRET": secrets.token_hex(24),
            "MYGAMES_MQTT_USER": "svc-outbox",
            "MYGAMES_MQTT_PORT": "18893",
            "MYGAMES_DRIVER": driver,
            "MYGAMES_SOURCE_DB": str(source_db),
            "MYGAMES_PUBLISHER": publisher,
        })
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        consumer_args = [driver, str(ledger_db), SOURCE_NAMESPACE]
        run([seed, driver, str(source_db), "--seed"], env)
        run([source_audit, driver, str(source_db), "1"], env)
        with (evidence / "broker.log").open("w+t", encoding="utf-8") as brokerlog:
            b = subprocess.Popen([broker], stdin=subprocess.PIPE,
                                 stdout=brokerlog, stderr=subprocess.STDOUT,
                                 env=env)
            try:
                check_ready(b, brokerlog, "FLOWIE_SECURE_BROKER_READY")
                # Crash 1: callback admitted bytes and token, not even a
                # ConsumerProjection transaction yet. No ACK is submitted.
                kill_at_boundary(
                    client, "before-commit", "MYGAMES_DEFERRED_BEFORE_COMMIT",
                    consumer_args, env, evidence / "before-commit.log")
                run([consumer_audit, driver, str(ledger_db),
                     SOURCE_NAMESPACE, "0", "0"], env)

                # No second PUBLISH: only actual Broker session redelivery
                # can make this Subscriber receive seq=1 again.
                kill_at_boundary(
                    client, "after-commit-before-ack",
                    "MYGAMES_DEFERRED_AFTER_COMMIT_BEFORE_ACK",
                    consumer_args, env, evidence / "after-commit.log")
                run([consumer_audit, driver, str(ledger_db),
                     SOURCE_NAMESPACE, "1", "1"], env)

                # No republish/reconciliation: identical receipt must return
                # REPLAY; native Flowie worker sends the deferred PUBACK.
                successful_round(
                    client, "replay-and-ack",
                    "MYGAMES_DEFERRED_REPLAY_AND_NATIVE_PUBACK",
                    consumer_args, env, evidence / "replay-ack.log")
                run([consumer_audit, driver, str(ledger_db),
                     SOURCE_NAMESPACE, "1", "1"], env)

                # A further same-ID persistent session reconnect without a
                # new PUBLISH must not redeliver the settled packet.
                successful_round(
                    client, "post-ack-no-redelivery",
                    "MYGAMES_DEFERRED_AFTER_ACK_NO_REDELIVERY",
                    consumer_args, env, evidence / "after-ack.log")
                run([consumer_audit, driver, str(ledger_db),
                     SOURCE_NAMESPACE, "1", "1"], env)
                brokerlog.flush()
                brokerlog.seek(0)
                actual = brokerlog.read().count("BROKER_EVENT_ACCEPTED")
                if actual != 1:
                    raise AssertionError(
                        "Redelivery was accidentally simulated via republish "
                        f"(broker accepted {actual} events, expected 1)")
                run([source_audit, driver, str(source_db), "2"], env)
                # Write only after ALL four modes and post-reconnect audits
                # have independently passed; never promote partial logs to PASS.
                (evidence / "acceptance.txt").write_text(
                    "result=PASS\\n"
                    "flowie=exact-source-73\\n"
                    "broker_unique_business_publish=1\\n"
                    "first_crash=pre_consumer_commit\\n"
                    "second_crash=post_consumer_commit_pre_puback\\n"
                    "durable_cursor=1\\n"
                    "effect_count=1\\n"
                    "redelivery=broker_persistent_session_same_client_id\\n"
                    "native_puback=settlement_callback_success\\n"
                    "post_ack_reconnect=no_redelivery_observed\\n"
                    "broker_process_restart=NOT_TESTED\\n",
                    encoding="ascii",
                )
                print(
                    "mygames.flowie_deferred_projection: PASS "
                    "1 Broker PUBLISH, 2 SIGKILL windows, persistent-session "
                    "redelivery, TurboDB COMMIT, receipt REPLAY, native "
                    "settlement and post-ACK no-redelivery. Broker remained live.",
                    flush=True,
                )
            finally:
                if b.poll() is None:
                    assert b.stdin is not None
                    try:
                        b.stdin.write(b"\n")
                        b.stdin.flush()
                        b.wait(timeout=12)
                    except (BrokenPipeError, subprocess.TimeoutExpired):
                        b.kill()
                        b.wait(timeout=5)
                if b.returncode != 0:
                    brokerlog.flush()
                    brokerlog.seek(0)
                    raise RuntimeError(
                        "Native authenticated Broker exited nonzero: "
                        + brokerlog.read()[-1600:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
