# MyGames developer session lease v1 — only hashed tickets at rest

This is a **local development-mode recovery capability**, not login or
production authorization. The server continues to bind to loopback; there
is no WSS, external principal authority, account proof or cross-node
identity-service lease. Never deploy this proof of concept on public networks.

## Secret and identity ownership

A CHttp WebSocket connection receives an OS-CSPRNG 256-bit ticket encoded
as 64 lowercase hexadecimal characters and a server-issued numeric
`player_id`. The client alone keeps the plaintext bearer token in memory.
In TurboDB mode, only `SHA256(ASCII lowercase-hex ticket)` is stored in the
`mg_session_leases.ticket_sha256` SQLite BLOB column (32 bytes). Ticket
entropy provides guessing resistance; SHA-256 here is **not a password hash**
and does not create an authenticated principal.

The database also tracks `generation`, `attached`, and an absolute
`expires_at_ms` UNIX-clock deadline. `mg_identity_alloc.last_issued`
ensures discarded connections' player IDs are never reassigned after a
restart.

## New connection and in-process disconnect

- CFlow Room Owner captures and copies a CHttp session handle.
- Generates CSPRNG bytes and encodes the ticket locally; computes SHA-256.
- TurboDB uses an epoch-fenced transaction to **allocate the next player ID
  and save only its digest**, before sending server.hello.
- Failed HELLO queue admission discards the newly issued provisional lease
  with a separate transaction, leaving player ID allocation monotonic.
- On a graceful CLOSE, the Owner transactionally marks this identity
  detached with a 30-second absolute realtime deadline, then updates its
  in-memory registry. An uncertain/failed DB result halts new mutations.

## Restart and recovery

1. Load/verify the canonical room Checkpoint and command journal.
2. Require that every occupied seat has an accompanying hashed bearer
   lease; otherwise **fail closed**. Never attach a seat to a stranger.
3. Owner Epoch CAS and detachment of previous-process attached leases
   happen **in the same SQLite transaction** with a bounded recovery
   deadline. Older owners cannot issue or rotate sessions after takeover.
4. A new WebSocket receives a provisional identity. Its session.resume
   command presents the *old* plaintext ticket; the Owner hashes it
   and asks TurboDB to verify attached=false and unexpired.
5. In one DB transaction, the provisional credential is deleted and the
   recovered identity's hash/generation are rotated atomically. Only after
   that confirmed commit does CFlow import a restored in-memory session,
   send session.ready and the current room.snapshot with the new epoch.
6. Reusing an old, rotated, attached or expired ticket receives only
   `resume_rejected`, never the claimed player's identity.

This does not recover a client which loses its plaintext ticket, e.g.
browser refresh with memory-only token storage.

## Expiration / terminal qualification

A bounded scan on new connections deletes detached, expired *unseated*
leases transactionally, without reusing numeric IDs. Expired **seated**
players remain in the authoritative room; their eventual removal must be
a committed, idempotent *system room command* with an Outbox event. Such
cleanup is not yet implemented. Also missing: autonomous timer progress
when no connections arrive, abnormal TCP terminal notifications when no
WebSocket CLOSE event is delivered, and reserved capacity for CLOSE
events when the CFlow mailbox is saturated.

The remaining blockers are tracked in Issue #3, while actual Cocos
Creator Editor/Web rendering remains Issue #2.

## Test evidence

C11 core and real TurboDB SQLite plugin tests cover import, issue,
atomic rotation, active/expired/stale-gen rejection, epoch-fenced
process reopen, and bounded expired/unseated deletion.

The Linux installed-SDK CI additionally runs real CHttp/CFlow
four-client room operations, closes the process, starts another against
the same database, verifies successful reattachment to the original
player and seat, then rejects old-ticket replay. All credentials are
kept in an isolated CI temporary file (0600), never printed or
uploaded as a GitHub artifact.

**This evidence does not qualify authenticated user login or public WSS.**
