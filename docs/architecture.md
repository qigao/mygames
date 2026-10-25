# MyGames architecture and verification

## Implemented development boundary (PR #1)

- **Authoritative C11 RoomDirectory**: bounded 32-room directory, 3 seats each,
  revisions, room owner/ready/leave invariants.
- **C11 Session Registry**: 64 slots; 256-bit caller-provided CSPRNG bytes;
  30s detached lease, resume-token rotation, active/expired rejection and
  per-session generation fencing. A stale transport never regains room authority.
- **CHttp/CNet gateway**: loopback-only HTTP health, WebSocket room commands,
  member-scoped snapshot send and stable recovery binding; one configured
  connection Owner serializes every game-state mutation.
- **Cocos Creator 3.8.8**: a tracked Lobby scene, runtime UI, TypeScript
  GameSocket with in-memory recovery capability, RoomStore epoch/revision
  protections.
- **Tests**: three OS C11 core CI, Node wire+scene+GameSocket fake-browser
  tests, pinned CHttp installed SDK build and live four-client WS room/recovery
  tests.

**Do not expose the development gateway publicly**: it has no external login
authority or TLS, and the recovery ticket is a bearer secret.

## Runtime ownership now

    CHttp connection Owner (count=1)
      |
      +-- borrowed socket callback views
      +-- SessionRegistry (ephemeral identities, rotating tickets)
      +-- RoomDirectory (single mutable room authority)
      +-- member-scoped WebSocket snapshot commands

No thread-affine object is shared with another mutating owner. Callback
packet buffers are not retained. CHttp's captured WebSocket session command
handle is generation-checked; direct response admission may fail under
backpressure and is not a gameplay rollback.

## Target production architecture

    Cocos browser/app
       -> authenticated CHttp/CNet WebSocket Gateway
       -> bounded CFlow Room Owner mailbox (per-room serialization)
       -> Room/Game aggregate + durable snapshot/journal
       -> transactional Outbox -> Flowie MQTT Pub/Sub
       -> spectator, replay and metrics consumers

    DouZero/KFCore inference workers
       -> immutable observed InfoSet -> best action
       -> Room Owner validates current round/turn/owner generation

Flowie owns MQTT protocol sessions and QoS settlement, not the game's Room
facts. A MQTT PUBACK never equals a committed Game ACK. KFCore's model
execution is off the network owner thread.

## Qualification and remaining gates

| Milestone | Current qualification |
|---|---|
| M0 C11 bounded Room Core | Linux / Windows / macOS CTest passed |
| M1a CHttp rooms + Cocos structural scene | real CHttp WS rooms + static scene checks passed |
| M1a Cocos Editor/Web preview | **open Issue #2; not yet qualified** |
| M1b1 in-process reconnect | rotating CSPRNG tickets, close/resume/replay tested |
| M1b2 login and continuous terminal supervision | **not implemented** |
| M1b3 independent CFlow owner + durable snapshots | **not implemented** |
| M2 Dou Dizhu game rule/deal/bid/play | **not implemented** |
| M3 Flowie committed-event fanout | **not implemented** |
| M4 KFCore/DouZero baseline parity | **not implemented** |

Important limitations: the session registry only performs lazy expiry on
ingress and does not store data across process restarts. A crashed TCP
connection that never delivers a WebSocket CLOSE callback can remain marked
attached until a future terminal/timeout solution is implemented. Cross-node
room_epoch/fencing and state restoration are separately required.
