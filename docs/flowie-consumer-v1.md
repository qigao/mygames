# Flowie subscribed-event projection v1 (local qualification)

This is a C11 example **backend consumer projection**, not the Room Owner
and not a browser MQTT client. It is intentionally an independent TurboDB
SQLite datastore. The authoritative game server still owns RoomDirectory,
round/turn validity and command commits.

## Source identity and exactly what is deduplicated

An event is identified by:

    (trusted_source_namespace, consumer_id, committed_global_seq)

The source namespace is supplied by the trusted backend configuration;
it is **not** inferred from the MQTT Topic, an untrusted JSON field or an
absolute database path. Keep this namespace stable across Flowie Broker
and publisher restarts. Different event sources must use different
namespaces.

The consumer stores only SHA-256 of the **canonical validated event
bytes** in a receipt row. On each independently processed event, one
TurboDB SERIALIZABLE SQLite transaction:

1. Reads the Consumer ID + namespace cursor.
2. If the sequence was already processed, compares the stored SHA-256;
   identical = REPLAY with **no new projection effect**, mismatch = CONFLICT.
3. If the sequence is not exactly last_seq+1, fails with SEQUENCE_GAP.
4. Otherwise inserts the receipt and atomically advances both last_seq
   and the demonstration projection's applied_count, using a cursor CAS.
5. Returns APPLIED only after a positively confirmed SQLite COMMIT;
   a COMMIT_UNKNOWN quarantines the projection connection.

The example projection effect is just a durable per-consumer **event count**.
This proves the same transactional technique a future room analytics
projection must use; it does NOT implement a gameplay scoreboard or room
state projection.

A fault injected **after receipt INSERT but before projection/cursor UPDATE**
rolls the transaction back. Reopening SQLite confirms no partial effect.

## Flowie Client callback ownership

The tested Flowie Subscriber is authenticated as service principal
`svc-consumer`. Its MQTT worker callback:

- verifies the expected internal Topic, message bounds and type;
- **copies** the borrowed MQTT payload into bounded, caller-owned memory;
- signals an atomic completion and returns; it performs **no SQL** and
  cannot access the authoritative CFlow Room Owner.

The test's separate main thread validates the exact public event allowlist,
extracts committed Owner Epoch + sequence, hashes canonical payload bytes,
then opens the independent Consumer Projection SQLite database and commits
receipt/counter/cursor. It never forwards MQTT directly into room mutation.

On a different process run, receipt and cursor remain durable.

## Broker authorization tests

The installed Flowie v1.0.4 test endpoint binds a real
`Flowie::Flowie` Security Realm with default deny. CI generates fresh
per-process credentials (never checked in or printed), and verifies:

| Principal | CONNECT | PUBLISH internal | SUBSCRIBE internal |
|---|---|---|---|
| `svc-outbox` | allowed on exact Publisher client ID | allowed | denied |
| `svc-consumer` | allowed on exact backend client ID | denied | allowed |
| `player-browser` | allowed for negative tests only | denied | denied |
| invalid password | denied | not admitted | not admitted |

Wildcard SUBSCRIBE patterns such as `mygames/#` and
`mygames/internal/v1/#`, and publisher's other Topic names, are also
denied. This is a **loopback test Realm** with passwords from temporary
CI process environment variables. It does **not** provision a deployed
service principal or test Internet WSS/TLS trust.

## Actual duplicate/restart integration

The real CI flow:

1. Creates canonical committed events with global seq 1,2.
2. Starts a secured Flowie Broker and an authenticated backend Subscriber.
3. Publisher reaches PUBACK for event 1, then deliberately dies **before**
   TurboDB Outbox ACK. Subscriber processes seq1 and advances its own
   separate SQLite projection to count1.
4. Restarts Subscriber and Publisher. The same event is published and
   received again. Subscriber returns REPLAY with count still 1.
5. Starts the Subscriber a third time and publishes seq2; it advances
   count and cursor to 2. An independent TurboDB process audits its file.
6. Negative tests reject wrong-role Publisher/SUBSCRIBER operations and
   wildcard attempts before publication reaches the Broker application.

This qualifies **no duplicate side effect for events the application
actually receives and durably processes**. It does not qualify the
stronger statement "no messages can be lost across a Subscriber crash."

## Fail-closed receipt history qualification

On every projection database open, the adapter validates committed
receipt coverage: the number of receipts must equal each cursor's
`last_seq`, the contiguous interval must start at 1 and end at that
sequence, and there may be no orphaned receipt without a parent cursor.
The sample projection count must match the committed cursor.

The C11 test uses the same TurboDB ORM SQLite plugin to delete one
previously committed receipt while leaving its cursor apparently valid.
A separate ORM Runtime then refuses the corrupted database rather than
silently resuming at the next event. This protects a local ledger from
incomplete recovery, but it is not a cryptographic integrity proof against
an attacker with write access to all related tables.

[Installed SDK CI #40220090282](https://github.com/qigao/mygames/actions/runs/40220090282)
passed 5/5 including this corruption refusal, the real Broker ACL
negative matrix, real MQTT duplicate/restart projection, and unchanged
C11/CHttp/Cocos regressions.

## Still required before production

- **Subscriber QoS1 ACK timing:** Flowie Client may acknowledge incoming
  QoS1 before this consumer commits its projection. A crash after network
  acknowledgement but before the database transaction can lose a received
  event if the broker has no durable pending record/session. Current
  `clean_start=1` test clients do not provide durable subscriber sessions.
  A Flowie-supported deferred PUBACK/manual settlement or a verified durable
  Broker session + recovery/reconciliation mechanism is needed.
- **Operational subscriber:** current subscriber is a test client, not a
  daemon with supervised queueing, reconnect and backpressure controls.
- **Real projection:** move each authorized business effect into the SAME
  transaction as the receipt/cursor. Never execute it in a Flowie callback.
- **Security:** deployed Flowie TLS/mTLS, per-service credential rotation,
  Broker ACL persistence and negative tests against actual server deployment
  are unqualified. Never expose the loopback/unauthenticated fixtures.
- **Publishing:** prevent uncontrolled simultaneous publishers with leader
  fencing, and cover negative PUBACK/unknown COMMIT recovery.
- **Full game:** Dou Dizhu rules, private cards, KFCore/DouZero AI and Cocos
  Editor rendering remain separate workstreams.

Tracking: [MyGames Issue #4](https://github.com/qigao/mygames/issues/4).

Upstream deferred incoming settlement is tracked by [Flowie #22](https://github.com/qigao/flowie/issues/22); the current downstream CI does **not** close it.

## Privileged archive catch-up (local CI qualification)

The source's TurboDB Outbox retains committed event bytes after Broker
PUBACK. A bounded test-only `mygames_event_reconcile_test` uses the
**trusted** source sequence to fill a ConsumerProjection gap independently
of MQTT delivery. It never updates RoomDirectory or silently advances past
an unavailable/invalid event. An explicit `--verify-all` pass detects
changed historical payloads by comparing durable SHA-256 receipts.

[Reconciliation v1](flowie-reconciliation-v1.md) documents the simulated
Flowie auto-PUBACK-before-DB-commit crash window, successful recovery of the
uncommitted third event and two corruption/forgery denial cases.

**The source archive is currently accessed using privileged local DB
credentials**. This is not a production read-only, principal-authorized
backfill endpoint; it does not close upstream Flowie #22 or guarantee
network-to-DB exactly-once/no-loss for subscribers without trusted source
access. Production still needs authenticated recovery, retention, offsets,
live-Subscriber coordination and an actual business projection.
