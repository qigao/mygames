# Flowie MQTT Outbox Publisher v1 — service-internal, at least once

This module is a **separate executable**, not a network callback or a task
that blocks the CFlow Room Owner. It uses the released **Flowie 1.0.4 C11
Client** and the existing committed TurboDB Outbox.

The authoritative chain is:

    authenticated Room Owner (future)
      -> CFlow room mutation
      -> TurboDB COMMIT (Checkpoint + exact command result + sanitized Outbox)
      -> Game ACK to original requester
      -> independent mygames_outbox_publisher
      -> Flowie::Client MQTT v5 PUBLISH QoS1
      -> valid PUBACK reason < 0x80
      -> TurboDB mg_outbox.delivered=1 COMMIT

**MQTT PUBACK is not Game ACK, nor proof every subscriber processed a
message.** Flowie client command-queue admission is also not PUBACK.

## SDK and operation

Compile with both `MYGAMES_ENABLE_TURBODB=ON` and
`MYGAMES_ENABLE_FLOWIE=ON`, using installed compatible Salts, SaltsUtils,
CHttp, TurboDB, Flowie, and pinned GmSSL roots. Flowie SDK CMake requires
`FLOWIE_ROOT`, `SALTS_ROOT`, `SALTS_UTILS_ROOT`,
`TURBODB_ROOT`, and `HTTP_SERVICES_ROOT` to be set consistently.

    cmake -S . -B build/gateway -DMYGAMES_ENABLE_GATEWAY=ON \
      -DMYGAMES_ENABLE_TURBODB=ON -DMYGAMES_ENABLE_FLOWIE=ON \
      -DCMAKE_PREFIX_PATH=/path/to/coherent/sdk
    cmake --build build/gateway --target mygames_outbox_publisher

    MYGAMES_MQTT_PORT=18893 ./build/gateway/mygames_outbox_publisher \
      /absolute/turbodb_driver_sqlite.so /absolute/mygames.sqlite --once

Modes: `--once` publishes at most one confirmed event; `--drain` consumes
currently pending entries in sequence order until empty; `--watch` repeats
the same bounded attempt as new entries appear. The CLI returns nonzero on
broker CONNECT failure, MQTT settlement uncertainty, malformed public
payload, or database ACK failure so a supervisor can restart it.

The current adapter is deliberately **TCP loopback-only at 127.0.0.1**.
It does not offer public or remote plaintext connections. Environment
variables `MYGAMES_MQTT_USER` and `MYGAMES_MQTT_PASSWORD` can supply a
local test service identity, but they do not substitute for production
TLS/mTLS, credential rotation or a verified Flowie Broker ACL.

## Topic and privacy boundary

The only v1 topic is:

    mygames/internal/v1/room-events

This is a **service-to-service internal topic**, not a client- or
spectator-subscribable public room feed. Its ACL is **not** configured by
MyGames; production Flowie deployments MUST explicitly grant PUBLISH to
the Outbox publisher service principal and SUBSCRIBE only to authorized
backend services. Player clients use authenticated CHttp room snapshots,
not MQTT access to this topic.

The Publisher has a strict canonical allowlist: it permits only
`room.command_accepted` and `room.session_expired` events emitted by
the current native Room Owner contract, validates their exact fields,
global committed sequence, and positive owner epoch no greater than the
current database owner epoch. Unknown keys, cards, session tokens, nested
JSON, arbitrary event types and malformed/extra bytes are **fail-closed**.
It never publishes private hand cards or CHttp recovery credentials.

Future event additions require an explicit versioned schema, privacy
classification and negative tests before entering this allowlist.

## Settlement and failure contracts

- **Positive PUBACK**: after the Flowie callback reports success and reason
  code below 0x80, the publisher ACKs exactly that Outbox sequence in a
  separate epoch-checked TurboDB transaction.
- **Flowie queue FULL, broker disconnected, negative PUBACK or timeout**:
  no Outbox ACK. Publisher exits nonzero, and a supervisor may retry later.
- **Publisher death after PUBACK but before DB ACK**: at-least-once duplicate.
  The same committed event is sent again on restart. No event is silently
  lost; downstream consumers must deduplicate by committed global `seq`
  (and the durable database/tenant namespace).
- **Owner Epoch changes while publishing**: database ACK rejects the stale
  epoch. Publisher must re-read the snapshot/epoch and retry. The broker
  may already have accepted a copy, hence consumers deduplicate.
- **Outbox row/Schema corruption**: fail closed and do not publish.
- **Retained MQTT messages**: disabled; TurboDB Outbox is the source of truth.
- **Service lifecycle**: Flowie MQTT callbacks update atomic completion flags
  only. They never call TurboDB, RoomDirectory, or destroy their own client.
  Destruction is performed after the publisher observes the callback terminal
  result and joins Flowie's client worker.

## Exact installed-SDK qualification

Linux SDK CI downloads **Flowie.Native 1.0.4** (SHA-256 pinned), builds
the *real* publisher against `Flowie::Client` and starts the *real*
`Flowie::Flowie` broker endpoint on 127.0.0.1:18893 (test-only, no auth).
The integration test:

1. Commits two sanitized events through TurboDB.
2. Kills the publisher **after receiving PUBACK but before Outbox ACK**.
   Confirms the broker accepted a publication and the DB row is pending.
3. Restarts Publisher and observes a duplicate publication, then drains
   the second event in order and confirms no messages when Outbox is empty.
4. Stops Broker, commits another event, verifies Publisher fails to CONNECT
   and DB Outbox is unchanged; restarts Broker, retries and ACKs successfully.
5. Runs strict negative payload tests that reject private cards, ticket
   fields, unknown event types, incorrect sequence/epoch and extra keys.

The broker used by CI intentionally has no service ACL so it can run without
external secrets. **Subscription authorization, real service accounts,
QoS1 negative reasons, multi-publisher leadership and Flowie Server ACL are
not yet separately qualified.** These remain release blockers.

## Product roadmap

Deliver a subscriber-consumer API with explicit principal/room-level
authorization and durable sequence deduplication, then bind it to room-specific
projections. Do not let MQTT callbacks mutate authoritative Room/Deal state.
Add broker ACL/TLS and subscriber authorization verification before any
non-loopback launch. An operational publisher supervisor and idempotent
consumer offsets are required for long-running production service.
