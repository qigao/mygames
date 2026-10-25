# MyGames architecture (bootstrap and planned milestones)

## Ownership

The room state is a bounded POD C11 model. Today only the in-process room library and lifecycle unit tests exist; no network handler mutates it yet.

Planned data plane:

    Cocos Creator / WebSocket client
                |
           CHttp/CNet gateway
                |
     authenticated command dispatch
                |
       Room Owner (CFlow actor)
          |           |
    game transaction   +--> KFCore/DouZero inference (async)
          |
    Game Store commit + event outbox
          |
       Flowie MQTT PUBLISH
          |
    gateway, bot and replay subscribers

No direct writes to authoritative room state by MQTT callbacks, UI code, or inference contexts.

The first version intentionally isolates three responsibilities:
- Room commands: ordered by one logical owner, with room_epoch + turn_id + command_id fencing.
- MQTT: at-least-once delivery where configured, ordered business events by room sequence; MQTT ACK is **not** a gameplay commit.
- KFCore: immutable model weights, worker-exclusive ExecutionContext, stale AI decisions rejected by the room actor.

## Phases

| Phase | Deliverable | Evidence |
|---|---|---|
| M0 (this PR) | Room POD/state invariants, CHttp dev handshake, Cocos TS scripts | Core CTest matrix; manual gateway + Creator checks pending |
| M1 | HTTP/WS auth, lobby, create/join/ready, room snapshots | 3 clients, concurrency, duplicate and disconnect tests |
| M2 | Deterministic Dou Dizhu deal/bid/play/settle | exhaustive card-type and replay tests |
| M3 | Flowie QoS1 committed-event fanout | outbox retry, duplicates, subscriber recovery |
| M4 | DouZero/KFCore CPU inference | three-position Python-vs-native parity |
| M5 | replay, spectators, matchmaking and multi-node ownership | owner epoch, durable recovery and failure tests |

## Security and lifecycle

- The development gateway is loopback-only and unauthenticated. Do not expose it externally.
- Production gateway must authenticate clients and authorize room membership before commands or private snapshots.
- Never publish player hand cards to a public room topic; use individually authorized private paths.
- Reconnect reads an authoritative room snapshot and resumes strictly sequenced events.
- All callback-scope CHttp/CNet views are borrowed; async processing must copy owned bytes before returning.
- Stop admission, settle inflight work, stop owners and only then destroy handlers/plugins.
