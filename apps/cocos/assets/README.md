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


## Real Creator Editor 3.8.8 / Web Desktop gate (M5i)

Source-only `npx tsc`, mocked Creator declarations, `Lobby.scene` JSON
structure tests, and physical CHttp/CFlow/TurboDB three-player tests **do not**
prove that Cocos Creator imported the project, resolved all asset UUIDs,
rendered a graphical game table or produced a real Web Desktop bundle.

For that separate gate, install **Cocos Creator 3.8.8** using the official
Cocos Dashboard on an interactive Windows/macOS desktop. Review its installed
version in the Dashboard or Editor About dialog. Then from the repository root:

- Windows: `python scripts/ci/verify_cocos_creator_web.py --creator "C:\\path\\to\\CocosCreator.exe"`
- macOS: `python3 scripts/ci/verify_cocos_creator_web.py --creator "/Applications/CocosCreator/Creator/3.8.8/CocosCreator.app/Contents/MacOS/CocosCreator"`
  (adjust both paths to the **actual installed** executable).

The script uses Cocos Creator's **real** `--project apps/cocos --build`
CLI, pins `buildPath=project://build` independently of prior Editor
Build-panel settings, and explicitly sets `startScene` to the verified UUID in the tracked
`Lobby.scene.meta` (`74bddbba-b612-4236-b8ed-d10f871e0d49`), and uses
a **unique** `outputName=mygames-m5i-web-desktop-<16-hex>` folder
per invocation, without deleting or reusing previous output.
It requires documented success exit **36** and nonempty
`build/<outputName>/index.html` **and**
`build/<outputName>/src/application.js`. The report stores
the source HEAD, scene UUID, binary SHA256 fingerprint and hashes of the
built entry files as nonsecret evidence.
It writes only `apps/cocos/build/cocos-editor-m5i-evidence.json` with
nonsecret build metadata and explicitly records that Editor version and
graphical/browser behavior are **not** established by that alone.
It does not download/repackage the Editor, print private hands or tickets,
or silently treat exit 0 or a stale index as successful acceptance.

The separate [M5i workflow](../../../.github/workflows/ddz-m5i-cocos-editor-web.yml)
runs only a **gate-harness self-test** on hosted PR CI. Its real Editor
job is opt-in on a **same-repository** PR: a maintainer applies the
`run-cocos-editor` label. The workflow can then run on a Windows
self-hosted logged-in GUI runner labeled `cocos-creator-3.8.8`
**without merging the Draft branch**. The installed executable path must
be configured by an administrator as repository variable
`MYGAMES_COCOS_CREATOR_388_EXE`, **and** the expected binary hash in
`MYGAMES_COCOS_CREATOR_388_SHA256`. The job passes the exact PR HEAD
and verifies both the Git checkout SHA and the executable hash **before
launching the Editor**. A local operator may also supply
`--expected-head` and `--expected-creator-sha256`; when omitted the
evidence clearly records the missing trusted pins. Git is required to
establish the physical source HEAD. The trusted executable SHA identifies
the binary but the operator must still independently verify the installed
Creator 3.8.8 version through Dashboard/About. No PR submitter or dispatch
input may supply an arbitrary executable path. A `workflow_dispatch`
route is also available once the workflow file is present on the default
branch. On ordinary PR runs, the actual Editor job is deliberately
**skipped**. A skipped job is **not Editor acceptance**.

After a true build, open `Lobby.scene` in the 3.8.8 Editor, check for
MissingScript/asset UUID/material and font issues, then launch the Web Desktop
build against the real loopback gateway using **three separate browser
sessions**. Verify two fully played rounds, a new landlord on the second
round, visible public-only settlement, 17/20-card hand layout, disabled
buttons, no opponent hand exposure, and exact pending-request replay on
authenticated reconnect. Retain sanitized browser, Editor and WebSocket
evidence linked to the immutable HEAD. Never upload bearer credentials,
private cards, shuffle/deck content or SQLite data.

The full real Editor/Web/visual gate remains [Issue #2](https://github.com/qigao/mygames/issues/2)
OPEN until those manual/interactive checks complete. External WSS/IAM and
multi-host fencing remain separate production requirements.
