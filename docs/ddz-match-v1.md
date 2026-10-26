# M5b — authoritative Dou Dizhu secure deal and bidding contract

**Status: candidate pure C11 game functionality, stacked on M5a #11.**
This is *not* a deployed game server, authenticated identity service, or
durable TurboDB Game ACK. Production Flowie SDK and MQTT remain unchanged.
[Tracking issue #12](https://github.com/qigao/mygames/issues/12).

## Starting a real match

`mygames_ddz_match_start(match, started_room, epoch, game_id)` is called
only from the **unique authoritative CFlow Room Owner** after its
`mygames_room_start()` has already committed/accepted three distinct
ready seats and transitioned the room to `STARTING`. It validates all
participants, the room's owner seat, `epoch != 0` and `game_id != 0`.
An invalid room/state/entropy leaves the destination completely unchanged.

A **real OS CSPRNG**, never `rand()`, supplies each 32-bit draw:
- Linux: `getrandom()`, handles `EINTR` and partial reads.
- Windows: `BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG)`, linked to bcrypt.
- macOS/BSD: `arc4random_buf()`.
- Other platforms fail closed.

Unbiased Fisher–Yates processes 54 distinct physical card IDs. Each
`[0,bound)` draw rejects the upper incomplete tail of the 2³² sample
space, avoiding modulo bias. A provider failure or 128 consecutive
rejections fails closed without publishing a partial deck. The special
entropy injection is defined **only under MYGAMES_DDZ_MATCH_TESTING**;
release builds expose only OS-generated games. Neither a player nor an
ML model chooses a shuffle seed or physical card permutation.

The server retains a **PRIVATE 54-card secret deck** and three 17-card
hands. The three unassigned bottom cards are PRIVATE during bidding.
Once a landlord wins, the prevalidated M5a round engine deals the three
bottom cards to that seat (20/17/17), and the bottom cards may be revealed
to all players as the public landlord award. All pass requires a **new
match/game_id/shuffle**; no silent hidden redeal in the same game ID.

## Bidding and seat authority

Bidding begins with the room owner seat. The three players each bid once,
in clockwise seat order. A bid is 0 (pass) or a number 1..3 that must
be strictly higher than the current highest positive bid; duplicate or
lower bids fail closed. A call of 3 immediately finishes bidding.
Otherwise the highest bid after three decisions wins; three passes mark
`REDEAL_REQUIRED` without producing a playable round or revealing the
bottom cards.

`mygames_ddz_match_bid()` checks exact persisted `(room_epoch, game_id,
bid_turn_id)` and maps an *already authenticated* player ID to its
seat. It never accepts a seat claimed by an untrusted network payload.
The function receives an immutable current match and writes to a
**different staged candidate** plus an explicit bid result; invalid
input, invalid identity, stale game/epoch/turn or wrong seat never changes
either output.

**Bidding is gameplay state**, not a game action ACK. Its candidate must
be committed along with a command digest/receipt and suitable public
Outbox event before the CFlow Owner swaps live state or sends a Game ACK.

## Playing, public state and member private hands

`mygames_ddz_match_play()` is permitted only after bidding selects a
landlord and creates a validated M5a `mygames_ddz_round`. It validates
`(epoch, game_id, turn_id)`, authenticated bound player and all round
invariants before delegating to the existing M5a `round_submit()`.
The output remains staged. Any attempt to play before bidding, after
winning, with cards belonging to another player or a stale turn is
rejected without mutating source or outputs.

`mygames_ddz_match_public_view()` returns only: epoch/game ID, bidding
status/highest visible bid, next bidder, **revealed** bottom cards only
after the landlord decision, and M5a's *public* current turn/revealed
play/remaining card counts/winner. The entire secret deck and all
unplayed hands are absent from this API.

`mygames_ddz_match_private_hand()` returns **only the one hand** for an
authenticated player ID with matching game and epoch. During bidding
it returns 17 cards; after deciding landlord, 20/17/17; during a game
it reflects cards actually played. A mere numeric ID is **not** a
credential; CHttp/CFlow must separately authenticate a principal and
bind it to the room seat before calling this function. Old epochs and
games, foreign player IDs and void all-pass games fail closed.

Do not publish the private match struct, remaining cards, original deck
or player tickets to Flowie room events or Cocos public snapshots.

## Integration contract (M5b next slice)

CFlow Room Owner must use **stage → TurboDB one transaction → publish
Game ACK**, never mutate on callback admission. The *durable* transaction
must include the private versioned game checkpoint, public room epoch,
original `(game_id, turn_id, player_id, request_id)` digest and exact
command result, and sanitized public Outbox event. If commit is
uncertain, quarantine until authoritative reconciliation; do not
retroactively claim that a Flowie PUBACK settled a game decision.

Gameplay network endpoints, bidding-score policy variants, room epoch
cross-host takeover, persisted private-hand restart and Cocos hand UI are
**not implemented** in this slice. [Issue #12](https://github.com/qigao/mygames/issues/12)
tracks those as release-blocking game functionality; #1/#7/#9/#11 and
the new M5b branch remain Draft/unmerged until integrated review.

## Exact-head validation

New dedicated `M5b Dou Dizhu secure bidding and Game Owner (C11)` CI
builds and runs real OS RNG/bidding/state tests on Linux, Windows, macOS,
repeats the M5b match CTest 20 times per platform, runs all pure C11
room/Session/Recovery/Rules CTests, plus source ASan+UBSan on Linux.

Negative tests include:
- Wrong/no ready room, duplicate player, missing epoch/game, entropy
  failure and injected rejection sampling without leaking partial state.
- Wrong bidder, lower bids, stale bid turn/epoch/game; all PASS/no reveal;
  instant bid3; hidden kitty until landlord selection.
- Foreign private-hand query and stale epoch/game, no destination mutation.
- Genuine OS shuffle permutation; owning player-only plays, out-of-turn,
  stale ML proposals and illegal leading PASS.
- A test-constructed valid 20-card airplane victory and post-win rejection,
  then a sanitized public winner announcement.
