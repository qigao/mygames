# MyGames

An open, server-authoritative multiplayer game platform, beginning with Dou Dizhu (斗地主).

## First bootstrap

- **C11 room core:** fixed three-seat rooms, join/leave/ready, owner handover, start admission, monotonically increasing revision, and lifecycle regression tests.
- **CHttp/CNet development gateway (opt-in):** GET /health + WebSocket /ws greeting/ping, bound to loopback only.
- **Cocos Creator 3.8.8 / TypeScript:** project manifest, WebSocket client, versioned frame parsing and a connection UI component.
- **Core CI:** Linux / Windows / macOS, no optional external SDK needed.

**Not yet implemented:** complete Cocos scenes/prefabs, session authentication, full room networking, game rules/dealing, MQTT fanout, persistent state, matchmaking, or KFCore/DouZero integration. This repository must not be deployed as an Internet-facing game service yet.

## Build C11 core (no SDK)

    cmake -S . -B build/core -DBUILD_TESTING=ON
    cmake --build build/core --parallel
    ctest --test-dir build/core --output-on-failure

Or use the bundled Ninja preset:

    cmake --preset core-debug
    cmake --build --preset core-debug
    ctest --preset core-debug

## Build the development gateway (installed CHttp SDK required)

Configure CMake with CHttp (and its transitive Salts/CNet dependencies) discoverable through CMAKE_PREFIX_PATH:

    cmake -S . -B build/gateway -DMYGAMES_ENABLE_GATEWAY=ON -DCMAKE_PREFIX_PATH=/path/to/installed/sdk
    cmake --build build/gateway --target mygames_server

Start the executable. It binds 127.0.0.1:1780, then:

- GET http://127.0.0.1:1780/health
- WS ws://127.0.0.1:1780/ws
- Run node scripts/ws_smoke.mjs with Node.js 22+ while the gateway runs

The gateway only supports a versioned server.hello / client.ping / server.pong handshake, not room commands. It is **unauthenticated, intentionally loopback-only and development-only**. Never publish it to an untrusted network.

## Cocos Creator

Open apps/cocos in **Cocos Creator 3.8.8**. The source scaffold has package.json, assets/scripts, and tsconfig.json. Cocos Editor needs to generate/serialize the scenes and prefabs; follow [the editor setup](docs/editor-setup.md). No game assets were copied from third-party projects.

## Architecture

See [architecture](docs/architecture.md) and [protocol](docs/protocol.md).

- KFCore = model package, backend and DouZero inference (future);
- CHttp/CNet = network ingress; CFlow = room-owner scheduling (future);
- Flowie MQTT = committed game-event distribution, not authoritative game-state writes;
- Game Room = the **single writer** for room and play state.

AGPL-3.0; see LICENSE. Consider dependency and asset licensing before distribution.
