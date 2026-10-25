# MyGames transactional room store — TurboDB SQLite v1

This is an **optional C11 library**, enabled by
`-DMYGAMES_ENABLE_TURBODB=ON`, separate from the CHttp/CNet server. It links
`Orm::C` and loads an **explicitly named** TurboDB SQLite Driver plugin using
`orm_runtime_load_driver`. There is no driver scan, fallback, private SQLite
implementation, or application-owned native file persistence.

The supplied SQLite database filename is explicit. SQLite's persistence,
journaling and atomic commit are managed by TurboDB and its SQLite plugin.
Opening requires `journal_mode=WAL` and `synchronous=FULL`; a failure is
not silently downgraded.

## Durable schema

The adapter initializes these tables transactionally on a fresh database:

- `mg_room_state`: one canonical row `id=1` containing positive `epoch`,
  monotonically increasing `seq` and the binary [Room Checkpoint v1](room-checkpoint-v1.md).
- `mg_commands`: the primary key is **`(player_id, request_id)`**. Each
  committed command stores the caller-supplied 32-byte SHA-256 semantic command
  digest, exact returned response, epoch and sequence. Multiple players are
  allowed to use their own `req-1` identifiers without collisions.
- `mg_outbox`: one event under each global sequence, initially unpublished.
  Outbox payloads must already be sanitized by the game owner; this module
  does not add MQTT topics, authenticate observers, or reveal private cards.

The schema is **development version 1**. Do not run it against an unknown
older schema: `CREATE TABLE IF NOT EXISTS` does not imply migration.

## Atomic command transaction

The unique Room Owner supplies an already validated post-command room
directory, its expected owner epoch/sequence, the verified player ID and
request ID, normalized request SHA-256, exact response bytes and sanitized
event payload.

One TurboDB transaction does:

1. Read/validate the durable checkpoint and verify epoch and sequence.
2. Look up existing `(player_id,request_id)` outcome.
3. If digest matches, **return REPLAY without writing**; the exact prior
   response can be read from `mygames_room_store_get_outcome`.
4. Otherwise CAS the singleton state row by `(epoch, seq)`, writing the
   new checkpoint and incremented sequence.
5. Insert the durable command outcome and unpublished event together.
6. Return **OK only after TurboDB confirms COMMIT**.

Invalid reused IDs return REQUEST_CONFLICT. Stale epochs/sequences return
explicit rejection. A pre-commit database failure rolls the transaction
back; failure to roll back quarantines the instance. TurboDB
`ORM_STATUS_COMMIT_UNKNOWN` maps to `MYGAMES_STORE_COMMIT_UNKNOWN`
and quarantines the owner. **Never retry an uncertain commit without
independent reconciliation**.

No game ACK must be sent merely because an SQL query or MQTT PUBACK returned
success; a complete database transaction must be confirmed first.

## Restart, owner epoch and event delivery

`mygames_room_store_load` loads and validates the entire persisted
checkpoint. Tests close the TurboDB runtime, reopen an on-disk SQLite
database, and prove room state/command results/outbox remain available.

`mygames_room_store_claim_epoch` atomically increments the epoch with
a compare-and-swap against the old epoch and sequence, re-encoding the
checkpoint in the same transaction. Old owners fail subsequent updates.

`mygames_room_store_next_event` returns the oldest remaining, unpublished
event; `ack_event` marks it delivered in a transaction only when the
external publisher has reached the intended settlement point. Thus crashes
after delivery but before ACK may lead to redelivery. Consumers must
deduplicate by sequence and epoch.

**Flowie MQTT publication is not implemented yet**: the outbox is durable
storage, not a running broker integration. The Outbox ACK is not a
gameplay commit.

## Evidence and mandatory safety limits

- Installed SDK test uses pinned `TurboDB.Native 2.3.6` and its real SQLite
  Driver module, plus the existing Salts/CHttp/GmSSL packages.
- C11 test covers bootstrap, commit, normalized replay, conflicting digest,
  a second player reusing the same request ID, persisted outcome retrieval,
  outbox sequencing and ACK, transaction rollback after an injected failure,
  owner epoch claim, stale owner rejection, and re-open of an on-disk DB.
- SQLite SDK tests run only in the optional Linux installed-SDK CI job;
  Linux/Windows/macOS core-only C11 tests still do not require TurboDB.
- This adapter is not yet wired into the live WebSocket Room Owner. Until
  authoritative commands use the transaction **before** publishing ACK,
  successful local WebSocket commands remain in-memory and non-durable.
- Session tickets, authenticated identity, users, room game rules,
  process-lock/election, outbox MQTT publication, schema migrations,
  crash/power-loss fault simulation and cross-node recovery remain open
  milestones in Issue #3.
- The Cocos scene has only structural/static testing; the Editor/browser
  runtime gate remains Issue #2.

Do not describe this as a complete production multiplayer persistence
system before those remaining boundaries have been qualified.
