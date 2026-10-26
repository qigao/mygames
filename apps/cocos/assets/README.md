# Cocos Creator 3.8.8 gameplay assets — M5e (Draft)

The versioned [Lobby.scene](scenes/Lobby.scene) already contains a real
serialized Canvas/Camera/LobbyBootstrap graph with tracked matching
`.meta` IDs. M5e **reuses** that asset; the `LobbyBootstrap` component
dynamically mounts `GameTableView` under its Canvas after a durable
`room.start`. The table uses Cocos UI graphics, Buttons, Labels and Nodes,
not an unverified hand-authored prefab.

The visual table includes three public seats, 17/20 authentic physical
cards belonging only to the bound player, a clickable selected-card row,
landlord bidding (pass / 1 / 2 / 3), play/pass controls, a revealed
post-landlord kitty, current trick/winner and server-authorized remaining
counts. The local UI never adjudicates a move; the native M5a/M5b Owner
will reject stale, out-of-turn and illegal proposals before any TurboDB
COMMIT.

M5e also adds a strict lossless TypeScript `game.*` protocol, private
`GameStore`, bounded in-memory pending requests with **identical request
IDs** on verified resume, session-dependent selective hand hydration and
monotonic epoch/game/turn projection.

Only the **current player's** private `game.hand` is kept in memory.
Session loss immediately hides all physical cards until authenticated
recovery and a new public snapshot + private-hand response. Do NOT
store hands, original secret deck or rotating bearer credentials in
localStorage, logs, Flowie, screenshots or analytics.

Open `apps/cocos` in Cocos Creator **3.8.8**, load
`assets/scenes/Lobby.scene`, and use three independent browser previews
against the **loopback-only** M5d CHttp/CFlow/TurboDB Game Owner. Create,
join, ready, click **三人已准备 · 房主启动**, then the host starts a
separately durable game. Players bid, view their own cards, select and
play, pass, and verify automatic state/hand resynchronization on
simulated disconnect and restored developer ticket.

CI verifies existing scene metadata, dynamic game-table source
bindings, TypeScript syntax, C11/JS protocol parity, state privacy,
bounded retry and real Linux multi-WebSocket game restart with pinned
native SDKs. It does **not** launch Cocos Editor, validate actual
font/layout rendering, build Web/native bundles or approve public WSS.
Those remain explicit next-stage acceptance gates. The development
server advertises `games` capability only with an admitted real
TurboDB-backed Owner; no gameplay fallback exists without durable state.

No external art/fonts or proprietary assets were copied.
