cmake_minimum_required(VERSION 3.25)
project(MyGames VERSION 0.1.0 LANGUAGES C)

option(MYGAMES_ENABLE_GATEWAY "Build optional CHttp/CNet development gateway" OFF)
option(MYGAMES_ENABLE_TURBODB "Build optional TurboDB transactional room storage" OFF)
option(MYGAMES_ENABLE_FLOWIE "Build optional Flowie QoS1 Outbox publisher and local broker test" OFF)

add_library(mygames_room STATIC games/doudizhu/src/room.c
  games/doudizhu/src/room_directory.c
  games/doudizhu/src/wire.c
  games/doudizhu/src/ddz_game_wire.c
  games/doudizhu/src/session_registry.c
  games/doudizhu/src/room_checkpoint.c)
add_library(MyGames::Room ALIAS mygames_room)
target_compile_features(mygames_room PUBLIC c_std_11)
target_include_directories(mygames_room PUBLIC games/doudizhu/include)
if(MSVC)
  target_compile_options(mygames_room PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_room PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# M5a authoritative Dou Dizhu move/rule kernel. Deliberately independent
# of the Room gateway, TurboDB, Flowie and all released SDK dependencies.
# Caller supplies a secret, externally shuffled deck after bidding; this
# component never persists or publishes player hands.
add_library(mygames_ddz_round STATIC games/doudizhu/src/ddz_round.c)
add_library(MyGames::DouDizhuRound ALIAS mygames_ddz_round)
target_compile_features(mygames_ddz_round PUBLIC c_std_11)
target_include_directories(mygames_ddz_round PUBLIC games/doudizhu/include)
if(MSVC)
  target_compile_options(mygames_ddz_round PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_round PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# R0: game-agnostic, explicit per-player Monte Carlo returns.
add_library(mygames_rl_targets STATIC ai/core/src/rl_targets.c)
add_library(MyGames::RLTargets ALIAS mygames_rl_targets)
target_compile_features(mygames_rl_targets PUBLIC c_std_11)
target_include_directories(mygames_rl_targets PUBLIC ai/core/include)
if(UNIX AND NOT APPLE)
  target_link_libraries(mygames_rl_targets PUBLIC m)
endif()
if(MSVC)
  target_compile_options(mygames_rl_targets PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_rl_targets PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# R1 first native slice: rank-equivalent complete actions via existing
# authoritative classifier, not a second rules implementation.
add_library(mygames_ddz_ai_actions STATIC
  games/doudizhu/src/ddz_ai_actions.c)
add_library(MyGames::DouDizhuAI ALIAS mygames_ddz_ai_actions)
target_compile_features(mygames_ddz_ai_actions PUBLIC c_std_11)
target_include_directories(mygames_ddz_ai_actions PUBLIC games/doudizhu/include)
target_link_libraries(mygames_ddz_ai_actions PUBLIC MyGames::DouDizhuRound)
if(MSVC)
  target_compile_options(mygames_ddz_ai_actions PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_ai_actions PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# M5b: real CSPRNG deal + staged bidding and Game Owner validation.
# This match library is private game authority, never a network callback or
# persisted transaction/ACK by itself. Strict C11 and no third-party SDK.
add_library(mygames_ddz_match STATIC games/doudizhu/src/ddz_match.c)
add_library(MyGames::DouDizhuMatch ALIAS mygames_ddz_match)
target_compile_features(mygames_ddz_match PUBLIC c_std_11)
target_include_directories(mygames_ddz_match PUBLIC games/doudizhu/include)
target_link_libraries(mygames_ddz_match PUBLIC MyGames::Room MyGames::DouDizhuRound)
if(WIN32)
  # Native OS CSPRNG only. No rand() or published TLS/provider dependency.
  target_link_libraries(mygames_ddz_match PRIVATE bcrypt)
endif()
if(MSVC)
  target_compile_options(mygames_ddz_match PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_match PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# R1: an AUTHORIZED player's own hand + committed PUBLIC history =>
# DouZero-shape features. Does not accept full private match/hidden deck.
add_library(mygames_ddz_ai_features STATIC
  games/doudizhu/src/ddz_ai_features.c)
add_library(MyGames::DouDizhuAIFeatures ALIAS mygames_ddz_ai_features)
target_compile_features(mygames_ddz_ai_features PUBLIC c_std_11)
target_include_directories(mygames_ddz_ai_features PUBLIC games/doudizhu/include)
target_link_libraries(mygames_ddz_ai_features PUBLIC MyGames::DouDizhuAI)
if(MSVC)
  target_compile_options(mygames_ddz_ai_features PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_ai_features PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# Pure offline DMC sample collector. Not linked into the CHttp Game Owner.
# Labels are sealed only after a real full-match final settlement.
add_library(mygames_ddz_dmc_episode STATIC
  ai/dmc/src/ddz_dmc_episode.c)
add_library(MyGames::DMCCollector ALIAS mygames_ddz_dmc_episode)
target_compile_features(mygames_ddz_dmc_episode PUBLIC c_std_11)
target_include_directories(mygames_ddz_dmc_episode PUBLIC ai/dmc/include)
target_link_libraries(mygames_ddz_dmc_episode PUBLIC
  MyGames::DouDizhuAIFeatures MyGames::DouDizhuMatch)
if(MSVC)
  target_compile_options(mygames_ddz_dmc_episode PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_dmc_episode PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# R1 private, versioned and canonical DMC Replay codec. This target owns
# NO file/network/SQLite handle. It checks every complete terminal label
# before serializing (no struct memcpy or unqualified source fallback).
add_library(mygames_ddz_dmc_replay STATIC
  ai/dmc/src/ddz_dmc_replay.c)
add_library(MyGames::DMCReplay ALIAS mygames_ddz_dmc_replay)
target_compile_features(mygames_ddz_dmc_replay PUBLIC c_std_11)
target_include_directories(mygames_ddz_dmc_replay PUBLIC ai/dmc/include)
target_link_libraries(mygames_ddz_dmc_replay PUBLIC MyGames::DMCCollector)
if(MSVC)
  target_compile_options(mygames_ddz_dmc_replay PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_dmc_replay PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# Offline-only C11 Self-Play. The policy callback sees ONLY its own
# authorized Observation + full legal mask list; no CFlow or SQLite owner.
add_library(mygames_ddz_dmc_selfplay STATIC
  ai/dmc/src/ddz_dmc_selfplay.c)
add_library(MyGames::DMCSelfPlay ALIAS mygames_ddz_dmc_selfplay)
target_compile_features(mygames_ddz_dmc_selfplay PUBLIC c_std_11)
target_include_directories(mygames_ddz_dmc_selfplay PUBLIC ai/dmc/include)
target_link_libraries(mygames_ddz_dmc_selfplay PUBLIC MyGames::DMCReplay)
if(MSVC)
  target_compile_options(mygames_ddz_dmc_selfplay PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_dmc_selfplay PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()


# Linux-only opt-in local corpus emitter: existing private 0700 directory,
# no-clobber O_EXCL/0600 files, fsync, no IO in the C11 algorithms.
# NOT an installer, a downstream consumer project, or a network service.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(mygames_ddz_dmc_generate ai/dmc/src/ddz_dmc_generate.c)
  target_compile_features(mygames_ddz_dmc_generate PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_dmc_generate PRIVATE MyGames::DMCSelfPlay)
  target_compile_options(mygames_ddz_dmc_generate PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()


# Offline model-to-rules inference boundary: version-fenced batched Q score
# provider, canonical COMPLETE legal mask list, bounded 64-action batches.
# No torch/ONNX/KFCore dependency, second DDZ engine or fallback scorer.
add_library(mygames_ddz_dmc_q_selector STATIC
  ai/dmc/src/ddz_dmc_q_selector.c)
add_library(MyGames::DMCQSelector ALIAS mygames_ddz_dmc_q_selector)
target_compile_features(mygames_ddz_dmc_q_selector PUBLIC c_std_11)
target_include_directories(mygames_ddz_dmc_q_selector PUBLIC ai/dmc/include)
target_link_libraries(mygames_ddz_dmc_q_selector PUBLIC MyGames::DMCSelfPlay)
if(MSVC)
  target_compile_options(mygames_ddz_dmc_q_selector PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_dmc_q_selector PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# Numerical conformance ONLY: pure C11 float32 LSTM + Q-reference CPU
# backend for frozen, role-specific PyTorch weight images. It is deliberately
# slower than KFCore and is NOT silently installed as inference fallback.
add_library(mygames_ddz_dmc_q_cpu STATIC
  ai/dmc/src/ddz_dmc_q_cpu.c
  ai/dmc/src/ddz_dmc_q_cpu_adapter.c)
add_library(MyGames::DMCQCPUReference ALIAS mygames_ddz_dmc_q_cpu)
target_compile_features(mygames_ddz_dmc_q_cpu PUBLIC c_std_11)
target_include_directories(mygames_ddz_dmc_q_cpu PUBLIC ai/dmc/include)
target_link_libraries(mygames_ddz_dmc_q_cpu PUBLIC MyGames::DMCQSelector)
if(UNIX AND NOT APPLE)
  target_link_libraries(mygames_ddz_dmc_q_cpu PUBLIC m)
endif()
if(MSVC)
  target_compile_options(mygames_ddz_dmc_q_cpu PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_dmc_q_cpu PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

# M5c: portable, fixed-size canonical PRIVATE match state codec. No native
# file persistence and no public game-state serialization API is provided.
add_library(mygames_ddz_checkpoint STATIC
  games/doudizhu/src/ddz_match_checkpoint.c)
add_library(MyGames::DouDizhuCheckpoint ALIAS mygames_ddz_checkpoint)
target_compile_features(mygames_ddz_checkpoint PUBLIC c_std_11)
target_include_directories(mygames_ddz_checkpoint PUBLIC games/doudizhu/include)
target_link_libraries(mygames_ddz_checkpoint PUBLIC MyGames::DouDizhuMatch)
if(MSVC)
  target_compile_options(mygames_ddz_checkpoint PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_ddz_checkpoint PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# The recovery admission core is pure C11; no network, database or
# fallback implementation is linked into this policy target.
add_library(mygames_recovery_gate STATIC
  games/doudizhu/src/recovery_gate.c)
add_library(MyGames::RecoveryGate ALIAS mygames_recovery_gate)
add_library(mygames_recovery_wire STATIC
  games/doudizhu/src/recovery_wire.c)
add_library(MyGames::RecoveryWire ALIAS mygames_recovery_wire)
target_compile_features(mygames_recovery_wire PUBLIC c_std_11)
target_include_directories(mygames_recovery_wire PUBLIC games/doudizhu/include)
if(MSVC)
  target_compile_options(mygames_recovery_wire PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_recovery_wire PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()
target_compile_features(mygames_recovery_gate PUBLIC c_std_11)
target_include_directories(mygames_recovery_gate PUBLIC games/doudizhu/include)
if(MSVC)
  target_compile_options(mygames_recovery_gate PRIVATE /W4 /WX)
else()
  target_compile_options(mygames_recovery_gate PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
endif()

if(MYGAMES_ENABLE_GATEWAY)
  find_package(Chttp CONFIG REQUIRED)
  add_executable(mygames_server apps/server/main.c apps/server/ws_origin.c)
  target_compile_features(mygames_server PRIVATE c_std_11)
  target_link_libraries(mygames_server PRIVATE MyGames::Room CHttp::Server Salts::Platform Salts::CFlow)
  if(MSVC)
    target_compile_options(mygames_server PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_server PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()

include(CTest)
if(MYGAMES_ENABLE_TURBODB)
  find_package(TurboDB CONFIG REQUIRED)
  # The durable CFlow Game Owner hashes normalized actions with Salts::Core.
  # No separate or mixed native SDK is brought in.
  find_package(Salts CONFIG REQUIRED)
  add_library(mygames_room_store STATIC
    games/doudizhu/src/room_store_turbodb.c)
  add_library(MyGames::Store ALIAS mygames_room_store)
  add_library(mygames_consumer_projection STATIC
    games/doudizhu/src/consumer_projection_turbodb.c)
  add_library(MyGames::ConsumerProjection ALIAS mygames_consumer_projection)
  target_compile_features(mygames_consumer_projection PUBLIC c_std_11)
  target_include_directories(mygames_consumer_projection PUBLIC
    games/doudizhu/include)
  target_link_libraries(mygames_consumer_projection PRIVATE Orm::C)
  if(MSVC)
    target_compile_options(mygames_consumer_projection PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_consumer_projection PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(BUILD_TESTING)
    target_compile_definitions(mygames_consumer_projection PRIVATE
      MYGAMES_CONSUMER_FAULT_TESTING=1)
    add_executable(mygames_consumer_projection_test tests/consumer_projection_test.c)
    target_compile_definitions(mygames_consumer_projection_test PRIVATE
      MYGAMES_CONSUMER_FAULT_TESTING=1)
    target_link_libraries(mygames_consumer_projection_test PRIVATE
      MyGames::ConsumerProjection Orm::C)
    add_test(NAME mygames.consumer_projection
      COMMAND mygames_consumer_projection_test
        "${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mygames-consumer-ci.sqlite")
  endif()

  if(MYGAMES_ENABLE_GATEWAY)
    target_compile_definitions(mygames_server PRIVATE
      MYGAMES_WITH_TURBODB=1)
    target_link_libraries(mygames_server PRIVATE
      MyGames::Store MyGames::DouDizhuGameOwner Salts::Core)
    if(BUILD_TESTING)
      # Fault injection exists only in an explicitly test-qualified build.
      target_compile_definitions(mygames_server PRIVATE
        MYGAMES_STORE_FAULT_TESTING=1
        MYGAMES_GATEWAY_STORE_FAULT_TESTING=1)
    endif()
  endif()
  target_compile_features(mygames_room_store PUBLIC c_std_11)
  target_include_directories(mygames_room_store PUBLIC
    games/doudizhu/include)
  target_link_libraries(mygames_room_store PUBLIC
    MyGames::Room MyGames::DouDizhuCheckpoint PRIVATE Orm::C)

  # Production candidate: the same thread-affine CFlow Room Owner drives
  # these stage->TurboDB COMMIT->Game ACK operations. No worker or network
  # callback is created by this library itself.
  add_library(mygames_ddz_game_owner STATIC
    games/doudizhu/src/ddz_game_owner.c)
  add_library(MyGames::DouDizhuGameOwner ALIAS mygames_ddz_game_owner)
  target_compile_features(mygames_ddz_game_owner PUBLIC c_std_11)
  target_include_directories(mygames_ddz_game_owner PUBLIC games/doudizhu/include)
  target_link_libraries(mygames_ddz_game_owner PUBLIC MyGames::Store PRIVATE Salts::Core)
  if(MSVC)
    target_compile_options(mygames_ddz_game_owner PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_game_owner PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(MSVC)
    target_compile_options(mygames_room_store PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_room_store PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(BUILD_TESTING)
    target_compile_definitions(mygames_room_store PRIVATE
      MYGAMES_STORE_FAULT_TESTING=1)
    if(MYGAMES_ENABLE_GATEWAY)
      add_executable(mygames_room_gateway_probe
        tests/room_gateway_persist_probe.c)
      target_link_libraries(mygames_room_gateway_probe PRIVATE
        MyGames::Store Salts::Core)
    endif()
    if(MYGAMES_ENABLE_GATEWAY)
      add_executable(mygames_idle_expiry_probe
        tests/room_idle_expiry_probe.c)
      target_link_libraries(mygames_idle_expiry_probe PRIVATE MyGames::Store)
    endif()
    add_executable(mygames_game_owner_test
      tests/ddz_game_owner_integration_test.c)
    target_compile_features(mygames_game_owner_test PRIVATE c_std_11)
    target_compile_definitions(mygames_game_owner_test PRIVATE MYGAMES_STORE_FAULT_TESTING=1)
    target_link_libraries(mygames_game_owner_test PRIVATE
      MyGames::DouDizhuGameOwner MyGames::Store Salts::Core)
    if(MSVC)
      target_compile_options(mygames_game_owner_test PRIVATE /W4 /WX)
    else()
      target_compile_options(mygames_game_owner_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME mygames.game_owner
      COMMAND mygames_game_owner_test
        "${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mygames-game-owner-ci.sqlite")

    add_executable(mygames_game_results_test
      tests/ddz_game_results_test.c)
    target_compile_features(mygames_game_results_test PRIVATE c_std_11)
    target_compile_definitions(mygames_game_results_test PRIVATE
      MYGAMES_STORE_FAULT_TESTING=1)
    target_link_libraries(mygames_game_results_test PRIVATE
      MyGames::Store MyGames::DouDizhuCheckpoint
      MyGames::DouDizhuAIFeatures)
    if(MSVC)
      target_compile_options(mygames_game_results_test PRIVATE /W4 /WX)
    else()
      target_compile_options(mygames_game_results_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME mygames.game_results
      COMMAND mygames_game_results_test
        "${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mygames-game-results-ci.sqlite")

    add_executable(mygames_game_store_test
      tests/ddz_turbodb_game_commit_test.c)
    target_compile_definitions(mygames_game_store_test PRIVATE
      MYGAMES_STORE_FAULT_TESTING=1)
    target_compile_features(mygames_game_store_test PRIVATE c_std_11)
    target_link_libraries(mygames_game_store_test PRIVATE
      MyGames::Store MyGames::DouDizhuCheckpoint Orm::C)
    if(MSVC)
      target_compile_options(mygames_game_store_test PRIVATE /W4 /WX)
    else()
      target_compile_options(mygames_game_store_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME mygames.game_store
      COMMAND mygames_game_store_test
        "${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mygames-game-store-ci.sqlite")

    add_executable(mygames_turbodb_store_test
      tests/room_store_turbodb_test.c)
    target_compile_definitions(mygames_turbodb_store_test PRIVATE
      MYGAMES_STORE_FAULT_TESTING=1)
    target_link_libraries(mygames_turbodb_store_test PRIVATE
      MyGames::Store Orm::C)
    add_test(NAME mygames.turbodb_store
      COMMAND mygames_turbodb_store_test
        "${TurboDB_DRIVER_DIR}/turbodb_driver_sqlite${CMAKE_SHARED_MODULE_SUFFIX}"
        "${CMAKE_CURRENT_BINARY_DIR}/mygames-turbodb-ci.sqlite")
  endif()
endif()


if(BUILD_TESTING)
  # Loopback browser Origin protection is a pure C11 policy, never a claim
  # that the development bearer authenticates users or qualifies public WSS.
  add_executable(mygames_ws_origin_test
    tests/ws_origin_test.c apps/server/ws_origin.c)
  target_compile_features(mygames_ws_origin_test PRIVATE c_std_11)
  if(MSVC)
    target_compile_options(mygames_ws_origin_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ws_origin_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ws_origin COMMAND mygames_ws_origin_test)

  add_executable(mygames_ddz_game_wire_test tests/ddz_game_wire_test.c)
  target_link_libraries(mygames_ddz_game_wire_test PRIVATE MyGames::Room)
  if(MSVC)
    target_compile_options(mygames_ddz_game_wire_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_game_wire_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_game_wire COMMAND mygames_ddz_game_wire_test)

  add_executable(mygames_ddz_checkpoint_test tests/ddz_match_checkpoint_test.c)
  target_compile_features(mygames_ddz_checkpoint_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_checkpoint_test PRIVATE MyGames::DouDizhuCheckpoint)
  if(MSVC)
    target_compile_options(mygames_ddz_checkpoint_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_checkpoint_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_checkpoint COMMAND mygames_ddz_checkpoint_test)
  # A deterministic entropy provider exists ONLY in CTest-linked source
  # builds, not in a production/installed match binary or network API.
  target_compile_definitions(mygames_ddz_match PRIVATE MYGAMES_DDZ_MATCH_TESTING=1)
  add_executable(mygames_ddz_match_test tests/ddz_match_test.c)
  target_compile_features(mygames_ddz_match_test PRIVATE c_std_11)
  target_compile_definitions(mygames_ddz_match_test PRIVATE MYGAMES_DDZ_MATCH_TESTING=1)
  target_link_libraries(mygames_ddz_match_test PRIVATE MyGames::DouDizhuMatch)
  if(MSVC)
    target_compile_options(mygames_ddz_match_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_match_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_match COMMAND mygames_ddz_match_test)

  add_executable(mygames_ddz_round_test tests/ddz_round_test.c)
  target_compile_features(mygames_ddz_round_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_round_test PRIVATE MyGames::DouDizhuRound)
  if(MSVC)
    target_compile_options(mygames_ddz_round_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_round_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_round COMMAND mygames_ddz_round_test)
  # R0/R1 reuse the existing core CTest graph; no probe/consumer targets.
  add_executable(mygames_rl_targets_test tests/rl_targets_test.c)
  target_compile_features(mygames_rl_targets_test PRIVATE c_std_11)
  target_link_libraries(mygames_rl_targets_test PRIVATE MyGames::RLTargets)
  add_executable(mygames_ddz_ai_actions_test tests/ddz_ai_actions_test.c)
  target_compile_features(mygames_ddz_ai_actions_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_ai_actions_test PRIVATE MyGames::DouDizhuAI)
  if(MSVC)
    target_compile_options(mygames_rl_targets_test PRIVATE /W4 /WX)
    target_compile_options(mygames_ddz_ai_actions_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_rl_targets_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(mygames_ddz_ai_actions_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.rl_targets COMMAND mygames_rl_targets_test)
  add_test(NAME mygames.ddz_ai_actions COMMAND mygames_ddz_ai_actions_test)
  # R1 actor-only observation history / feature shape / full C11 match
  # episode sample tests reuse the existing native core test graph.
  add_executable(mygames_ddz_ai_features_test tests/ddz_ai_features_test.c)
  target_compile_features(mygames_ddz_ai_features_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_ai_features_test PRIVATE
    MyGames::DouDizhuAIFeatures)
  add_executable(mygames_ddz_dmc_episode_test tests/ddz_dmc_episode_test.c)
  target_compile_features(mygames_ddz_dmc_episode_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_dmc_episode_test PRIVATE
    MyGames::DMCReplay MyGames::RLTargets)
  if(MSVC)
    target_compile_options(mygames_ddz_ai_features_test PRIVATE /W4 /WX)
    target_compile_options(mygames_ddz_dmc_episode_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_ai_features_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(mygames_ddz_dmc_episode_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_ai_features COMMAND mygames_ddz_ai_features_test)
  add_test(NAME mygames.ddz_dmc_episode COMMAND mygames_ddz_dmc_episode_test)
  add_executable(mygames_ddz_dmc_selfplay_test
    tests/ddz_dmc_selfplay_test.c)
  target_compile_features(mygames_ddz_dmc_selfplay_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_dmc_selfplay_test PRIVATE
    MyGames::DMCSelfPlay)
  if(MSVC)
    target_compile_options(mygames_ddz_dmc_selfplay_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_dmc_selfplay_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_dmc_selfplay
    COMMAND mygames_ddz_dmc_selfplay_test)
  # This hook lives ONLY in BUILD_TESTING builds. A production
  # DMCSelfPlay library never contains a deterministic/deck-injection path.
  target_compile_definitions(mygames_ddz_dmc_selfplay PRIVATE
    MYGAMES_DDZ_MATCH_TESTING=1)
  add_executable(mygames_ddz_dmc_paired_eval_test
    tests/ddz_dmc_paired_eval_test.c)
  target_compile_features(mygames_ddz_dmc_paired_eval_test PRIVATE c_std_11)
  target_compile_definitions(mygames_ddz_dmc_paired_eval_test PRIVATE
    MYGAMES_DDZ_MATCH_TESTING=1)
  target_link_libraries(mygames_ddz_dmc_paired_eval_test PRIVATE
    MyGames::DMCSelfPlay)
  if(MSVC)
    target_compile_options(mygames_ddz_dmc_paired_eval_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_dmc_paired_eval_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_dmc_paired_eval
    COMMAND mygames_ddz_dmc_paired_eval_test)
  # Real-offline Self-Play adapter with injectable scoring callback.
  # The fake scorer is TEST-ONLY and not linked into any production service.
  add_executable(mygames_ddz_dmc_q_selector_test
    tests/ddz_dmc_q_selector_test.c)
  target_compile_features(mygames_ddz_dmc_q_selector_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_dmc_q_selector_test PRIVATE
    MyGames::DMCQSelector)
  if(MSVC)
    target_compile_options(mygames_ddz_dmc_q_selector_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_dmc_q_selector_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_dmc_q_selector
    COMMAND mygames_ddz_dmc_q_selector_test)
  # Real portable C11 reference implementation; no Python/Torch runtime
  # or CI-published weights. Zero-model golden, CRC, version and nonfinite.
  add_executable(mygames_ddz_dmc_q_cpu_test
    tests/ddz_dmc_q_cpu_test.c)
  target_compile_features(mygames_ddz_dmc_q_cpu_test PRIVATE c_std_11)
  target_link_libraries(mygames_ddz_dmc_q_cpu_test PRIVATE
    MyGames::DMCQCPUReference)
  if(MSVC)
    target_compile_options(mygames_ddz_dmc_q_cpu_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_ddz_dmc_q_cpu_test PRIVATE
      -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.ddz_dmc_q_cpu
    COMMAND mygames_ddz_dmc_q_cpu_test)









  add_executable(mygames_room_test tests/room_test.c)
  target_link_libraries(mygames_room_test PRIVATE MyGames::Room)
  if(MSVC)
    target_compile_options(mygames_room_test PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_room_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME mygames.room COMMAND mygames_room_test)
  add_executable(mygames_recovery_wire_test tests/recovery_wire_test.c)
  target_link_libraries(mygames_recovery_wire_test PRIVATE MyGames::RecoveryWire)
  add_test(NAME mygames.recovery_wire COMMAND mygames_recovery_wire_test)
  add_executable(mygames_recovery_gate_test tests/recovery_gate_test.c)
  target_link_libraries(mygames_recovery_gate_test PRIVATE MyGames::RecoveryGate)
  add_test(NAME mygames.recovery_gate COMMAND mygames_recovery_gate_test)
  add_executable(mygames_room_directory_test tests/room_directory_test.c)
  target_link_libraries(mygames_room_directory_test PRIVATE MyGames::Room)
  add_test(NAME mygames.room_directory COMMAND mygames_room_directory_test)
  add_executable(mygames_wire_test tests/wire_test.c)
  target_link_libraries(mygames_wire_test PRIVATE MyGames::Room)
  add_test(NAME mygames.wire COMMAND mygames_wire_test)
  add_executable(mygames_session_test tests/session_registry_test.c)
  target_link_libraries(mygames_session_test PRIVATE MyGames::Room)
  add_test(NAME mygames.sessions COMMAND mygames_session_test)
  add_executable(mygames_checkpoint_test tests/room_checkpoint_test.c)
  target_link_libraries(mygames_checkpoint_test PRIVATE MyGames::Room)
  add_test(NAME mygames.checkpoint COMMAND mygames_checkpoint_test)
  if(MYGAMES_ENABLE_GATEWAY)
    add_executable(mygames_cflow_owner_test tests/cflow_owner_test.c)
    target_link_libraries(mygames_cflow_owner_test PRIVATE MyGames::Room Salts::CFlow)
    add_test(NAME mygames.cflow_owner COMMAND mygames_cflow_owner_test)
  endif()
endif()

if(MYGAMES_ENABLE_FLOWIE)
  if(NOT MYGAMES_ENABLE_TURBODB)
    message(FATAL_ERROR "Flowie Outbox requires MYGAMES_ENABLE_TURBODB=ON")
  endif()
  find_package(Flowie CONFIG REQUIRED)
  add_library(mygames_outbox STATIC games/doudizhu/src/outbox_dispatch.c)
  add_library(MyGames::Outbox ALIAS mygames_outbox)
  target_compile_features(mygames_outbox PUBLIC c_std_11)
  target_include_directories(mygames_outbox PUBLIC games/doudizhu/include)
  target_link_libraries(mygames_outbox PUBLIC MyGames::Store)
  add_executable(mygames_outbox_publisher apps/publisher/main.c)
  target_compile_features(mygames_outbox_publisher PRIVATE c_std_11)
  target_link_libraries(mygames_outbox_publisher PRIVATE
    MyGames::Outbox Flowie::Client Salts::Platform)
  if(MSVC)
    target_compile_options(mygames_outbox PRIVATE /W4 /WX)
    target_compile_options(mygames_outbox_publisher PRIVATE /W4 /WX)
  else()
    target_compile_options(mygames_outbox PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(mygames_outbox_publisher PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  # Independent, opt-in **loopback mTLS-only** recovery endpoint.
  # Never linked into the unauthenticated development Room gateway.
  if(MYGAMES_ENABLE_GATEWAY)
    add_executable(mygames_recovery_service apps/consumer/recovery_service.c)
    target_compile_features(mygames_recovery_service PRIVATE c_std_11)
    if(BUILD_TESTING)
      target_compile_definitions(mygames_recovery_service PRIVATE
        MYGAMES_RECOVERY_TEST_TRACE=1)
    endif()
    target_link_libraries(mygames_recovery_service PRIVATE
      MyGames::RecoveryGate MyGames::RecoveryWire MyGames::Outbox
      MyGames::Store CHttp::Server Salts::Core Salts::Platform Orm::C)
    if(MSVC)
      target_compile_options(mygames_recovery_service PRIVATE /W4 /WX)
    else()
      target_compile_options(mygames_recovery_service PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
  if(BUILD_TESTING)
    target_compile_definitions(mygames_outbox_publisher PRIVATE MYGAMES_OUTBOX_TEST_FAULTS=1)
    add_executable(mygames_outbox_contract_test tests/outbox_contract_test.c)
    target_link_libraries(mygames_outbox_contract_test PRIVATE MyGames::Outbox)
    target_compile_features(mygames_outbox_contract_test PRIVATE c_std_11)
    add_test(NAME mygames.outbox_contract COMMAND mygames_outbox_contract_test)
    add_executable(mygames_flowie_consumer_projection_audit
      tests/flowie_consumer_projection_audit.c)
    target_link_libraries(mygames_flowie_consumer_projection_audit PRIVATE
      MyGames::ConsumerProjection Orm::C)
    target_compile_features(mygames_flowie_consumer_projection_audit PRIVATE c_std_11)
    add_executable(mygames_flowie_acl_client_test tests/flowie_acl_client.c)
    target_link_libraries(mygames_flowie_acl_client_test PRIVATE
      Flowie::Client MyGames::ConsumerProjection MyGames::Outbox Salts::Core Orm::C)
    target_compile_features(mygames_flowie_acl_client_test PRIVATE c_std_11)
    add_executable(mygames_archive_corrupt_test tests/flowie_archive_corrupt_test.c)
    target_compile_features(mygames_archive_corrupt_test PRIVATE c_std_11)
    target_link_libraries(mygames_archive_corrupt_test PRIVATE Orm::C)
    add_executable(mygames_event_reconcile_test apps/consumer/room_event_reconcile.c)
    target_compile_features(mygames_event_reconcile_test PRIVATE c_std_11)
    target_link_libraries(mygames_event_reconcile_test PRIVATE
      MyGames::Store MyGames::ConsumerProjection MyGames::Outbox
      Salts::Core Orm::C)
    add_executable(mygames_recovery_native_client_test
      tests/recovery_native_client.c)
    target_compile_features(mygames_recovery_native_client_test PRIVATE c_std_11)
    target_link_libraries(mygames_recovery_native_client_test PRIVATE
      CHttp::Client Salts::Platform)
    if(MSVC)
      target_compile_options(mygames_recovery_native_client_test PRIVATE /W4 /WX)
    else()
      target_compile_options(mygames_recovery_native_client_test PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_executable(mygames_flowie_broker_test tests/flowie_local_broker.c)
    target_link_libraries(mygames_flowie_broker_test PRIVATE
      Flowie::Flowie Salts::Core)
    add_executable(mygames_outbox_seed_test tests/flowie_outbox_seed.c)
    target_link_libraries(mygames_outbox_seed_test PRIVATE MyGames::Store)
    add_executable(mygames_outbox_audit_test tests/flowie_outbox_audit.c)
    target_link_libraries(mygames_outbox_audit_test PRIVATE MyGames::Store)
    target_compile_features(mygames_flowie_broker_test PRIVATE c_std_11)
    target_compile_features(mygames_outbox_seed_test PRIVATE c_std_11)
    target_compile_features(mygames_outbox_audit_test PRIVATE c_std_11)
  endif()
endif()
