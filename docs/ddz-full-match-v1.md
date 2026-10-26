# M5f — real full Dou Dizhu game, terminal result and rematch (Draft)

M5f completes the previously qualified M5a–M5e game path without changing
published Salts/CHttp/TurboDB/GmSSL packages, app public ABI, Flowie MQTT
allowlist or any production infrastructure. The entire flow remains
**localhost-only, development-identity WebSocket**, not Internet-ready WSS.
The authoritative game and command journal are in a physical TurboDB
SQLite WAL/FULL database, driven by one CFlow Room Owner.

## End-to-end real gameplay

A full match starts with three actual WebSocket clients, a persisted room
with three ready members, a committed room.start, a server-generated game
ID and OS-CSPRNG-shuffled 17/17/17+3-card deal. The room owner bids three,
so the same seat becomes landlord with 20 private physical cards.

Every physical landlord card is played as a valid single, with two real
farmers replying PASS while the trick is open. The last twentieth card
must immediately end the authoritative round. The full game uses roughly
60 **independent real TurboDB COMMITs**, not a mocked final state or
synthetic internal game callback. Every submitted action has a unique
request ID and exact turn ID. Actual successful Game ACK sequence numbers
advance once per committed action, and the public match becomes:

- `phase=finished`, landlord winner seat, server-owned turn sentinel 3;
- 0 physical cards remaining in the winner's private hand;
- 17 physical cards for each passive farmer (their hands remain PRIVATE);
- no further new play/pass permitted, but the same prior command receipt
  still returns its original byte-equivalent Game ACK without another turn.

## Real rematch and state privacy

The real server process then exits. A **new** process reopens the exact
SQLite file and the CFlow Owner claims a new epoch. Three member-only
rotating bearer identities are reattached, and each receives the same
public winner snapshot without exposing any prior private hand.

The original first Game ACK, first play ACK and winner state remain
authoritative. A new game.start with a **fresh request ID** succeeds only
after completion, mints a strictly newer game ID, uses a fresh secret
server CSPRNG shuffle and re-enters bidding. A rematch sequence exercises
a different player becoming landlord (owner passes, player B calls two,
player C raises to three). The new public kitty is hidden until award,
then player C receives exactly 20 own cards, each other player exactly
17. The old game ID no longer selects a live public/private match.

A second process exit and new epoch takeover preserve the rematch and
its original Game ACK. Legitimate old original command IDs are still
read as receipt-only replay, never re-executed into the new game.

Every rotated bearer capability stays in a temporary mode-0600 file
which is never included in CI artifacts, telemetry, JSON public events,
screenshots or log output. The event fanout includes only public game
phase, seat count, winner, last revealed cards and post-award kitty.

## Cocos game-completion presentation

The dynamic Cocos Creator 3.8.8 GameTableView now explicitly distinguishes
**landlord vs farmer-team victory**, using the server-provided landlord
seat/winner seat. A farmer teammate wins when the other farmer empties
their hand. Local hand selection is disabled in a finished round; the
room owner sees the original Start button become available for a rematch.
It explicitly says **scoring is not yet implemented**, rather than
claiming bid/bomb/spring payout computation.

The GameStore separately refuses stale public snapshots older than a
durably confirmed `game.ack` phase/turn and rejects regression from
FINISHED into PLAYING even after reconnect; an authenticated reconnect
may rehydrate the **same** current turn exactly, while new game IDs
invalidate old private-card selections.

## Required evidence

- Native Linux actual CHttp/CFlow/TurboDB/GmSSL three-client game + winner
  + rematch with two physical service restarts and independent epoch claims.
- Linux, Windows, macOS strict Cocos gameplay protocol and state tests;
  C11 Room/game regression on Linux. The existing pinned M5d
  quick WebSocket positive/negative tests remain separately required.
- Exact source HEAD, no CI-dependency disable or compiled mock broker.
- Explicit Editor/Web import and actual rendered multi-browser UX remain
  an OPEN gate in [Issue #2](https://github.com/qigao/mygames/issues/2).

## Still NOT implemented

Official competitive Dou Dizhu scoring, bid/bomb/rocket/spring multipliers,
user lobby invitations, WSS/Origin/cross-host authentication, concurrency
across room processes, full persistent/private-hand encryption, rate limits,
timeouts/forfeits, Cocos Editor build acceptance, model player decisions,
Flowie production game event schema, deployed exactly-once delivery and
network/hardware power-failure guarantees.

This Draft is **not merged**. Its purpose is verifying and improving a
complete, actual local game lifecycle while keeping the original
transaction/fail-closed privacy constraints.
