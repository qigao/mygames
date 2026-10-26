#!/usr/bin/env python3
"""M4.4 genuine dual-network ingress, one Consumer Owner, one TurboDB ledger.

A real authenticated Flowie Broker delivers MQTT QoS1 through the native
Flowie callback, while independent Python/OpenSSL performs a genuine
CA-verified, client-authenticated TLS1.3 GET against the source READONLY
CHttp/CNet recovery service. The verified HTTP page is copied over a
TEST-ONLY IPC pipe into the same Subscriber process/ConsumerOwner. The
callback and HTTP ingress must both be applied by that one Owner exactly
once in the durable projection. This does NOT implement Flowie #22's
ACK-after-COMMIT, nor turn the fixture's IPC into a production HTTP client.
"""
from __future__ import annotations

import hashlib
import json
import os
import pathlib
import secrets
import select
import ssl
import subprocess
import sys
import tempfile
import urllib.request

from ci_flowie_acl_smoke import check_ready, run
from ci_recovery_mtls import generate_ca, generate_leaf


def stop_with_line(proc: subprocess.Popen, label: str) -> None:
    if proc.poll() is None and proc.stdin:
        try:
            proc.stdin.write(b"\n")
            proc.stdin.flush()
            proc.wait(timeout=12)
        except (BrokenPipeError, subprocess.TimeoutExpired):
            proc.kill()
            proc.wait(timeout=6)
    else:
        proc.wait(timeout=6)
    if proc.returncode != 0:
        raise AssertionError(f"{label}: abnormal exit code {proc.returncode}")


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit(
            "usage: ci_flowie_owner_live_mtls.py broker subscriber "
            "outbox-publisher seed outbox-audit consumer-audit "
            "recovery-service sqlite-plugin"
        )
    broker, subscriber, publisher, seed, audit, consumer_audit, service, plugin = [
        str(pathlib.Path(v).resolve()) for v in sys.argv[1:]
    ]
    with tempfile.TemporaryDirectory(prefix="mygames-owner-live-mtls-") as temp:
        root = pathlib.Path(temp)
        source_db = root / "source.sqlite"
        ledger_db = root / "consumer.sqlite"
        namespace = "mygames-ci-source-v1"
        consumer_id = "backend-projection-v1"

        env = os.environ.copy()
        env["MYGAMES_TEST_BROKER_SECURE"] = "1"
        env["MYGAMES_TEST_PUBLISHER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_CONSUMER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_TEST_PLAYER_SECRET"] = secrets.token_hex(24)
        env["MYGAMES_MQTT_USER"] = "svc-outbox"
        env["MYGAMES_MQTT_PASSWORD"] = env["MYGAMES_TEST_PUBLISHER_SECRET"]
        env["MYGAMES_MQTT_PORT"] = "18893"
        env["MYGAMES_TEST_PROJECTION_DB"] = str(ledger_db)
        env["MYGAMES_TEST_PROJECTION_DRIVER"] = plugin
        env["MYGAMES_TEST_SOURCE_NAMESPACE"] = namespace

        run([seed, plugin, str(source_db), "--seed"], env)
        run([audit, plugin, str(source_db), "1"], env)

        ca, ca_key = generate_ca(root, "m4-owner-CA")
        server_crt, server_key = generate_leaf(root, "server", ca, ca_key)
        client_crt, client_key = generate_leaf(root, "consumer", ca, ca_key)
        fingerprint = hashlib.sha256(
            ssl.PEM_cert_to_DER_cert(
                client_crt.read_text(encoding="ascii"))
        ).hexdigest()

        with (root / "broker.log").open("w+t", encoding="utf-8") as broker_log:
            broker_proc = subprocess.Popen(
                [broker], stdin=subprocess.PIPE, stdout=broker_log,
                stderr=subprocess.STDOUT, env=env)
            recovery = None
            try:
                check_ready(
                    broker_proc, broker_log, "FLOWIE_SECURE_BROKER_READY")
                recovery = subprocess.Popen(
                    [service, plugin, str(source_db),
                     str(server_crt), str(server_key), str(ca),
                     fingerprint, namespace, consumer_id],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, text=False, bufsize=0,
                    env=env,
                )
                assert recovery.stdout is not None
                ready, _, _ = select.select([recovery.stdout], [], [], 12)
                if not ready:
                    raise AssertionError("mTLS recovery source never became ready")
                line = recovery.stdout.readline().decode("ascii").strip()
                if not line.startswith("MYGAMES_RECOVERY_TLS_PORT="):
                    raise AssertionError(
                        "Invalid recovery listener startup marker: " + line)
                port = int(line.partition("=")[2])
                if not (1 <= port <= 65535):
                    raise AssertionError("Invalid recovery HTTPS port")

                with (root / "subscriber.log").open(
                    "w+t", encoding="utf-8"
                ) as subscriber_log:
                    sub = subprocess.Popen(
                        [subscriber, "subscriber-owner-mtls"],
                        stdin=subprocess.PIPE, stdout=subscriber_log,
                        stderr=subprocess.STDOUT, env=env,
                    )
                    try:
                        check_ready(
                            sub, subscriber_log,
                            "FLOWIE_ACL_SUBSCRIBER_READY")

                        # Use the DEFAULT OpenSSL TLS1.3 X25519-first HRR
                        # profile + compatibility CCS, with server SAN and
                        # CA verification and the matching client cert.
                        tls = ssl.create_default_context(cafile=str(ca))
                        tls.minimum_version = ssl.TLSVersion.TLSv1_3
                        tls.maximum_version = ssl.TLSVersion.TLSv1_3
                        tls.load_cert_chain(str(client_crt), str(client_key))
                        assert tls.options & ssl.OP_ENABLE_MIDDLEBOX_COMPAT
                        url = (
                            f"https://127.0.0.1:{port}"
                            "/internal/recovery/v1/events?after=0&limit=1"
                        )
                        with urllib.request.urlopen(
                            urllib.request.Request(url, method="GET"),
                            context=tls, timeout=8
                        ) as response:
                            if response.status != 200:
                                raise AssertionError("Authenticated GET not 200")
                            raw_page = response.read(32769)
                        if len(raw_page) == 0 or len(raw_page) > 32768:
                            raise AssertionError("HTTP response violates bound")
                        decoded = json.loads(raw_page)
                        if (decoded.get("source_namespace") != namespace or
                            decoded.get("consumer_id") != consumer_id or
                            decoded.get("resume_after") != 1 or
                            [v["seq"] for v in decoded.get("events", [])] != [1]):
                            raise AssertionError("Authenticated history mismatched")

                        # This is deliberately a bounded IPC handoff to the
                        # same Owner process, NOT an alternate production
                        # network client. The callback arrives independently
                        # from a real Flowie broker in its worker thread.
                        assert sub.stdin is not None
                        sub.stdin.write(f"{len(raw_page)}\n".encode("ascii"))
                        sub.stdin.write(raw_page)
                        sub.stdin.flush()
                        run([publisher, plugin, str(source_db), "--once"], env)
                        sub.wait(timeout=14)
                        subscriber_log.seek(0)
                        actual = subscriber_log.read()
                        if sub.returncode != 0 or (
                            "FLOWIE_OWNER_REAL_MTLS_HISTORY_PASS"
                            not in actual
                        ):
                            raise AssertionError(
                                "M4.4 real network Owner failed: "
                                + actual[-2200:]
                            )
                    finally:
                        if sub.poll() is None:
                            sub.kill()
                            sub.wait(timeout=5)
                run([consumer_audit, plugin, str(ledger_db), namespace,
                     "1", "1"], env)
                # The --seed fixture has at least events 1 and 2.
                # Publishing --once settles seq1, leaving seq2 pending.
                # Never interpret "one successful publish" as an empty
                # authoritative Outbox archive.
                run([audit, plugin, str(source_db), "2"], env)
                broker_log.flush()
                broker_log.seek(0)
                accepted = broker_log.read().count("BROKER_EVENT_ACCEPTED")
                if accepted != 1:
                    raise AssertionError(
                        f"Expected exactly one Broker event, got {accepted}")
                print("mygames.flowie_owner_real_mtls: PASS "
                      "broker MQTT ingress + real CA-verified HTTPS page "
                      "under one bounded Owner, one durable effect/receipt",
                      flush=True)
            finally:
                if recovery is not None:
                    try:
                        stop_with_line(recovery, "recovery source")
                    finally:
                        if recovery.stdout:
                            recovery.stdout.close()
                        if recovery.stderr:
                            recovery.stderr.close()
                stop_with_line(broker_proc, "authenticated Flowie broker")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
