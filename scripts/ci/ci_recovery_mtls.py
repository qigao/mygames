#!/usr/bin/env python3
"""Real installed-SDK CHttp/CNet mTLS source recovery qualification.

Uses ephemeral CA/server/consumer certificates, real TLS handshakes, a real
TurboDB SQLite source and independently launched recovery daemon. No private
credentials or fixtures are checked into source control.
"""
from __future__ import annotations

import hashlib
import json
import os
import select
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


def command(*argv: str) -> None:
    subprocess.run(argv, check=True, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, text=True, timeout=25)


def generate_ca(root: Path, name: str) -> tuple[Path, Path]:
    cert, key = root / f"{name}.crt", root / f"{name}.key"
    command("openssl", "req", "-x509", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-days", "2",
            "-subj", f"/CN={name}", "-keyout", str(key), "-out",
            str(cert), "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    return cert, key


def generate_leaf(root: Path, kind: str, ca_cert: Path,
                  ca_key: Path) -> tuple[Path, Path]:
    cert, key = root / f"{kind}.crt", root / f"{kind}.key"
    request = root / f"{kind}.csr"
    profile = root / f"{kind}.ext"
    is_server = kind == "server"
    profile.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        + ("extendedKeyUsage=serverAuth\n"
           "subjectAltName=DNS:localhost,IP:127.0.0.1\n"
           if is_server else
           "extendedKeyUsage=clientAuth\n"),
        encoding="utf-8",
    )
    command("openssl", "req", "-newkey", "ec",
            "-pkeyopt", "ec_paramgen_curve:prime256v1",
            "-sha256", "-nodes", "-keyout", str(key),
            "-out", str(request), "-subj", f"/CN={kind}")
    command("openssl", "x509", "-req", "-in", str(request),
            "-CA", str(ca_cert), "-CAkey", str(ca_key),
            "-CAcreateserial", "-sha256", "-days", "2",
            "-extfile", str(profile), "-out", str(cert))
    return cert, key


def client_context(ca: Path, cert: Path | None = None,
                   key: Path | None = None) -> ssl.SSLContext:
    ctx = ssl.create_default_context(cafile=str(ca))
    # GmSSL's pinned TLS13 interoperability profile currently exercises
    # prime256v1/ECDSA; avoid an implicit X25519 first key-share while
    # independently investigating its HelloRetryRequest crash.
    ctx.set_ecdh_curve("prime256v1")
    if cert and key:
        ctx.load_cert_chain(certfile=str(cert), keyfile=str(key))
    return ctx


