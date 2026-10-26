# Cocos Creator 3.8.8 development lobby

The repository now includes a real serialized scene at
`apps/cocos/assets/scenes/Lobby.scene`, a linked `LobbyBootstrap` component and
the matching scene/script `.meta` files. The bootstrap dynamically renders the
lobby, seats and buttons with Cocos UI components and vector graphics; it does
not depend on external textures or copy any third-party assets.

## First run (Editor qualification)

1. Install **Cocos Creator 3.8.8**, import/open `apps/cocos` and let asset-db process the
   tracked `.meta` files.
2. Open `assets/scenes/Lobby.scene`, verify it contains a Canvas with a Camera and
   `LobbyBootstrap` component. A MissingScript or broken component reference is a
   qualification failure, not an acceptable fallback.
3. Mark Lobby.scene as the start scene in the Editor Build panel if needed.
4. Start the installed SDK development gateway on `127.0.0.1:1780`.
5. Preview Web in a browser on that same machine. The lobby UI builds automatically.
6. In three independent tabs, create a room in tab 1; join using the displayed room
   ID in tabs 2/3; press Ready and watch all seat labels synchronize. Close a waiting
   tab and verify other clients observe seat cleanup.
7. Save the scene from the **same Editor** and examine any metadata migration before
   claiming Web build acceptance.

## M5e UI boundaries

Buttons: Connect, Create Room, Join Room, Ready, Cancel Ready, Refresh, Leave,
**Room Start** (three ready players, owner only), and game-level **Start,
Bid 0/1/2/3, select own cards, Play, Pass, Refresh, Retry original request**.

The existing scene's `LobbyBootstrap` dynamically constructs
`GameTableView`, a 54-physical-card Cocos Graphics/Node/Label/Buttons
implementation. It is shown only in the durable STARTING room, and only
offers actions when the CHttp hello explicitly advertises `games`.
Card shapes and turn selection are still validated by the native
TurboDB-backed CFlow Room Owner, never trusted from UI state.

`GameStore` displays only the current bound session's hand; an
unverified/old/foreign private-hand frame is ignored, and a transient
disconnect clears displayed cards until a verified rotated bearer
resume, public game snapshot and fresh member-only hand. The server
proactively sends a **PUBLIC game.snapshot** after validated resume
so a restarted Creator client can learn a server-minted game ID.
The original command ID is replayed after recovery, never silently
minted again.

This is a development loopback UI with no public IAM, WSS/Origin,
rate limits, verified remote clients, optional gameplay timeout or
commercial Cocos build acceptance. Android/iOS/simulator or remote
browser sessions cannot reach another machine's localhost.

## Automated vs. manual evidence

CI checks the serialized scene graph, `__id__` links, script/scene UUID metadata,
TypeScript syntax and the WebSocket room protocol. It does **not** launch Cocos
Editor or build a production Web/native package. Those are separate pending gates.

## Resume smoke (dev-only)

After joining the waiting room, click **模拟断线 · 保留身份** and then press Connect
again in the **same running Cocos scene instance**. The client retains its
recovery capability in memory, sends `session.resume`, and should restore the
original player ID and room snapshot. Restarting the Cocos page destroys that
in-memory capability and starts a new provisional identity. This workflow is
still pending actual Creator Editor/Web preview qualification (Issue #2).
