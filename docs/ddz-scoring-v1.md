# M5g — Authoritative Dou Dizhu zero-sum score contract (C11 v1)

This is the **first real game-point settlement** on top of the accepted
M5f three-client full match, server-authoritative CFlow Game Owner and
on-disk TurboDB WAL/FULL. It is intentionally a **versioned local
house rule** for three-player 斗地主, not a universal tournament,
real-money, betting, matchmaking or payout service.

## Exact fixed scoring rules

A landlord selected by the finished three-player bidding phase has a
strictly positive **base** score `1..3`. Each **legal** four-of-kind
Bomb or two-joker Rocket actually accepted by the M5a card classifier
doubles the multiplier. These are cumulative regardless of which seat
played them; an invalid command, PASS, repeated request ID, out-of-turn
client/model proposal or already-committed replay does **not** increment.

- **Landlord spring:** landlord wins and the two farmers collectively
  played ZERO non-PASS moves. Multiply by two.
- **Farmer counter-spring:** either farmer wins and the landlord accepted
  exactly ONE non-PASS move in the whole game. Multiply by two.
- Otherwise there is no spring doubling. One game can only have one
  spring/counter-spring and no post-win actions may affect it.

`multiplier = 2 ^ (bomb_or_rocket_count + spring ? 1 : 0)`.

`unit = bid_base * multiplier`. In a landlord victory,
`delta[landlord]=+2*unit`, each farmer receives `-unit`. In a
farmer-team victory, the landlord receives `-2*unit`, both farmers
receive `+unit`, even if only one farmer actually emptied their hand.
The sum of the three integer seat deltas is **always exactly zero**.
No transfer of money, paid chips, accounts or balances is provided.

At most 54 physical cards can be played. The defensive scoring
calculator caps recorded bombs/rockets at 14 and accepted plays at
landlord 20/farmers 34. The arithmetic is checked within int32
(`bid <= 3`, maximum multiplier `2^15`,
maximum absolute score `196608`).

## Single owner, no client score authority

Only the immutable C11 M5b `mygames_ddz_match_play` path updates the
PRIVATE score counters *after* M5a accepts the exact authentic physical
card mask. Bombs and rockets are recognized by `mygames_ddz_classify`,
not by a field in the WebSocket command. If the CFlow/TurboDB Owner
receives a terminal candidate, it re-executes the same canonical M5b
game transition and requires exact v2 checkpoint parity before COMMIT.

The full privately authenticated game checkpoint moves directly from
unreleased **v1/216-byte** format to **v2/224-byte** canonical
little-endian bytes. Bytes 216..218 carry bomb/rocket count, landlord
non-pass count, and combined farmer non-pass count; bytes 219..223
are reserved and MUST be zero. Its 32-byte header declares version 2
and a 192-byte payload. Older v1 bytes are rejected:
**no fallback, implied migration, dual write, or loss of the new
scoring history**. This is a Draft-only schema change, not a production
upgrade. The physical `mg_game_state` table length constraint matches
the v2 size.

A real TurboDB COMMIT atomically writes the complete private deck,
hands, score counters, room/game generation, winner, original command
receipt and public-only Outbox. Reconnecting with a new Owner epoch
rebases the same canonical v2 private bytes in the epoch-CAS
transaction. No score is issued for an uncommitted terminal candidate.

## Terminal public contract and Cocos UI

Bidding and PLAYING public `game.snapshot` have **no `score`**
field. After an accepted, COMMITTED winning play and only in phase
`finished`, the server appends this public result:

```json
"score": {
  "base": 3, "bombs": 0, "spring": "landlord",
  "multiplier": 2, "unit": 6, "delta": [12, -6, -6]
}
```

These public integers reveal NO unplayed hand, deck or secret kitty.
The Cocos TypeScript parser requires the exact score schema, valid
bounded multipliers, zero-sum deltas and correct landlord/team signs.
It will refuse an unsolicited unscored FINISHED snapshot or score
fields in a PLAYING snapshot, preserving the prior safe UI state.

The Cocos `GameTableView` displays the server-confirmed winner team,
this player's signed point delta, bid, bombs and spring. It does NOT
show a player wallet, ranking ladder, payout, currency or game platform
credit. A rematch has fresh score counters and must not inherit
the prior game's multiplier or winner.

## Qualification and remaining scope

- Three-platform host C11 score calculation and actual legal Bomb and
  Rocket moves; invalid score/counter output immutability.
- Strict v2 checkpoint roundtrip at bidding/playing and after legal
  move; reject v1/corrupt/rechecksummed forged counters/reserved bytes.
- Real native CHttp/CFlow/TurboDB Game Owner, 20 authentic single-card
  landlord leads, two farmer PASS actions between leads, physical
  SQLite reopen/replay, and the exact terminal spring score on
  both cold-restored terminal snapshots.
- Linux/Windows/macOS Cocos TypeScript public schema and real privacy
  tests, with negative samples for wrong team, nonzero sum and illegally
  changing a persisted score. Parent M5a–M5f tests stay required.

Not yet part of this work: production public WSS/IAM, Cocos Creator
Editor/Web screenshot acceptance, timeouts/surrender/forfeit scoring,
tournament house-rule variants, rank progression across multiple games,
money or billing, external Flowie event allowlist, distributed fencing
and physical power-loss correctness. All stacked PRs stay Draft.
