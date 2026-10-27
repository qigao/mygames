# M5j — authenticated room-local historical result and standings (Draft)

**Scope:** real player-visible gameplay history, not another immutable
ledger implementation. The existing M5h `mg_game_results` table is the
**only authoritative credit source**. M5j adds bounded C11 query routing,
CFlow seat authorization, canonical WebSocket responses and a small Cocos
room-history view. The player must have a current, generation-bound
developer session AND still occupy a seat in the same STARTING room.
The client may **never** choose room_id, player_id, owner epoch, SQL
database or score. No mutations, hidden SDK proxy, retries or new
independent points store are introduced by a read.

## Strict v1 requests

```json
{"v":1,"type":"game.result.get","request_id":"h-1","game_id":"8"}
{"v":1,"type":"game.standings.get","request_id":"h-2"}
```

Inputs are subject to the existing max-512-byte strict C11 parser.
`game.result.get` requires an exact positive, quoted 64-bit decimal
`game_id`; this may be an earlier *finished* game in the caller's
**current room**. `game.standings.get` has **no** game ID; both reject
duplicate/extra keys or a client-supplied `room_id`/`player_id`.
No API exists to query arbitrary rooms, unknown players, other tenants,
private cards, secrets or wallet balances.

`game.result` returns a previously COMMITTED M5h record or
`game.error` with `result_not_found`. The frame has the original
game ID and terminal `finish_seq` as **lossless strings**,
the same three public player IDs in seat order, original landlord and
winner seats, and the already-validated public `score`:
`base,bombs,spring,multiplier,unit,delta[3]`. It never attempts to
reapply the game or issue a new `game.ack`. There is no result for
a started/bidding/playing match until its actual terminal SQL COMMIT.

`game.standings` is a three-player **room-only** read of the existing
historical M5h totals. It returns current Room Owner epoch and durable
`checkpoint_seq` as lossless positive decimal strings, and exactly
three records in current seat order:
`{"player_id":"101","points":"12","games":"1"}`.
Signed points and game counts are strings to preserve full 64-bit
precision in Cocos/JavaScript. Points are NONMONETARY. With the current
three fixed STARTING seats, every finished game must count once for
each player, all three `games` counts must match, and their signed
point totals sum to zero. SQL row corruption or this cross-check failing
halts admission rather than emitting an unbalanced scoreboard.

## Authorization and recovery

- The existing strict `game.*` handler is only invoked on the unique
  CFlow Room Owner, after `current_identity` verifies the current
  session generation and resolves the player's occupied room. No
  CHttp/CNet callback touches ORM or a Game Owner directly.
- The current room must be STARTING; a fourth connected but unseated
  player receives the existing `room_not_started` error for both
  historical operations and cannot infer another room's results.
- The result reader additionally checks that all historical seat IDs
  match the caller's *current* RoomDirectory seats, and that the result
  ID is no newer than the owner's current game. This is an exact
  room-local access policy, not a general multi-room history API.
- The ORM result read is the existing validated immutable
  `mygames_room_store_game_result` from M5h; the three point queries
  are the existing `mygames_room_store_player_game_points` aggregate.
  Neither reads/writes command receipts, private v2 game checkpoints,
  ticket hashes, Outbox or Flowie. The one serialized CFlow Owner is
  the sole ledger writer during a read.
- Cocos `GameStore` accepts historical frames only for an
  **exact pending request ID**, same authenticated room/player seat,
  and the requested game ID. Room standings also require epoch and
  checkpoint sequence not older than already confirmed Game ACK
  state. Invalid/mismatched/duplicate/forged result frames do not
  overwrite historical state.
- On disconnect/logoff, private hand and room-visible historical state
  are cleared from UI memory; a validated reattachment makes fresh
  read-only queries. Unresolved **business** Game ACKs retain their
  original request IDs independently of these read-only queries.

## Actual verification / remaining limitations

The existing real M5f full-match CI drives TWO physically completed
three-player games through native CHttp WebSocket -> single CFlow Owner
-> SQLite WAL/FULL and reopens the same DB on three real process
restarts. M5j acceptance adds reads before any settlement, after the
first result, during the unfinished rematch, after a different
landlord wins the second game, and after the final restart:

- First result `[+12,-6,-6]` and 1 finished game per seat.
- No premature second result or second credit while rematch is live.
- Second result `[-6,-6,+12]`, accumulated points
  `[+6,-12,+6]`, 2 finished games per seat.
- Both immutable result IDs and finishing sequences survive cold
  restart, Game ACK replay and epoch changes; arbitrary fourth player
  cannot query the first room.
- Node/TypeScript tests reject wrong score signs/zero sum, malformed
  signed decimal, missing/extra keys, mismatched request ID, foreign
  room/seat, stale checkpoint, and allow game ID > 2^53 as a string.

This is **not** account-wide ranking, wallet, leaderboard across rooms,
open tournament discovery, IAM, WSS/Origin/rate control, hardware power
failure proof, Cocos Editor live Web preview or another production SDK
release. None of the stacked Draft PRs is merged or published. Actual
Creator GUI remains a separately gated external prerequisite in
[Issue #2](https://github.com/qigao/mygames/issues/2). SDK TSan
diagnostic work remains explicitly out of scope.
