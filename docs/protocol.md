# MyGames wire contract v1 (bootstrap)

Transport: a plain development WebSocket endpoint at /ws, only on 127.0.0.1:1780.
This is **not** a production authentication protocol.

## Implemented messages

Server proactively sends on connection:

    {"v":1,"type":"server.hello","capabilities":["ping"]}

Client sends exact development probe:

    {"v":1,"type":"client.ping"}

Server replies:

    {"v":1,"type":"server.pong"}

Other text messages receive:

    {"v":1,"type":"server.error","code":"unsupported_command"}

These are the only supported wire messages in this PR. Exact ping matching is deliberately a smoke probe, **not a JSON command decoder**. Binary messages and room commands are unsupported. Do not use untrusted input to control game state.

## Reserved business envelope (future)

All future commands must be schema-checked and authorized:

- v: protocol version
- type: command or event name
- command_id: idempotence key for a player command
- room_id, round_id: stable logical identities
- room_epoch: owner generation fence
- turn_id: active turn fence
- seq: server-authoritative monotonically increasing event sequence

The authoritative room actor rejects stale epoch, wrong player/turn, duplicates and illegal cards. Confirmed game events are emitted only **after** commit. Flowie PUBACK never stands in for Game ACK.

Future room snapshots must be private per authenticated player; live incremental streams must resync upon seq gaps. No implicit replay of commands following reconnect.
