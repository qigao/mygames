# MyGames trusted-source Outbox reconciliation v1

Status: **CI-qualified local administration/recovery prototype**, NOT a
deployed public backfill API or a substitute for Flowie inbound deferred ACK.

## Why this path exists

Flowie 1.0.4 MQTT Client invokes on_message with a borrowed packet, and
automatically emits an inbound QoS1 PUBACK after the callback returns
SALTS_OK. MyGames correctly avoids blocking that callback on SQLite: it
copies the bounded payload and commits the consumer receipt/effect/cursor
later on the consumer's own thread. Therefore:

    Flowie incoming callback -> wire PUBACK -> [CRASH WINDOW] -> consumer COMMIT

A crash in that window may permanently lose delivery to the application if
the Broker has already retired the message, especially with an
MQTT clean-start transient subscriber session.

The authoritative Room Owner's TurboDB Outbox currently retains **all**
committed canonical event rows by global sequence even after
`delivered=1`. We add a bounded administrative reconciliation path:

    verified source room checkpoint + committed archive seq 1..N
           |
    trusted local source read (including already delivered events)
           |
    strict canonical public event schema + SHA-256
           |
    independent ConsumerProjection:
       receipt(namespace,consumer,seq,digest)
       + contiguous cursor
       + demonstration projection effect
       in ONE SERIALIZABLE TurboDB transaction

The consumed event's source identity is a **trusted configuration value**
(`source_namespace`), not a client-supplied MQTT Topic/JSON field.
A duplicated event receives REPLAY, not a second projection effect.
A missing source row, a gap, a forged digest, a private field, or a
corrupt consumer receipt history stops reconciliation without skipping.

## Current APIs

`mygames_room_store_get_committed_event(store, exact_seq, ...)` retrieves
original bounded bytes for a committed sequence whether the Outbox row
is pending or already marked `delivered=1`. An absent sequence at or
below the durable Checkpoint sequence is **CORRUPT**, not a successful
empty read. It never resets delivered status or sends an MQTT ACK.

A test/local-admin-only executable, built with
`MYGAMES_ENABLE_FLOWIE=ON`, is `mygames_event_reconcile_test`.
For a trusted source SQLite file and a separate consumer SQLite file:

    mygames_event_reconcile_test /abs/turbodb_driver_sqlite.so \
      /abs/source-room.sqlite /abs/consumer-projection.sqlite \
      trusted-source-v1 backend-projection-v1 --catchup 128

`--catchup` starts at the consumer's durable last_seq+1.
`--verify-all` explicitly replays from event #1 to compare immutable
historical receipts; it does **not** apply historical effects again.
The batch size must be 1..128, and an incomplete batch exits nonzero with
the next sequence so a supervisor can explicitly continue. The tool does
not attempt unbounded allocation or hidden background retries.

**Security restriction (M4 first slice):** This tool now opens the source
using `mygames_room_store_open_readonly`, backed by TurboDB SQLite
`open_mode=read_only` / `SQLITE_OPEN_READONLY`. It verifies the existing
checkpoint+journal and refuses writes or missing database creation. The
consumer still has **local trusted filesystem access** to the source,
however: this is not remote principal authentication, a deployed
read-only network endpoint, or an immutable tamper-evident archive.
Only run this CLI in an isolated trusted local/CI environment.
A production backfill service must authenticate and authorize a Consumer
principal, bind it to a stable source namespace and retention policy,
enforce bounded rate/cursor semantics, and serve stable page snapshots.
Track the separate M4 contract at [Issue #5](https://github.com/qigao/mygames/issues/5).

## Test evidence / exact boundaries

In the installed SDK test with a real authenticated Flowie Broker:

1. Valid event #1 is published; Publisher crashes after PUBACK but
   before database Outbox ACK; Subscriber applies #1 once.
2. Subscriber/Publisher restart; event #1 is redelivered and receives
   REPLAY; event #2 advances Subscriber's durable cursor/effect to 2.
3. Publisher then publishes event #3 and gets QoS1 PUBACK/Outbox ACK;
   a Subscriber actually receives it but deliberately **does not COMMIT**.
   Its process is terminated to simulate the crash window.
4. A separate process audits ConsumerProjection at seq2 while source
   has seq3 marked delivered. The trusted-source reconciler fills #3,
   producing count3; subsequent --verify-all reports historical REPLAY
   without another effect.
5. Independent fault tests use the actual TurboDB ORM to modify the
   source archive: a hidden `resume_token` field is rejected before
   projection, and a different *schema-valid* payload for an already
   committed seq1 triggers CONFLICT with the consumer's receipt. Neither
   tamper case advances the consumer cursor.

This proves recovery from the simulated early-ACK gap **when a trusted
source archive remains available**. It does not prove every possible
subscriber crash is detected or that automatic redelivery happens.
The current example effect remains a counter, not an actual scoreboard.
Outbox source tables lack an append-only cryptographic integrity
guarantee against a database administrator who can change all records.

## Production gates — intentionally still open

- Upstream [Flowie #22](https://github.com/qigao/flowie/issues/22):
  bounded, nonblocking deferred inbound QoS1 settlement, or a provably
  durable Broker session/reconnection contract. The MyGames test does not
  replace this with worker-thread blocking or a fake ACK.
- A supervised authorized **read-only backfill service** (not merely the new local read-only source connection), with isolated
  identity, namespace pinning, TLS/ACL, archive retention long enough for
  the slowest consumer, paging and operational lag metrics.
- A real consumer projection business effect in the SAME transaction as
  receipts/cursor (the example currently counts committed events).
- Reconciliation concurrency/fencing with live Subscriber deliveries:
  both writers must serialize per consumer stream or resolve durable
  SQLITE_BUSY/retry without discarding messages.
- Real deployed Broker ACL/TLS, MQTT session durability and multiple
  subscriber fanout/backpressure tests.
- Publisher leader fencing and negative PUBACK/COMMIT_UNKNOWN handling.

No public MyGames API or production delivery guarantee is claimed.
