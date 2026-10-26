# MyGames

A server-authoritative multiplayer game platform, initially for Dou Dizhu (斗地主).

## Current development prototype

- **C11 Room Core:** bounded 32-room directory, three-seat admission, owner handover, ready state, revision tracking, lifecycle tests.
- **C11 wire decoder:** strict bounded JSON subset with versioned room and session.resume commands and regression tests.
- **Recovery:** OS-CSPRNG-generated 256-bit bearer tickets, 30-second detached grace,
  rotating credentials, per-session idempotence; optional TurboDB mode can
  recover the same seat after a process restart using SHA-256-only stored tickets.
- **CHttp/CNet development gateway:** real WebSocket handlers for room.create / join / ready / leave / snapshot.get. **Loopback-only and unauthenticated.**
- **Cocos Creator 3.8.8:** serialized Lobby.scene (Canvas/Camera/Bootstrap), TypeScript runtime-rendered room/lobby UI, WebSocket and room presentation state.
- **CI:** Linux/Windows/macOS core tests and Node 22 TypeScript protocol tests. SDK gateway is opt-in and separately qualified.

**Do not expose or deploy as a public game service.** The gateway lacks externally verified user login, public WSS admission, gameplay,
Flowie integration and KFCore/DouZero. Hashed bearer-based restart recovery is
available only in the optional local TurboDB development mode.

## Core-only tests

    cmake -S . -B build/core -DBUILD_TESTING=ON
    cmake --build build/core --parallel
    ctest --test-dir build/core --output-on-failure

Or use the Ninja core-debug preset included in CMakePresets.json.

## Development gateway (requires installed CHttp SDK)

    cmake -S . -B build/gateway -DMYGAMES_ENABLE_GATEWAY=ON -DCMAKE_PREFIX_PATH=/path/to/installed/sdk
    cmake --build build/gateway --target mygames_server

Start mygames_server on 127.0.0.1:1780. For real network smoke:

    node scripts/ws_smoke.mjs
    node scripts/room_ws_smoke.mjs

Node.js 22+ required. These scripts are **not** run by core CI because they
need a separately installed compatible CHttp/CNet SDK and live server.

## Cocos

Open apps/cocos in Cocos Creator 3.8.8 and open `assets/scenes/Lobby.scene`.
The LobbyBootstrap component creates the room/lobby UI at runtime; no manual button
placement is required. Follow
[docs/editor-setup.md](docs/editor-setup.md). No third-party game textures or sounds
have been copied. Scene UUID and TypeScript syntax gates run in CI, but Cocos Editor
import and actual Web preview are not yet verified.

See [wire protocol](docs/protocol.md) and [architecture](docs/architecture.md).
In later milestones KFCore will run DouZero models and Flowie MQTT will fan out
committed, sanitized room events; MQTT protocol acknowledgement never commits gameplay.

License: AGPL-3.0, see LICENSE.

**Bearer-token recovery is not user authentication.** In-memory-only mode does
not survive process restart; TurboDB mode can rebind a prior player and room
seat using a valid rotating ticket without storing its plaintext. See
[session lease v1](docs/session-lease-v1.md) and [remaining gates](docs/architecture.md).


## Room Owner threading

The real development gateway delegates all room/session mutations to a single
bounded **Salts::CFlow SerialExecutor worker**, separate from its CHttp/CNet
transport owner. Incoming WS messages are copied into owned CFlow tasks;
generation-fenced async CHttp commands return ACK/snapshots to subscribers.
Queue rejection is explicit and does not mutate game state. Installed SDK CI
now verifies CFlow worker isolation, capacity and cancellation/finalize
contracts. See [architecture](docs/architecture.md).

This is not yet multi-node durable CFlow sharding or authenticated gameplay.

## Recoverable room data contract

A versioned, pure-C11 room checkpoint codec is now available in
`games/doudizhu/include/mygames/room_checkpoint.h`. It validates before
serializing and rejects corrupt or unsupported snapshots without modifying
state. See [checkpoint v1 specification](docs/room-checkpoint-v1.md).
The codec itself is storage-neutral; optional TurboDB provides transactional
room facts, command results and Outbox, but not authenticated session recovery.


## Optional TurboDB transactional store

`games/doudizhu/src/room_store_turbodb.c` provides an opt-in transactional
room store using TurboDB ORM and the explicit SQLite Driver plugin. A single
confirmed transaction persists the post-command Room Checkpoint, durable
per-player command result and an unpublished Outbox event. It supports
owner-epoch CAS fencing, failed-commit quarantine, and validated reload from a
real SQLite database after closing and reopening the ORM Runtime.

