# M5a — Dou Dizhu authoritative round / move contract v1

**Scope:** a real, pure C11 legality and turn engine. This is the first
**gameplay feature**, not another broker, storage or sanitizer diagnostic.
It does not yet make the CHttp Room Server a publicly playable 斗地主 service.

## Card identity and secrecy

Physical card IDs are **0–53**. For 0–51, `rank = id / 4`:
0=3, 1=4, ..., 10=K, 11=A, 12=2. Cards 52 and 53 are the
small and big jokers (rank 13 and 14). An in-memory `uint64_t` mask
uses bit `id` to represent a physical card; upper ten bits MUST be zero.

A caller supplies a **complete, unique, server-secret shuffled permutation**
and an already authorized landlord seat. The engine validates all 54 distinct
cards, deals 17/17/17 cards round-robin, gives the final three kitty cards to
the landlord, and makes that landlord the opening player with `turn_id=1`.
The engine deliberately does **not** invent a predictable PRNG or accept a
client-controlled shuffle seed. The production gateway must use an
unpredictable, unbiased CSPRNG-backed Fisher–Yates shuffle, after a separately
committed bidding decision. Neither shuffle nor bidding is implemented by M5a.

`mygames_ddz_round` stores all three **PRIVATE hands**, and must never be
dumped to JSON, public `room.snapshot`, Flowie room events, client logs or
other players. `mygames_ddz_action_result` contains only move kind,
the acting/next/winning seat, and the public remaining-card counts. Later
persistent gameplay checkpoints require authenticated and member-scoped
private-state storage rather than reusing the room-only checkpoint codec.

## Recognized patterns

| Type | Shape | Comparable key |
|---|---|---|
| Single / Pair / Triple | 1/2/3 of one rank | Rank |
| Triple + single / pair | 3+1 / 3+2 | Triple rank |
| Straight | ≥5 consecutive single ranks 3..A | Length, highest rank |
| Pair chain | ≥3 consecutive pairs 3..A | Chain length, highest rank |
| Triple chain | ≥2 consecutive triples 3..A | Chain length, highest rank |
| Airplane + singles | ≥2 consecutive triples, 1 wing card each | Chain length, highest triple |
| Airplane + pairs | ≥2 consecutive triples, 1 pair-wing each | Chain length, highest triple |
| Four + 2 singles / pairs | Four of a kind plus 2 singles / 2 different pairs | Four rank |
| Bomb | Exactly four of a kind | Rank |
| Rocket | Both jokers | Absolute top |

In this **explicit v1 house rule**, airplane single-wing ranks may repeat
at most twice (so one pair is valid as two single wings), but cannot contain
a further triple/four or any card from the triple-chain ranks. Airplane pair
wings must have separate ranks, and a single four-of-kind is *not* two
independent pair wings. There is no wild card, straight containing 2/jokers,
or ambiguous alternate pattern accepted as an alias.

An ordinary response must have the exact same kind, count and chain length,
and a strictly higher comparable rank. Any bomb beats a non-bomb,
a higher bomb beats a lower bomb, and the rocket beats everything else.
No card-rearrangement from the caller affects classification, and all
invalid shapes are rejected without changing the output.

## Authoritative turn contract

`mygames_ddz_round_submit(round, seat, expected_turn_id, mask, result)`:

1. Validate the entire round, including nonoverlapping hands, exactly 54
   accounted-for physical cards, last play, seat and winner invariants.
2. Reject old `turn_id`, wrong seat, cards the actor does not own, illegal
   shape, an insufficient response or passing when nobody is leading.
3. A successful play removes only the actor's selected physical cards,
   records a new public comparable move and increments the turn ID.
4. PASS is `mask=0`. Two consecutive responses that PASS clear the trick;
   next turn returns to the previous successful player to lead.
5. The first player who empties their hand wins. Subsequent submissions
   return `MYGAMES_DDZ_FINISHED` without mutating state.
6. Any error leaves **both** the round and action result unchanged. Turn ID
   overflow is rejected rather than wrapped, so stale actions cannot revive.

This operation is **not a durable Game ACK**. The CFlow Room Owner must
stage the whole round and original command outcome, authorize the authenticated
actor/generation, compare room epoch + turn_id, then commit room/game state,
receipt and safe public Outbox event in **one TurboDB transaction** before
swapping live state and responding. A model proposal from KFCore/DouZero must
pass the same authority check. Flowie MQTT acknowledgements are independent.

## Current qualification and remaining M5 gates

The dedicated native CI executes `mygames.ddz_round` 20 times on actual
Linux, Windows and macOS hosts, plus a pure-source Linux ASan/UBSan lane.
Tests cover representative legal shapes, ranking, bombs/rocket, 2/joker
chain rejection, invalid/duplicate deck, no mutation on error, wrong owner,
stale turn, PASS→trick reset, 20-card landlord win, and finished rejection.
These are genuine rules operations, not production game-network simulations.

Still required before enabling a real `game.play` WebSocket path:

- Verified, secret CSPRNG shuffle and authoritative **bidding/landlord**.
- Round/game checkpoint format, transactionally committed turn and
  per-player command outcome; crash/restart, COMMIT_UNKNOWN quarantine.
- Session principal and room-owner epoch fencing, bounded recovery and
  per-player visibility (no other hand or kitty leaks).
- Cocos hand UI and legal move selection; DouZero/KFCore can propose,
  but cannot commit an action independently.
- Gameplay-specific timeout/surrender/settlement and secure WSS authority.
- Confirm house-rule variants with product design before broader play.

M4.5 MyGames #9/Flowie #73 deferred ACK work remains independent; no
production Flowie SDK change, release or public service claim is made here.
