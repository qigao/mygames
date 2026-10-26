#!/usr/bin/env python3
"""Real MQTT 5 Receive Maximum=1 across two TurboDB COMMIT/PUBACKs.

The Broker uses its own disk-backed Flowie protocol repository. The
subscriber advertises Receive Maximum=1 in its actual CONNECT properties.
After the Broker has accepted both independent PUBLISHes, the subscriber
is paused at a supervised stdin barrier before its first COMMIT: exactly
one QoS1 on_delivery must have crossed the wire and no effect/ACK happened.
After first durable COMMIT + native PUBACK the second event may deliver.
This is not a synthetic backpressure helper or a sleep-based inference.
"""
from __future__ import annotations

import os
import pathlib
import secrets
import subprocess
import sys
import tempfile

from ci_flowie_acl_smoke import check_ready, run
from ci_flowie_deferred_broker_restart import (
    start_broker, finish_broker, durable_deliveries,
)

NAMESPACE = "mygames-ci-source-v1"


def main() -> int:
    if len(sys.argv) != 8:
        raise SystemExit(
            "usage: ci_flowie_receive_maximum.py broker deferred-consumer "
            "publisher seed source-audit consumer-audit sqlite-driver")
    broker, consumer, publisher, seed, source_audit, consumer_audit, driver = [
        str(pathlib.Path(p).resolve()) for p in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-window-") as temp:
        root = pathlib.Path(temp)
        ev_arg = os.environ.get("MYGAMES_RECEIVE_WINDOW_EVIDENCE_DIR")
        evidence = pathlib.Path(ev_arg).resolve() if ev_arg else root
        evidence.mkdir(parents=True, exist_ok=True)
        source_db, ledger_db, broker_db = (
            root / "source.sqlite",
            root / "projection.sqlite",
            root / "broker.sqlite",
        )
        env = os.environ.copy()
        env.update({
            "MYGAMES_TEST_BROKER_SECURE": "1",
            "MYGAMES_TEST_BROKER_DB": str(broker_db),
            "MYGAMES_TEST_BROKER_DRIVER": driver,
            "MYGAMES_TEST_PUBLISHER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_CONSUMER_SECRET": secrets.token_hex(24),
            "MYGAMES_TEST_PLAYER_SECRET": secrets.token_hex(24),
            "MYGAMES_MQTT_USER": "svc-outbox",
            "MYGAMES_MQTT_PORT": "18893",
            "MYGAMES_MQTT_PASSWORD": "",
        })
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        run([seed, driver, str(source_db), "--seed"], env)
        run([source_audit, driver, str(source_db), "1"], env)
        b, brokerlog = start_broker(broker, env, evidence, 1)
        try:
            with (evidence / "consumer-window.log").open(
                "w+t", encoding="utf-8"
            ) as log:
                p = subprocess.Popen(
                    [consumer, "receive-window-two", driver, str(ledger_db),
                     NAMESPACE],
                    stdin=subprocess.PIPE, stdout=log,
                    stderr=subprocess.STDOUT, env=env,
                )
                try:
                    check_ready(p, log, "MYGAMES_DEFERRED_SESSION_READY")
                    # Both independent QoS1 application PUBLISHes are
                    # acknowledged upstream before releasing the subscriber.
                    run([publisher, driver, str(source_db), "--drain"], env)
                    run([source_audit, driver, str(source_db), "0"], env)
                    check_ready(
                        p, log, "MYGAMES_RECEIVE_WINDOW_FIRST_UNACKED"
                    )
                    brokerlog.flush()
                    brokerlog.seek(0)
                    accepted = brokerlog.read().count("BROKER_EVENT_ACCEPTED")
                    if accepted != 2:
                        raise AssertionError(
                            f"Broker accepted {accepted}, expected 2 distinct "
                            "business publications"
                        )
                    # Both packet-ID rows must be committed to the Broker's
                    # independent protocol SQLite repository, even if its
                    # Receive Maximum=1 window only allowed ONE PUBLISH on
                    # the subscriber's actual TCP connection.
                    durable_deliveries(broker_db, 2)
                    run([consumer_audit, driver, str(ledger_db), NAMESPACE,
                         "0", "0"], env)
                    assert p.stdin is not None
                    p.stdin.write(b"\n")
                    p.stdin.flush()
                    p.wait(timeout=15)
                    log.seek(0)
                    output = log.read()
                    if p.returncode != 0 or \
                            "MYGAMES_RECEIVE_WINDOW_FENCED" not in output or \
                            "MYGAMES_RECEIVE_WINDOW_TWO_COMMITTED" not in output:
                        raise AssertionError(
                            "Receive Maximum Client did not settle both events: "
                            + output[-2600:]
                        )
                    run([consumer_audit, driver, str(ledger_db), NAMESPACE,
                         "2", "2"], env)
                    durable_deliveries(broker_db, 0)
                    (evidence / "acceptance-receive-window.txt").write_text(
                        "result=PASS\n"
                        "mqtt5_receive_maximum=1\n"
                        "durable_broker_events_before_consumer_ack=2\n"
                        "qos1_callback_deliveries_before_first_puback=1\n"
                        "native_puback_after_turbodb_commit=2\n"
                        "consumer_cursor=2\n"
                        "consumer_effect_count=2\n"
                        "broker_delivery_rows_after_two_pubacks=0\n"
                        "synthetic_republish=0\n"
                        "broker_process_crash=NOT_TESTED_IN_THIS_SCENARIO\n",
                        encoding="ascii",
                    )
                    print(
                        "mygames.flowie_receive_maximum: PASS real MQTT5 "
                        "Receive Maximum=1, two durable Broker deliveries, "
                        "one pre-ACK Client admission, two serialized actual "
                        "TurboDB COMMITs and native PUBACK retirements",
                        flush=True,
                    )
                finally:
                    if p.poll() is None:
                        p.kill()
                        p.wait(timeout=5)
            finish_broker(b, brokerlog)
            b = None
        finally:
            if b is not None:
                if b.poll() is None:
                    b.kill()
                    b.wait(timeout=5)
                if not brokerlog.closed:
                    brokerlog.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