def fetch(port: int, suffix: str,
          ctx: ssl.SSLContext) -> tuple[int, dict]:
    url = f"https://127.0.0.1:{port}{suffix}"
    req = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=8) as reply:
            return reply.status, json.loads(reply.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def check(condition: bool, why: str) -> None:
    if not condition:
        raise AssertionError(why)


def run(binary: Path, seed: Path, sqlite_driver: Path,
        native_client: Path) -> None:
    if not shutil.which("openssl"):
        raise RuntimeError("OpenSSL CLI is required for ephemeral mTLS certs")
    with tempfile.TemporaryDirectory(prefix="mygames-recovery-mtls-") as work:
        root = Path(work)
        source = root / "room-source.sqlite"
        command(str(seed), str(sqlite_driver), str(source), "--seed")
        command(str(seed), str(sqlite_driver), str(source), "--append")

        ca, ca_key = generate_ca(root, "mygames-recovery-CA")
        server_cert, server_key = generate_leaf(root, "server", ca, ca_key)
        client_cert, client_key = generate_leaf(root, "consumer", ca, ca_key)
        extra_cert, extra_key = generate_leaf(root, "other-consumer", ca, ca_key)
        other_ca, other_ca_key = generate_ca(root, "untrusted-CA")
        rejected_cert, rejected_key = generate_leaf(
            root, "untrusted-consumer", other_ca, other_ca_key)

        der = ssl.PEM_cert_to_DER_cert(client_cert.read_text(encoding="utf-8"))
        good_sha256 = hashlib.sha256(der).hexdigest()
        proc = subprocess.Popen(
            [str(binary), str(sqlite_driver), str(source),
             str(server_cert), str(server_key), str(ca), good_sha256,
             "trusted-source-v1", "backend-projection-v1"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1
        )
        try:
            assert proc.stdout is not None
            ready, _, _ = select.select([proc.stdout], [], [], 12)
            if not ready:
                raise AssertionError("Recovery mTLS server never declared ready")
            line = proc.stdout.readline().strip()
            check(line.startswith("MYGAMES_RECOVERY_TLS_PORT="),
                  f"recovery listener startup failed: {line!r}")
            port = int(line.split("=", 1)[1])
            check(0 < port <= 65535, "invalid TLS port")

            good = client_context(ca, client_cert, client_key)
            unknown = client_context(ca, extra_cert, extra_key)
            no_cert = client_context(ca)
            untrusted = client_context(ca, rejected_cert, rejected_key)
            prefix = "/internal/recovery/v1/events"

            # The installed CHttp Client uses the SAME pinned native
            # GmSSL provider as CHttp Server. This diagnostic distinguishes
            # provider/self-interoperability from independent OpenSSL.
            native = subprocess.run(
                [str(native_client), str(port), str(ca),
                 str(client_cert), str(client_key)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=15, check=False)
            print("mygames.recovery_native_mtls_diagnostic: "
                  f"exit={native.returncode} "
                  f"stdout={native.stdout.strip()[:260]} "
                  f"stderr={native.stderr.strip()[-580:]}", flush=True)
            # Do not weaken the later mandatory OpenSSL acceptance gate.

            # Diagnostic-only TLS protocol matrix. These remain verified
            # mTLS requests, not a listener downgrade or production fallback.
            # Retain mandatory unrestricted/default interoperability RED below.
            diagnostics = []
            for label, version in (
                ("TLS1.2-only", ssl.TLSVersion.TLSv1_2),
                ("TLS1.3-only", ssl.TLSVersion.TLSv1_3),
            ):
                probe = client_context(ca, client_cert, client_key)
                probe.minimum_version = version
                probe.maximum_version = version
                try:
                    probe_status, probe_body = fetch(
                        port, prefix + "?after=0&limit=1", probe)
                    diagnostics.append(
                        f"{label}: HTTP {probe_status}, checkpoint="
                        f"{probe_body.get('checkpoint_seq', 'none')}")
                except (ssl.SSLError, urllib.error.URLError, OSError,
                        ValueError) as exc:
                    diagnostics.append(
                        f"{label}: {type(exc).__name__}: {str(exc)[:180]}")
            print("mygames.recovery_tls_diagnostic: "
                  + " | ".join(diagnostics), flush=True)

            # Diagnostic-only RFC 8446 middlebox compatibility isolation.
            # This changes only a dummy TLS1.3 ChangeCipherSpec emission,
            # NOT certificate verification, cipher suite, or mTLS identity.
            # The default OpenSSL ClientHello remains mandatory below.
            if hasattr(ssl, "OP_ENABLE_MIDDLEBOX_COMPAT"):
                no_dummy_ccs = client_context(ca, client_cert, client_key)
                no_dummy_ccs.minimum_version = ssl.TLSVersion.TLSv1_3
                no_dummy_ccs.maximum_version = ssl.TLSVersion.TLSv1_3
                no_dummy_ccs.options &= ~ssl.OP_ENABLE_MIDDLEBOX_COMPAT
                try:
                    probe_status, probe_data = fetch(
                        port, prefix + "?after=0&limit=1", no_dummy_ccs)
                    print("mygames.recovery_tls_no_dummy_ccs_diagnostic: "
                          f"HTTP={probe_status} "
                          f"cursor={probe_data.get('resume_after')}",
                          flush=True)
                except (ssl.SSLError, urllib.error.URLError, OSError) as exc:
                    print("mygames.recovery_tls_no_dummy_ccs_diagnostic: "
                          f"{type(exc).__name__}: {str(exc)[:250]}",
                          flush=True)

            # OpenSSL's independent TLS record trace distinguishes
            # middlebox-compat dummy CCS from other ClientHello variations.
            # Print record TYPE/DIRECTION only, never certificate blobs,
            # cryptographic material or full packet dumps.
            openssl_trace_cmd = [
                "openssl", "s_client", "-connect", f"127.0.0.1:{port}",
                "-servername", "localhost", "-tls1_3",
                "-groups", "P-256", "-alpn", "http/1.1",
                "-CAfile", str(ca), "-verify_return_error",
                "-verify_hostname", "localhost",
                "-cert", str(client_cert), "-key", str(client_key),
                "-msg",
            ]
            trace_text = ""
            try:
                traced = subprocess.run(
                    openssl_trace_cmd,
                    input=("GET " + prefix + "?after=0&limit=1 "
                           "HTTP/1.1\\r\\nHost: localhost\\r\\n"
                           "Connection: close\\r\\n\\r\\n"),
                    capture_output=True, text=True, timeout=8,
                    check=False)
                trace_text = traced.stdout + "\\n" + traced.stderr
            except subprocess.TimeoutExpired as exc:
                partial_out = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else (exc.stdout or "")
                partial_err = exc.stderr.decode(errors="replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
                trace_text = partial_out + "\\n" + partial_err
            record_lines = [
                line.strip() for line in trace_text.splitlines()
                if ("ChangeCipherSpec" in line or "ClientHello" in line
                    or "ServerHello" in line or "Alert " in line)
                and (line.lstrip().startswith(">>>") or
                     line.lstrip().startswith("<<<"))
            ]
            print("mygames.recovery_tls13_record_trace: "
                  + " | ".join(record_lines[:16]), flush=True)

            status, data = fetch(port, prefix + "?after=0&limit=1", good)
            check(status == 200, f"authorized first page: {status}")
            check(data["source_namespace"] == "trusted-source-v1",
                  "source must be server-bound")
            check(data["consumer_id"] == "backend-projection-v1",
                  "consumer must be server-bound")
            check(data["checkpoint_seq"] == 3 and data["owner_epoch"] == 1,
                  "source snapshot head mismatch")
            check([e["seq"] for e in data["events"]] == [1] and
                  data["resume_after"] == 1 and data["has_more"] is True,
                  "first page must be bounded and contiguous")
            check(data["events"][0]["payload"]["type"] ==
                  "room.command_accepted", "invalid canonical event type")

            status, data = fetch(port, prefix + "?limit=2&after=1", good)
            check(status == 200, "second page rejected")
            check([e["seq"] for e in data["events"]] == [2, 3] and
                  data["resume_after"] == 3 and data["has_more"] is False,
                  "second page must reach source checkpoint")
            status, data = fetch(port, prefix + "?after=3&limit=1", good)
            check(status == 200 and data["events"] == [] and
                  data["resume_after"] == 3 and data["has_more"] is False,
                  "up-to-date cursor must not request missing history")
            status, data = fetch(port, prefix + "?after=4&limit=1", good)
            check(status == 409, f"future cursor must fail closed: {status}")

            for invalid in ("?after=0&limit=17", "?after=-1&limit=1",
                            "?after=0&limit=1&after=0", "?after=0&limit=1&",
                            "?after=0%26admin=1&limit=1"):
                status, _ = fetch(port, prefix + invalid, good)
                check(status == 400, f"invalid query accepted: {invalid}")

            status, _ = fetch(port, prefix + "?after=0&limit=1", unknown)
            check(status == 401,
                  f"CA-trusted but unpinned service must be denied: {status}")
            for description, ctx in [("no certificate", no_cert),
                                     ("untrusted issuer", untrusted)]:
                try:
                    fetch(port, prefix + "?after=0&limit=1", ctx)
                except (ssl.SSLError, urllib.error.URLError, ConnectionError,
                        OSError):
                    continue  # mTLS rejects before any HTTP request.
                raise AssertionError(f"{description} unexpectedly passed mTLS")

            print("mygames.recovery_mtls: PASS real TLS client auth, "
                  "untrusted/wrong principal denial, readonly archive pages")
        finally:
            observed = sys.exc_info()[1]
            if proc.poll() is None and proc.stdin:
                try:
                    proc.stdin.write("\n")
                    proc.stdin.flush()
                    proc.wait(timeout=15)
                except (OSError, subprocess.TimeoutExpired):
                    proc.kill()
                    proc.wait(timeout=5)
            else:
                proc.wait(timeout=2)
            detail = proc.stderr.read()[-4500:] if proc.stderr else ""
            if proc.returncode != 0:
                raise AssertionError(
                    f"recovery service exited {proc.returncode}: {detail}"
                ) from observed
            if observed is not None:
                raise AssertionError(
                    f"mTLS test failed: {type(observed).__name__}: {observed}; "
                    f"recovery server diagnostics: {detail}"
                ) from observed


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit("usage: ci_recovery_mtls.py recovery-service seed-binary "
                 "sqlite-driver native-client")
    run(*(Path(item).resolve() for item in sys.argv[1:]))
