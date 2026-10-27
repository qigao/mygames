# M5h — immutable per-game settlement ledger (Draft)

This change is **business gameplay functionality**, not an SDK TSan
workaround or a wallet/payment system. It stacks on M5g, M5f, the CFlow
Game Owner and TurboDB SQLite WAL/FULL. No released SDK, GmSSL, Flowie,
Cocos scene, or public ABI is changed.

## Problem solved

A canonical private match checkpoint remembers only the *current* game.
After the room owner starts the next CSPRNG shuffle, that previous
checkpoint is overwritten. Even though the previous `game.ack` is
durable, it is not a separately indexed historical zero-sum score
record. An application must never credit a game from an uncommitted
final move, a repeated request ID or a volatile client-rendered score.

## One terminal transaction, one immutable result

`mg_game_results` is an **append-only per-room/per-game** settlement
table with primary key `(room_id,game_id)` and an independently unique
global `finish_seq`. It records three *public numeric player IDs*
in actual seat order, confirmed base/bomb/spring multiplier, landlord
and winner seats, public score unit and each signed seat delta.

No card hand, CSPRNG shuffle, hidden kitty, session token or private
GameStore image is stored or returned by this table. It represents
**nonmonetary game points**, not a permanent account wallet.

The only write site is inside the exact existing
`mygames_room_store_commit_game()` transaction:

1. The CFlow owner staged a legal M5b `PLAYING -> FINISHED` result.
2. TurboDB compares the prior room epoch and global sequence and rejects
   prior committed command IDs or conflicting SHA256 request fingerprints.
3. The store reexecutes the canonical M5b move from the exact private
   predecessor and verifies the candidate v2 bytes are identical.
4. In the **same** transaction as room image, v2 private score counters,
   committed command receipt and public-only Outbox, it inserts precisely
   one `mg_game_results` row with that final `next_seq`.
5. Only a positively confirmed COMMIT returns success to the Game
   Owner, which then may send the original exact Game ACK.

The table primary key and unique sequence are strict failure gates;
there is **no INSERT OR IGNORE**, fallback, double-credit repair or
independent background ledger writer. A failure after inserting the
private match and score, but before the receipt/Outbox completes,
rolls the entire SQLite transaction back, leaving neither score nor ACK.
Unknown COMMIT continues to quarantine admission until trusted reopen.

## Read-only game results and per-player totals

Two **trusted CFlow/RoomStore** functions read the physical ledger:

- `mygames_room_store_game_result(store,room_id,game_id,&result)`:
  returns the exact immutable committed game's `finish_sequence`,
  three seat player IDs, factors and points. It checks the mathematical
  multiplier, winner/team signs, bounds and exact zero sum again.
  A missing game returns NOT_FOUND and never mutates the destination.
- `mygames_room_store_player_game_points(store,room_id,player_id,
  &points,&finished_games)`: a strict SQLite `SUM(CASE...)` from
  committed rows in that room alone, not a mutable counter. A player
  in a different room is not counted; unplayed/rematch games contribute
  nothing. Arithmetic failures are backend failures, not silent wrap.

Neither function is exposed as an unauthenticated HTTP/WS endpoint.
A future authenticated game history UI must independently authorize
the user/room before returning per-player totals. The original game
receipts remain separate and are not changed.

## Exact-head acceptance and boundaries

An additional compiled C11 TurboDB fixture runs a true 20-card
landlord spring game with two farmers PASSing after 19 plays, injects
a real rollback **after** the would-be private final checkpoint but
**before** receipt/Outbox, verifies zero credited results, then retries
the same legal final move and checks precisely one result. Duplicate
original `request_id` returns REPLAY, conflicting digest returns
CONFLICT, no additional score is posted. A newly started, unfinished
rematch is uncredited. The closed/reopened physical SQLite connection
and new Room Owner epoch retain the original one-game score.

The existing real three-client CHttp/CFlow/TurboDB M5f full-match CI
independently opens a **read-only SQLite connection** after each of
three genuine process executions and verifies the ledger contains
exactly the original score `[+12,-6,-6]` at the original
winning Game ACK sequence. A later unfinished rematch remains uncredited;
the first game's points do not multiply after restart or old receipt
replay. No private credential or original shuffled cards are uploaded.

This is Draft, with no real-money gaming/payments, persistent ranked
account service, cross-room/global scoring, scheduled session expiry,
production WSS/IAM, Cocos Editor runtime acceptance, external Flowie
game events, real power-cut claims or cross-host Game Owner leadership.
No pending Draft PR is merged as a consequence of these tests.
