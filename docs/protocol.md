# MyGames wire contract v1 — development room and resumable leases

Transport is a **loopback-only, unauthenticated** CHttp WebSocket at
`ws://127.0.0.1:1780/ws`. This is not a production user login service.
It provides an **ephemeral player identity and a recovery capability** within
one process lifetime, not an independently verified account identity.

## New WebSocket session

On open, the server generates 256 bits with the Salts OS CSPRNG and sends:

    {"v":1,"type":"server.hello","capabilities":["ping","rooms","resume"],"player_id":"1","resume_token":"<64 lower-case hex digits>"}

The 64-digit token is a **bearer secret**. Never place it in URLs, logs,
a room event, retained MQTT topic or public room snapshots. The Cocos client
keeps it **in memory only**, not localStorage/sessionStorage.

## Recovery after a graceful CLOSE

Disconnecting retains a seated player's identity, seat and last 8 command
replies for a **30-second monotonic grace**. On the next WS connection, the
server sends a *new, provisional* `server.hello` (a fresh player and ticket).
The client then immediately sends the *old* ticket:

    {"v":1,"type":"session.resume","resume_token":"<prior 64-hex ticket>"}

The server atomically binds the player to the new WS generation, invalidates
the old ticket and publishes a rotated credential:

    {"v":1,"type":"session.ready","player_id":"1","generation":2,"resume_token":"<new 64-hex ticket>","resumed":true}

If the recovered player remains in a room, a current `room.snapshot`
follows. Successful recovery **cannot silently merge identities**: the fresh
provisional session is destroyed, and old connections cannot mutate the new
generation.

Invalid, expired, already-attached or malformed tickets are not accepted.
The first three cases reply with the **same** opaque error, avoiding an identity
oracle:

    {"v":1,"type":"session.error","code":"resume_rejected"}

A rejected resume retains the **new provisional session** only; clients must
discard the old room view. This version does not attempt automatic retries
or takeover from an already active connection.

## Room commands

All room commands have `v=1` and a 1..48-character ASCII `request_id`.

    {"v":1,"type":"client.ping"}
    {"v":1,"type":"room.create","request_id":"req-1"}
    {"v":1,"type":"room.join","request_id":"req-2","room_id":"room-1"}
    {"v":1,"type":"room.ready","request_id":"req-3","ready":true}
    {"v":1,"type":"room.ready","request_id":"req-4","ready":false}
    {"v":1,"type":"room.leave","request_id":"req-5"}
    {"v":1,"type":"room.snapshot.get","request_id":"req-6"}

Room ACK, error and member-scoped snapshot:

    {"v":1,"type":"room.ack","request_id":"req-1","room_id":"room-1","revision":1,"seat":0}
    {"v":1,"type":"room.error","request_id":"req-2","code":"full"}
    {"v":1,"type":"room.snapshot","room_id":"room-1","room_epoch":1,"revision":2,"phase":"waiting","owner_id":"1","seats":[{"player_id":"1","ready":false},{"player_id":"2","ready":false},null]}

The game room remains the authoritative state writer; ACKs are not MQTT
PUBACKs. A repeated identical request within the last eight per-session
commands yields the previous reply, even across a successful connection
recovery. Conflicting reuse of the same request_id is rejected.

## Bounds and limitations

- Maximum: 32 peers, 64 session records and 32 rooms with three seats each.
  An independent single CFlow SerialExecutor worker owns all room mutations.
- Command bytes <=512. Flat strict JSON subset only; escaped strings, duplicate
  keys, unknown fields and nested data are rejected.
- A resumed session is **not a user authentication mechanism**.
- Session tickets and per-session idempotence records are **in-memory only**.
  Server restart or crash loses all seats and recovery credentials.
- Expiration is lazy in memory mode. With TurboDB enabled, seated-player
  expiry is held until a transactional system-command implementation exists;
  there is no autonomous timer and the fixed session table may fill.
- Graceful WebSocket CLOSE recovery is regression-tested. Abrupt transport loss
  without a CLOSE callback requires a separate terminal-owner cleanup path.
- In this prototype rooms cannot yet transition into a full playable game;
  there are no private cards or persistent player profiles.

Production will require an external identity authority, authenticated WSS,
durable seat/session snapshots, generation fencing across process restart,
CFlow room-owner execution, autonomous lease deadlines, an outbox and Flowie
post-commit MQTT publication.

## Optional developer-only durable transactions

With `MYGAMES_ENABLE_TURBODB=ON` at build time and **both** environment
variables `MYGAMES_SQLITE_PLUGIN` and `MYGAMES_DB_PATH`, accepted room
mutations must commit Checkpoint, exact ACK bytes and an unpublished sanitized
Outbox event before ACK is sent. Room snapshots expose the claimed durable
owner epoch. Replays use per-player request ID + normalized SHA-256 command
digest. Invalid commands and read-only snapshots do not enter the journal.
A restart with leftover seats but no persisted identities fails closed.