Build with `MYGAMES_ENABLE_TURBODB=ON` and a compatible installed TurboDB
SDK. [Transaction contract](docs/room-store-turbodb-v1.md).
The CFlow Room Owner now uses this library when **both** env variables are set:

    cmake -S . -B build/gateway -DMYGAMES_ENABLE_GATEWAY=ON \
      -DMYGAMES_ENABLE_TURBODB=ON -DCMAKE_PREFIX_PATH=/path/to/installed/sdk
    cmake --build build/gateway --target mygames_server

    MYGAMES_SQLITE_PLUGIN=/absolute/path/to/turbodb_driver_sqlite.so \
    MYGAMES_DB_PATH=/absolute/path/to/mygames.sqlite \
      ./build/gateway/mygames_server

When enabled, it stages mutations, atomically commits the Checkpoint, original
command outcome and unpublished Outbox event, then changes the live state
and sends ACK. Failed commits never fall back to memory. Without both env
variables the localhost service runs in its original in-memory mode.

SDK CI executes real WebSocket clients against an on-disk database, closes the
server, restarts on the same datastore, and verifies that the *original*
rotating bearer credential reclaims the original player ID and seat with a
new Owner Epoch. A stale ticket is rejected; unknown clients get separate
provisional identities. The DB retains **only SHA-256 of tickets**, never
their plaintext. The server rejects missing session bindings on occupied seats.

A 125ms producer submits coalesced Ticks to the CFlow Room Owner, so expiry
continues even without new network messages. In durable mode, an expired
WAITING player is removed by a **single TurboDB transaction** covering
Checkpoint, retired hashed lease, an idempotent system-command outcome and an
unpublished Outbox event. Expired unseated leases are also reclaimed without
reusing player IDs.

CHttp CLOSE callbacks use a bounded terminal reserve outside the ordinary
CFlow mailbox; on FULL it remains possible to settle the terminal once the
actor resumes. SDK tests deliberately saturate the normal queue and prove the
old player can recover the same seat. These tests do not prove that CHttp
always emits CLOSE on **abrupt TCP loss**; transport-terminal integration
is a remaining gate. No external login, public WSS, Flowie MQTT publisher
or full gameplay recovery is claimed.
See [durable lease protocol](docs/session-lease-v1.md).

## Flowie MQTT committed Outbox publisher

The optional `mygames_outbox_publisher` executable consumes **only
committed** TurboDB Outbox rows and publishes sanitized room events over the
real `Flowie::Client` MQTT 5 protocol at QoS1. It ACKs an Outbox row
**only after a successful PUBACK**, never just on command admission. If the
process dies after Broker acceptance but before the DB ACK, it resends that
same event after restart: the contract is **at least once**, so subscribers
must deduplicate by durable event sequence.

Build using `MYGAMES_ENABLE_FLOWIE=ON` (also requires TurboDB), and invoke
the executable against an explicitly configured local broker. The development
adapter is **127.0.0.1-only**, and the internal topic
`mygames/internal/v1/room-events` is a service topic, **not a public client
subscription**. CI exercises the released Flowie 1.0.4 Client and actual
Flowie Broker endpoints, PUBACK, broker outage, crashed publisher and replay.
The Publisher rejects extra private fields, recovery tickets and unknown
event schemas before reaching MQTT.

See [Flowie Outbox v1](docs/flowie-outbox-v1.md) and
[Subscriber Projection V1](docs/flowie-consumer-v1.md). Installed-SDK CI now
runs a real authenticated loopback Broker Realm, verifies service-only Topic
ACL (including wildcard denial), and persists subscriber receipt + cursor
+ sample effect in a separate TurboDB file. A duplicate MQTT event delivered
after Publisher/Subscriber restarts is deduplicated without another effect.

**This is not public MQTT deployment or guaranteed durable subscription:**
deployed TLS, long-lived subscriber sessions/ACK-after-DB-commit and full
business projection integration remain open in Issue #4.

## Subscriber gap reconciliation (privileged local test)

A bounded local `mygames_event_reconcile_test` can read committed source
Outbox history **even after delivered=1**, validating exact schema and
sequence before atomically updating the separate ConsumerProjection
receipt/effect/cursor. Real Flowie CI now simulates a subscriber that
receives an event but dies before durable projection; a trusted-source
replay repairs the missing event without duplicate effects. Forged private
fields or a historical SHA-256 receipt conflict reject closed.

[Reconciliation v1](docs/flowie-reconciliation-v1.md). This is NOT a
production authorized read-only backfill service, and Flowie #22 inbound
deferred PUBACK remains a separate upstream prerequisite for full
delivery-settlement semantics.
