cmake_minimum_required(VERSION 3.25)
project(MyGames VERSION 0.1.0 LANGUAGES C)

option(MYGAMES_ENABLE_GATEWAY "Build optional CHttp/CNet development gateway" OFF)
option(MYGAMES_ENABLE_TURBODB "Build optional TurboDB transactional room storage" OFF)
option(MYGAMES_ENABLE_FLOWIE "Build optional Flowie QoS1 Outbox publisher and local broker test" OFF)

add_library(mygames_room STATIC games/doudizhu/src/room.c
  games/doudizhu/src/room_directory.c
  games/doudizhu/src/wire.c
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
  add_executable(mygames_server apps/server/main.c)
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
      MyGames::Store Salts::Core)
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
    MyGames::Room PRIVATE Orm::C)
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
