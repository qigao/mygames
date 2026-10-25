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
- **No durable room or session fact source.** The RoomDirectory, recovery
  tokens and recent command results are lost on process restart. A typed,
  versioned checkpoint/journal through an explicit TurboDB adapter is still
  needed. Do not introduce ad-hoc native files.
- The current single CFlow serial worker owns *all* rooms. Future sharding
  must pin exactly one owner per `room_id`, fence `room_epoch`, and migrate
  ownership only at an explicit quiescent boundary.
- A graceful WebSocket CLOSE is handled; abrupt socket failure without a CLOSE
  callback still needs a transport-terminal notification contract.
- CLOSE admission can be rejected under a saturated CFlow queue. Reserved
  close capacity and terminal reconciliation are separate fail-closed gates.
- Expired session leases are collected on subsequent OPEN work only, not by a
  continuously progressing timer. A quiet server can retain expired seats.
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
