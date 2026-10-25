# Cocos Creator 3.8.8 M1 development room UI

Open apps/cocos in Cocos Creator **3.8.8** and let the Editor generate .meta and
temp/tsconfig.cocos.json. The submitted project intentionally does not ship
unverified hand-authored .scene or .prefab files.

## Create the development scene

Create a 2D Scene with Canvas and a child node with ConnectionPanel.ts attached.

Assign:
- statusLabel: connection / error state
- roomLabel: room id, revision, owner and player count
- joinRoomInput: EditBox containing a room id such as room-1
- serverUrl: ws://127.0.0.1:1780/ws (local development only)

Create Buttons and bind corresponding click handlers:
- Connect: onConnectClick
- Ping: onPingClick
- Create room: onCreateRoomClick
- Join room: onJoinRoomClick
- Ready: onReadyClick
- Not Ready: onUnreadyClick
- Leave room: onLeaveClick
- Refresh: onRefreshClick

Preview three browser tabs, one per client session. Create a room in the first,
copy its id into the other two tabs and join, then ready all three. Revisions
should advance monotonically and each tab should show 3/3 players. Closing a
waiting player removes that seat if the CLOSE callback is delivered.

The UI is a developer console, not the final Lobby / Room / Game scenes.
This bootstrap has no drag-to-play, cards, art assets, authentication or
connected player reconnection. Cocos Editor import/scene serialization and
native/web builds remain **unqualified** until run in the matching Editor.
