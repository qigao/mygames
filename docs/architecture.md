# MyGames architecture / delivery gates

## Implemented today (development only)

- A fixed-size, process-local C11 RoomDirectory, writable only from the development
  CHttp server's explicitly configured **one connection Owner**.
- C11 bounded flat-JSON command validation. Commands cannot invent player IDs;
  the server creates an **ephemeral** per-WebSocket numeric identity.
- CHttp HTTP health, WebSocket room create/join/ready/leave/snapshot and
  bounded room snapshot fanout using generation-fenced server session handles.
- Cocos TypeScript GameSocket, RoomStore, developer ConnectionPanel.
- Core/Node tests without optional external SDK.
- Rooms are **not durable**, have no authenticated reconnect, and do not yet
  start actual gameplay. Only WAITING room member leave is supported.

## Planned production layering

    Cocos Creator clients
              |
    CHttp/CNet authenticated gateway
              |
      CFlow room-owner scheduler
              |
        C11 Room/Game Engine  <---- asynchronous KFCore/DouZero worker
              |
     Game Store commit + Outbox
              |
        Flowie MQTT PUBLISH
              |
        gateways/replay/bots

Room command -> single Owner -> validate -> commit -> Game ACK.
Only post-commit events enter Flowie. MQTT PUBACK != Room/Game ACK.
Flowie maintains MQTT protocol sessions, not authoritative room/game sessions.
Room Owner and network connection Owner may differ in production.

## Milestones and evidence

| Stage | Deliverable | Qualification |
|---|---|---|
| M0 | C11 room model; CHttp WS handshake; Cocos TS source | core CI, TypeScript protocol CI |
| M1a (current Draft) | networked dev room create/join/ready/leave/snapshot | core tests; live CHttp SDK, WS clients and Cocos Editor pending |
| M1b | actual authentication, session binding, reconnect, durable snapshots, CFlow owner | real concurrency/expiry/ABA and recovery |
| M2 | deterministic deal, bid, play, settle, replays | exhaustive rule and randomized replay tests |
| M3 | transactional outbox + Flowie MQTT fanout | duplicates, reorder, backpressure, ACL |
| M4 | KFCore ONNX DouZero bots | Python/ONNX/native three-position parity |
| M5 | matchmaking, spectator, rankings and cluster | owner epoch failover, durable recovery |

## Safety/consistency

- No player can join more than one room at a time.
- Room mutations are single-threaded in the prototype; CFlow is not yet wired.
- CHttp packet views are callback-scoped. The synchronous C11 decoder copies
  command fields and never retains packet buffers.
- Snapshot publish may fail due to capacity or stale session; clients must
  request snapshots; no silent assumption of exactly-once fanout.
- The current 8-entry per-connection duplicate cache is NOT a durable
  request-id log; it is insufficient for authenticated retry/reconnect.
- Do not publish private hand cards to room-wide topics or bypass
  membership-aware serialization.
- Do not interpret a successful QoS ACK as validation or commit of a play.
