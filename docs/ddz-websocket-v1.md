# M5d — real CFlow / CHttp authoritative Dou Dizhu WebSocket contract

**Draft, localhost development only.** Based on the M5c real TurboDB
private GameStore, M5b authenticated-seat/round API and M5a C11 legality.
This is NOT external login, public WSS, deployment IAM, hardware
power-loss certification, distributed fencing or a complete Cocos UI.
The released SDK TSan diagnostic is explicitly out of scope.

## Unique authority and post-COMMIT response

The real `mygames_server` continues to bind **127.0.0.1 only**. CHttp
WebSocket callbacks copy each bounded text frame into a CFlow task; they
do not touch RoomDirectory, TurboDB, CSPRNG deal, private hands or the
Game Owner. The unique serial CFlow Room Owner resolves a generation-
bound session to its local development player ID and room seat.

Room creation/join/ready are unchanged. With three distinct ready
players, the room owner issues `room.start`: this transitions
`WAITING -> STARTING` by the **existing RoomStore CHECKPOINT+
receipt+Outbox transaction**, and returns the original `room.ack`
ONLY after that transaction. `game.start` is a separate transaction:
a CFlow-owned fresh `game_id` (derived from the global durable
sequence) is never accepted from the caller. A crash between these
transactions leaves a recoverable STARTING room with no game; it does
NOT manufacture a game ACK.

Each `game.start/bid/play/pass` loads the actual private match and
global durable sequence via `MyGames::DouDizhuGameOwner`, validates
player, bound room, epoch, phase and turn, builds the candidate state,
and calls the existing **single TurboDB WAL/FULL transaction** for
private game checkpoint + Room snapshot + exact per-player
`request_id`/semantic digest/ack bytes + PUBLIC-only Outbox event.
No mutation is published unless the transaction returns confirmed
`MYGAMES_STORE_OK`.

Only after confirmed COMMIT does the CFlow Owner submit the original
`game.ack` text through the generation-fenced CHttp send command.
Successful network queue admission is not proof of remote receipt;
on WebSocket close, retry the same request ID. A normalized semantic
digest excludes **room_epoch**, which is a SQL CAS authority fence,
so the *exact* original game/turn command result can replay after a
legitimate process restart and epoch handoff; game_id and turn_id do
remain in the digest. Server-generated `game.start` excludes minted
game_id from its digest so its previous result also replays.

On any unknown COMMIT the Room Owner halts further game/room
mutation until a trusted separate ORM reopen/reconciliation.
Invalid gameplay reports `game.error`; it never generates `game.ack`
or a fake success sequence.

## Strict input JSON

All commands have `v:1`, a 1–48 character ASCII `request_id`
(`[A-Za-z0-9_.:-]`), **exactly the listed members**, no duplicate
members, escapes, extra fields, hidden cards or nested objects.
Maximum 512 bytes. The C11 decoder is independent of public/SDK
parser services.

```json
{"v":1,"type":"room.start","request_id":"r-start-1"}
{"v":1,"type":"game.start","request_id":"g-start-1"}
{"v":1,"type":"game.bid","request_id":"g-bid-1","game_id":"8","turn_id":"1","bid":3}
{"v":1,"type":"game.play","request_id":"g-play-1","game_id":"8","turn_id":"1","cards_hex":"00000000000001"}
{"v":1,"type":"game.pass","request_id":"g-pass-1","game_id":"8","turn_id":"2"}
{"v":1,"type":"game.snapshot.get","request_id":"g-view-1","game_id":"8"}
{"v":1,"type":"game.hand.get","request_id":"g-hand-1","game_id":"8"}
```

`game_id` and `turn_id` are **quoted canonical decimal uint64**
(not unsafe JavaScript numbers). `cards_hex` is exactly fourteen
lowercase hex characters representing a nonempty 54-bit physical
card mask. `bid` must be an integer 0..3; two-pass rules and
last-move legality are delegated to M5b/M5a. `game.start` cannot
supply any game ID or shuffle seed; supplying `player_id` or
`resume_token` to `game.hand.get` fails closed.

A post-COMMIT `game.ack` contains request ID, room ID, owner epoch,
server-created game ID, phase, next turn and durable global sequence.
`game.snapshot` includes public bidding/round, revealed kitty only
after landlord selection, last public play, and per-seat remaining
card **counts**, never unplayed cards. `game.hand` includes only the
hand belonging to the resolved session principal, as a fixed-width
physical card mask. No other player's hand or CSPRNG shuffle is exposed.

The separate RoomStore game Outbox event is generated internally and
contains only `game.state_committed`, room ID, epoch, game ID, phase,
turn and global sequence. The released Flowie Publisher's
`room-events` allowlist does NOT yet include this new event; public
MQTT fanout for game events is a separate, blocked product gate. No
automatic protocol downgrade is permitted.

## CI and real cold restart

The M5d workflow requires Linux/Windows/macOS full no-SDK C11
regressions and the lossless game JSON decoder. A **real Linux CHttp
installed SDK server** runs three actual WebSocket clients with
TurboDB WAL/FULL and one separate CFlow Owner:

- Three ready players `room.start`, unauthorized start rejection.
- Service-minted game start; kitty hidden during bidding, then
  revealed only when landlord is confirmed.
- `game.hand` member isolation; spoofed extra player identity refused.
- Real play/pass/pass and public final trick reset; stale actor/turn,
  malformed extra fields and replay never add another effect.
- Process close and independent SQLite disk reopen, Room Owner epoch
  CAS claim and authorized old ticket reattachment; private hand and
  public game resume without reading another player's hand.
- Replaying the *original* `game.start` / `game.play` request across
  restart yields exactly the original committed ACK and sequence.
- The secret resume capability is held in a mode-0600 temporary audit
  file and NEVER uploaded as a CI artifact.

**Still open:** durable private hand encryption/provisioned IAM,
federated account login and revocation, non-loopback WSS/Origin/rate
policy, concurrent Room sharding, gameplay timeout/scoring/surrender,
Cocos Web Editor scene verification, KFCore/DouZero proposal
conformance, application-level SIGKILL inside SQL COMMIT and deployed
multi-host lease fencing. The dedicated M5d PR remains Draft with
no production cutover or exactly-once/no-loss claim.
