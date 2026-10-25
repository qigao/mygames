# Cocos Creator 3.8.8 development lobby

The repository now includes a real serialized scene at
\`apps/cocos/assets/scenes/Lobby.scene\`, a linked \`LobbyBootstrap\` component and
the matching scene/script \`.meta\` files. The bootstrap dynamically renders the
lobby, seats and buttons with Cocos UI components and vector graphics; it does
not depend on external textures or copy any third-party assets.

## First run (Editor qualification)

1. Install **Cocos Creator 3.8.8**, import/open \`apps/cocos\` and let asset-db process the
   tracked \`.meta\` files.
2. Open \`assets/scenes/Lobby.scene\`, verify it contains a Canvas with a Camera and
   \`LobbyBootstrap\` component. A MissingScript or broken component reference is a
   qualification failure, not an acceptable fallback.
3. Mark Lobby.scene as the start scene in the Editor Build panel if needed.
4. Start the installed SDK development gateway on \`127.0.0.1:1780\`.
5. Preview Web in a browser on that same machine. The lobby UI builds automatically.
6. In three independent tabs, create a room in tab 1; join using the displayed room
   ID in tabs 2/3; press Ready and watch all seat labels synchronize. Close a waiting
   tab and verify other clients observe seat cleanup.
7. Save the scene from the **same Editor** and examine any metadata migration before
   claiming Web build acceptance.

## UI boundaries

Buttons: Connect, Create Room, Join Room, Ready, Cancel Ready, Refresh, Leave.
The UI is a development lobby/room view, not a complete game table. There is no
card rendering, game timer, bidding, authentication or reconnect. \`LobbyBootstrap\`
defaults to the loopback server. Android/iOS/simulator or remote browser sessions
cannot reach another machine's localhost; external network access is intentionally
unsupported until authentication and TLS are added.

## Automated vs. manual evidence

CI checks the serialized scene graph, \`__id__\` links, script/scene UUID metadata,
TypeScript syntax and the WebSocket room protocol. It does **not** launch Cocos
Editor or build a production Web/native package. Those are separate pending gates.
