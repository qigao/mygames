# M4.4 Consumer Owner v1 — in-process admission contract

**Status:** source qualification on stacked Draft PR #7, not an installed
production API. Parent PR #1 remains Draft. Tracking: [M4.4 #8](https://github.com/qigao/mygames/issues/8),
[M4.3 #6](https://github.com/qigao/mygames/issues/6), [Flowie #22](https://github.com/qigao/flowie/issues/22).

## Ownership, authority and copied input

A single `MyGames::ConsumerOwner` is initialized with one borrowed
`ConsumerProjection` and fixed `(source_namespace, consumer_id)`.
Only the Owner-creating OS thread can call `pump_one()`, which is the
sole method permitted to use TurboDB `snapshot()/apply()`. Its
address-stable thread-affinity guard checks the calling thread.

An independent authenticated MQTT callback may call
`mygames_consumer_owner_admit_mqtt()` with the **exact** internal topic
and a canonical public event. The Owner copies at most 1024 bytes
**before returning from the callback**. It never stores borrowed Flowie
callback pointers and never touches the consumer database from that
callback. A separately verified CHttp mTLS response can call
`admit_history()`, which first strictly decodes/validates the full
versioned 32 KiB max response against the configured namespace/consumer,
then atomically copies at most 16 events into the same mailbox. No
partial page publication is allowed.

The local mailbox has 24 bounded slots. MQTT may use at most 8,
leaving room for one complete 16-event historical page even when eight
future MQTT messages have already arrived. Admission FULL is explicit;
there is no unbounded queue, implicit retry, dropped gap or fallback.

### Sequencing and durability

The Owner loads the real durable consumer cursor before each pump.
It prefers the exact next contiguous global event sequence over
duplicates; when only future sequence numbers are buffered, `GAP`
is returned and the cursor is **never skipped**. A duplicate already
committed event is SHA256-matched against an existing TurboDB receipt
and yields `REPLAY`, with no second projection effect. A mismatched
digest, `COMMIT_UNKNOWN`, backend failure or mismatched cursor/effect
quarantines the Owner. It does not automatically reopen and try again.

Only `mygames_consumer_projection_apply()` authorizes a new
receipt, effect and cursor inside one TurboDB transaction. Queue
admission or HTTP page retrieval never does.

### Cancellation and shutdown

Every accepted slot has a nonzero monotonically increasing generation
ticket. A `cancel()` on a queued ticket retires exactly that slot; an
already `TAKEN` event returns `BUSY` because the commit cannot be
revoked. A reused slot does not let stale cancellation affect its new
occupant. Concurrent `begin_close()` stops further admissions and
drops queued entries. It does **not** free the Owner or interrupt a
transaction already running; the sole Owner must finish its pump and
call `destroy()` only after all Flowie and CHttp callback users have
been stopped/joined. The application retains the consumer ledger until
Owner destruction.

The separate, test-only file-lock `consumer_owner_guard` admits one
cooperating recovery process per privately owned local database
pathname. It is deliberately not a cross-host distributed lease, a
thread mutex or a durable owner epoch. Production cross-host failover
requires transactional generation fencing and explicit externally
supervised authority.

## CI evidence and incomplete gates

- Deterministic genuine TurboDB test covers concurrent producers,
  full 8 MQTT + 16 HTTP-page capacity, GAP before repair, ordering,
  identical receipt replay, conflicting digest quarantine, stale
  generations, clean rollback, close while queued and Creator-thread-only
  pump. The C11/ledger workflow runs the test on Linux with real
  installed TurboDB and preserves the Linux/Windows/macOS page tests.
- The authenticated Flowie Broker test includes an opt-in
  `subscriber-owner` mode. The actual Flowie v1.0.4 callback
  copies one received message into the Owner; the main Owner thread
  admits an overlapping *test-encoded* historical event and confirms
  one durable effect. These are **two qualified admission paths**, not
  yet two real network connections in the same process.
- The separate `ci_flowie_owner_live_mtls.py` qualification
  launches the real Flowie Broker/Publisher and a source-built
  CHttp/CNet mTLS read-only recovery service using one source
  database. Independent CA-verified OpenSSL reads a real page and
  passes the bounded response to the Flowie Subscriber process over
  a **test-only IPC pipe**; its callback and history ingress share the
  same in-process Owner and one durable consumer ledger. This tests
  dual *real* network origin but does not convert the IPC into
  production HTTP client logic.

**Critical Flowie boundary:** the existing Flowie 1.0.4 callback
return code is admission status only; native QoS1 PUBACK may precede
consumer TurboDB COMMIT. This Owner must **never** claim that MQTT
receive is durably settled when `admit_mqtt()` returns OK. Only
Flowie #22's independently tested bounded deferred inbound
disposition could enable that contract. QoS2 FSM and outbound/control
progress must remain owned by Flowie.

Still OPEN: source TSan, cancellation overlapping an actual taken DB
COMMIT, real concurrent CHttp Client worker + Flowie callback,
process/host leader fencing, retention expiry and pruning, IAM
rotation/revocation, unknown DB COMMIT audit after power loss,
Windows/macOS genuine Broker/HTTPS integration, release packaging.
Neither exactly-once end-to-end nor production no-loss is established.
