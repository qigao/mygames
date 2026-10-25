# MyGames Architecture and Qualification

## Implemented M1b2: CFlow Room Owner separation (development-only)

The authoritative C11 `RoomDirectory` and in-memory `SessionRegistry` now
run **exclusively on one independent CFlow SerialExecutor worker**.
CHttp and CNet have separate network owner threads; they do not mutate rooms.

    Cocos Creator 3.8 + TypeScript
                  |
    CHttp / CNet WebSocket connection owner
      - capture generation-fenced session value
      - copy bounded incoming frame (max 1024 bytes)
      - nonblocking CFlow task descriptor admission
                  |
    CFlow SerialExecutor (1 worker, capacity 128)
      - ordered OPEN / MESSAGE / CLOSE
      - authenticate provisional recovery capability (development only)
      - RoomDirectory: create / join / ready / leave / snapshot
      - SessionRegistry: detach / resume / ticket rotation
      - serialized room mutation + per-session request cache
                  |
    CHttp generation-fenced async send commands
      - queue copies of room.ack and member-only room.snapshot
      - return on queue pressure; admission does not imply delivery

CFlow accepts copied descriptors. Every accepted task executes exactly one
`run` or `cancel` callback, then `finalize` releases its owned allocation.
Rejected admissions release owned memory synchronously. No borrowed CHttp
packet buffer or live socket pointer is kept by the Room Owner.

The network callbacks reject a full task queue without blocking on model or
room work. The main shutdown path seals admission, stops the CHttp server,
drains accepted CFlow tasks, then destroys the executor and server, in that
order. It does not free state while accepted work is still outstanding.

### Owner conformance

The installed-SDK Linux CI tests an actual independent CFlow serial worker.
It proves:
- Worker callbacks begin without the caller driving any `run_one` method.
- At most one room mutation is executing at a time.
- Exact FULL admission under a blocked worker (no implicit waiting).
- Accepted tasks run exactly once; `finalize` happens exactly once.
- `DRAIN` and `CANCEL_PENDING` shutdown settle all admitted task descriptors.
- Submissions after shutdown reject with CLOSED.

The CFlow unit test explicitly enables assertions even in Release mode.
The real CHttp SDK four-client network test also runs against this layout,
including reconnection, secret rotation and cross-socket duplicate requests.

## What is not yet production-ready

- **No real user authentication or authorization authority.** Loopback-only
  bearer recovery tickets are not an identity provider. Do not expose the
  dev gateway to the Internet.
- TurboDB mode persists room facts, per-player command outcomes and SHA-256
  digests of recovery tickets; it does **not** persist plaintext tickets or
  externally authenticated login principals. A client must present a valid
  prior bearer ticket to recover its original identity across restarts.
- The current single CFlow serial worker owns *all* rooms. Future sharding
  must pin exactly one owner per `room_id`, fence `room_epoch`, and migrate
  ownership only at an explicit quiescent boundary.
- A graceful WebSocket CLOSE is handled; abrupt socket failure without a CLOSE
  callback still needs a transport-terminal notification contract.
- CLOSE admission can be rejected under a saturated CFlow queue. Reserved
  close capacity and terminal reconciliation are separate fail-closed gates.
- Expiration is still ingress-driven, not scheduled autonomously. Durable
  mode now transactionally deletes expired **unseated** leases in bounded
  sweeps. Seated-player expiry is withheld pending a committed system
  command/Outbox, so orphaned seats can still exhaust bounded resources.
- The TypeScript Lobby.scene has passed serialization-structure and syntax
  tests; Creator Editor import and real Web preview remain unqualified (#2).
- No dealing, bidding, out-card legality, scoring, replay, Flowie MQTT outbox,
  KFCore inference, or multiplayer production login is implemented.

## Intended production flow

    authenticated CHttp/CNet ingress
       -> CFlow per-room owner-affine mailbox
       -> validate user, round/turn and owner epoch
       -> atomic room/game commit + command outcome + outbox
       -> Game ACK and authorized private player snapshots
       -> Flowie MQTT publish of committed sanitized events

A MQTT PUBACK never substitutes for the game action commit. A model decision
from KFCore/DouZero is a proposed action, not an authoritative mutation, and
must be validated by the current Room Owner.

## Evidence

- PR #1: M0 and M1a tests on Linux/Windows/macOS, Cocos wire/scene tests.
- Added CFlow Owner tests: independent worker, queue backpressure and both
  executor shutdown policies in the installed-SDK Linux job.
- Issue #2: Cocos Editor / Web build gate.
- Issue #3: remaining authentication, durable state, terminal/expiry handling,
  multi-owner sharding and reconciliation.

The PR remains Draft until its safety and product qualifications are explicit.

## M1b2: room checkpoint codec (storage adapter pending)

The C11 `mygames_room_checkpoint_encode/decode` boundary now supports a
canonical little-endian, versioned, bounded room-data representation with
explicit caller-provided `room_epoch`, deterministic encoding and fail-closed
decode. The codec is pure in-memory logic, covered by Linux/Windows/macOS
CTest; it does **not** mean any room or session is persisted. An actual
TurboDB transaction and command/event/outbox journal must be implemented
and verified before advertising crash recovery. Details:
[checkpoint v1](room-checkpoint-v1.md).

## Optional committed Room Owner via TurboDB

When built with `MYGAMES_ENABLE_TURBODB=ON`, and exact SQLite plugin/database
paths supplied through `MYGAMES_SQLITE_PLUGIN` and `MYGAMES_DB_PATH`,
the CFlow Room Owner now looks up normalized per-player command outcomes,
stages all room mutations in a separate copy, commits the Checkpoint +
exact response + sanitized unpublished Outbox event, then swaps live state
and sends ACK **only after successful TurboDB COMMIT**. Any ambiguous or
failed commit stops mutation admission. A plain development gateway
without these variables remains in-memory.

Installed-SDK CI runs the *actual* durable WebSocket service with four
clients, then closes it, independently reopens its SQLite datastore to
verify state, command outcome, epoch and Outbox, then restarts and proves a
valid old bearer ticket recovers the same player and seat, rotates the token,
and rejects old-token replay. See
[TurboDB store contract](room-store-turbodb-v1.md).

This implements **development-only bearer-based restart reattachment**, but
not an external identity authority, automatic seated expiry, abnormal
transport-terminal recovery, a Flowie publisher, or completed gameplay. Those remain tracked in Issue #3.

## Persistent bearer session lease v1

See [session lease specification](session-lease-v1.md). In TurboDB mode,
transactional player ID allocation and salted-by-entropy 256-bit random
tickets are enforced by the Room Owner. Only SHA-256 ticket digests are stored.
The owner epoch CAS also detaches old-process leases within an absolute UTC
recovery grace. A reconnecting client must rotate its ticket in the same
TurboDB transaction that removes its provisional session. This is a local
development capability, not externally verified user authentication.
