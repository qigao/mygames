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

An additional compiled C11 TurboDB fixture runs **two complete physical**
20-card landlord spring games with different landlords. In game one, two
farmers PASS after each of 19 plays. The test injects a real transaction
failure **after** the would-be private final checkpoint but **before**
receipt/Outbox. No result or credit survives that failed COMMIT. The
confirmed retry inserts exactly one immutable result; replaying the same
`request_id` and submitting a conflicting digest cannot credit twice.

The rematch remains uncredited throughout bidding and nonterminal play.
Its different landlord then legally wins after a second genuine sequence
of 20 singles and 38 farmer PASSes. The two terminal results carry
distinct `game_id` and `finish_seq` values, with deltas
`[+12,-6,-6]` and `[-6,+12,-6]`, yielding
`[+6,+6,-12]` per-seat cumulative totals. A physical SQLite connection
close/reopen and new Room Owner epoch claim preserve both historical
rows and original terminal sequences. The immutable HEAD
`8a22afecc1ec231d4e940dd8fec51932c8f273a2` established the
first exact-head green C11 two-game ledger gate.

The separate M5f/M5h **real native CHttp/CFlow/TurboDB three-client**
qualification is extended to **four physical server processes**:

1. Finish a complete landlord spring in seat 0. A separate read-only
   SQLite connection must find exactly one `[+12,-6,-6]` row at its
   durable winning Game ACK sequence.
2. Cold restart, claim a new epoch, replay the original receipts, then
   bid and start a new shuffled game with seat 2 as landlord. Its
   **nonterminal** score must not appear in the ledger.
3. Cold restart again, reauthenticate all three physical WebSocket
   clients, replay old receipts, then finish the second real full-length
   game using 20 distinct seat-2 plays and 38 genuine seat-0/seat-1
   PASS actions. Audit two different immutable results, score deltas
   `[+12,-6,-6]` and `[-6,-6,+12]`, and cumulative points
   `[+6,-12,+6]`.
4. Cold restart a third time. Read-only SQLite audit must still
   find **the same two committed historical rows and original terminal
   sequence numbers**. Original Game ACKs from *both* finished games
   must replay byte-for-byte without any new credit.

The third/fourth-stage real-WebSocket extension is a **new candidate**
until its own exact-HEAD M5f and M5h workflows pass. In contrast, the
prior immutable `8a22afe` native green gate tested two fully finished
games in the C11 fixture but only one fully finished game through
real WebSocket clients. This distinction must not be elided.

The private member resume manifest (0600) is deleted with the temporary
test directory; no private hand, hidden deck, credential, SQLite DB or
private checkpoint is uploaded as a CI artifact.

This remains Draft. There is no real-money gaming/payment ledger,
persistent global ranked account service, production WSS/IAM, Cocos
Editor runtime acceptance, external Flowie game events, real power-cut
claim or cross-host leadership/fencing proof. These tests neither merge
the stacked PRs nor change any deployed SDK or Flowie pins.
