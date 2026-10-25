# MyGames

A server-authoritative multiplayer game platform, initially for Dou Dizhu (斗地主).

## Current development prototype

- **C11 Room Core:** bounded 32-room directory, three-seat admission, owner handover, ready state, revision tracking, lifecycle tests.
- **C11 wire decoder:** strict bounded JSON subset with versioned room commands and regression tests.
- **CHttp/CNet development gateway:** real WebSocket handlers for room.create / join / ready / leave / snapshot.get. **Loopback-only and unauthenticated.**
- **Cocos Creator 3.8.8:** TypeScript wire, socket client, room presentation store and Editor-bindable buttons.
- **CI:** Linux/Windows/macOS core tests and Node 22 TypeScript protocol tests. SDK gateway is opt-in and separately qualified.

**Do not expose or deploy as a public game service.** The gateway lacks login,
secure reconnect, gameplay, storage, Flowie integration and KFCore/DouZero.

## Core-only tests

    cmake -S . -B build/core -DBUILD_TESTING=ON
    cmake --build build/core --parallel
    ctest --test-dir build/core --output-on-failure

Or use the Ninja core-debug preset included in CMakePresets.json.

## Development gateway (requires installed CHttp SDK)

    cmake -S . -B build/gateway -DMYGAMES_ENABLE_GATEWAY=ON -DCMAKE_PREFIX_PATH=/path/to/installed/sdk
    cmake --build build/gateway --target mygames_server

Start mygames_server on 127.0.0.1:1780. For real network smoke:

    node scripts/ws_smoke.mjs
    node scripts/room_ws_smoke.mjs

Node.js 22+ required. These scripts are **not** run by core CI because they
need a separately installed compatible CHttp/CNet SDK and live server.

## Cocos

Open apps/cocos in Cocos Creator 3.8.8. Set up the development buttons using
[docs/editor-setup.md](docs/editor-setup.md). No third-party game textures or sounds
have been copied.

See [wire protocol](docs/protocol.md) and [architecture](docs/architecture.md).
In later milestones KFCore will run DouZero models and Flowie MQTT will fan out
committed, sanitized room events; MQTT protocol acknowledgement never commits gameplay.

License: AGPL-3.0, see LICENSE.
