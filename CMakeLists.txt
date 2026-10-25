cmake_minimum_required(VERSION 3.25)
project(MyGames VERSION 0.1.0 LANGUAGES C)

option(MYGAMES_ENABLE_GATEWAY "Build optional CHttp/CNet development gateway" OFF)
option(MYGAMES_ENABLE_TURBODB "Build optional TurboDB transactional room storage" OFF)

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
    add_executable(mygames_turbodb_store_test
      tests/room_store_turbodb_test.c)
    target_compile_definitions(mygames_turbodb_store_test PRIVATE
      MYGAMES_STORE_FAULT_TESTING=1)
    target_link_libraries(mygames_turbodb_store_test PRIVATE
      MyGames::Store)
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
