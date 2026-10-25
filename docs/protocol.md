# MyGames wire contract v1 (development room service)

The M1 **development-only** gateway listens on 127.0.0.1:1780.
There is no authentication, session resumption, persistence, ACL, or Internet access.
Player identity is a new ephemeral numeric string assigned at WebSocket open.

## Protocol

On WebSocket connection:

    {"v":1,"type":"server.hello","capabilities":["ping","rooms"],"player_id":"1"}

Client heartbeat probe:

    {"v":1,"type":"client.ping"}
    {"v":1,"type":"server.pong"}

Room commands (request_id is 1..48 ASCII [A-Za-z0-9_.:-] characters):

    {"v":1,"type":"room.create","request_id":"req-1"}
    {"v":1,"type":"room.join","request_id":"req-2","room_id":"room-1"}
    {"v":1,"type":"room.ready","request_id":"req-3","ready":true}
    {"v":1,"type":"room.ready","request_id":"req-4","ready":false}
    {"v":1,"type":"room.leave","request_id":"req-5"}
    {"v":1,"type":"room.snapshot.get","request_id":"req-6"}

Room responses (seat=3 on room.leave success):

    {"v":1,"type":"room.ack","request_id":"req-1","room_id":"room-1","revision":1,"seat":0}
    {"v":1,"type":"room.error","request_id":"req-2","code":"full"}

Room snapshots are sent to **current members only**, after each successful state change;
no hidden hand cards exist yet. Snapshot is fixed to three seats:

    {"v":1,"type":"room.snapshot","room_id":"room-1","room_epoch":1,"revision":2,"phase":"waiting","owner_id":"1","seats":[{"player_id":"1","ready":false},{"player_id":"2","ready":false},null]}

Request errors, malformed frames and unknown commands:

    {"v":1,"type":"server.error","code":"invalid_command"}

## Admission and data limits

- Only one Room Owner thread is configured in the development CHttp server.
- At most 32 simultaneous peers, 32 rooms, 3 players per room.
- Each player can be in only one room.
- Command bytes are at most 512; only a strict flat JSON subset is supported.
- JSON keys must be unique; extra fields, escaped/non-ASCII strings and nested JSON are rejected.
- Room ids are server-issued ASCII strings and not cryptographically random credentials.
- Same connection caches 8 recent request_id/byte-identical reply records. Same ID with a different payload is rejected.
- Repeat join/ready/create operations are idempotent at the authoritative room model.
- Request/reply send admission may fail; clients can query a fresh snapshot. Backpressure is not a game rollback.

Current reconnect creates a **new** player identity. It cannot reclaim a prior seat.
A graceful WebSocket Close removes a waiting player; crash/abrupt disconnect cleanup
requires separate real-network qualification. Stale identities, room epochs and
authenticated reconnect are still missing.

## Next production contract

Replace ephemeral player identities with authenticated sessions; add generation-fenced
room ownership and event sequence numbers, close-on-expiry, durable room snapshots
and command ID idempotency across reconnect and process restart. Add commit-based
Outbox publishing before integrating Flowie MQTT. MQTT PUBACK != game ACK.
