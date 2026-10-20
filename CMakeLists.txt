cmake_minimum_required(VERSION 3.20)
project(moho64 C CXX)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Floating point: no FMA contraction or fast-math anywhere - the sim must give the same bits on
# every compiler and CPU (replays, multiplayer).
if(NOT MSVC)
  add_compile_options(-ffp-contract=off -fno-fast-math)
endif()

# Link-time optimisation (the Lua VM's table and call functions get inlined across files).
option(MOHO64_LTO "Link-time optimisation" OFF)
if(MOHO64_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# mimalloc (MIT, microsoft/mimalloc v2.1.7): the Lua heap's allocator - scripts allocate a lot
# of small tables and strings, and mimalloc is much faster at that than the C runtime's malloc.
option(MOHO64_MIMALLOC "Use mimalloc for the Lua heap" ON)
if(MOHO64_MIMALLOC)
  add_library(mimalloc STATIC third_party/mimalloc/src/static.c)
  target_include_directories(mimalloc PUBLIC third_party/mimalloc/include)
  target_compile_definitions(mimalloc PRIVATE MI_SECURE=0 MI_STAT=0 MI_DEBUG=0 NDEBUG)
  if(NOT MSVC)
    target_compile_options(mimalloc PRIVATE -w)
  endif()
endif()

# GPG-dialect Lua 5.0 (see docs/lua-dialect.md)
file(GLOB LUA_SOURCES third_party/lua/*.c)
add_library(gpglua STATIC ${LUA_SOURCES})
target_include_directories(gpglua PUBLIC third_party/lua)
if(MOHO64_MIMALLOC)
  target_link_libraries(gpglua PUBLIC mimalloc)
  target_compile_definitions(gpglua PRIVATE MOHO64_MIMALLOC=1)
endif()
if(NOT MSVC)
  target_compile_options(gpglua PRIVATE -w)
endif()

add_library(zlibinflate STATIC
  third_party/zlib/adler32.c third_party/zlib/crc32.c third_party/zlib/inffast.c
  third_party/zlib/inflate.c third_party/zlib/inftrees.c third_party/zlib/zutil.c)
target_include_directories(zlibinflate PUBLIC third_party/zlib)
if(NOT MSVC)
  target_compile_options(zlibinflate PRIVATE -w)
endif()

add_library(mohocore STATIC
  src/core/log.cpp src/core/crash.cpp src/core/hostfs.cpp src/core/zip.cpp src/core/vfs.cpp
  src/script/script_state.cpp src/script/core_bindings.cpp src/script/mods.cpp
  src/script/threads.cpp src/script/script_handle.cpp src/script/mount.cpp src/sim/script_object.cpp src/sim/blueprints.cpp src/sim/replay.cpp
  src/sim/terrain.cpp src/sim/sim.cpp src/sim/entities.cpp src/sim/effects.cpp src/sim/bp_reflect.cpp
  src/sim/bp_derived.cpp src/sim/skeleton.cpp src/sim/motion.cpp src/sim/air.cpp src/sim/transport.cpp src/sim/navigation.cpp src/sim/hpath.cpp src/sim/landnav.cpp src/sim/formation.cpp
  src/sim/steering.cpp
  src/sim/entity_grid.cpp
  src/sim/prop_motor.cpp
  src/sim/ai_threat.cpp
  src/sim/ai_brain.cpp
  src/sim/commands.cpp src/sim/economy.cpp src/sim/build.cpp src/sim/collision.cpp
  src/sim/combat.cpp src/sim/damage.cpp src/sim/projectile.cpp src/sim/anim.cpp src/sim/intel.cpp src/sim/recorder.cpp src/sim/fingerprint.cpp
  src/sim/script_bits.cpp src/sim/command_stream.cpp src/sim/session.cpp src/sim/sim_view.cpp src/sim/command_writer.cpp src/sim/ai_personality.cpp src/sim/ai_platoon.cpp
  src/sim/ai_build.cpp src/sim/prefetch.cpp)
target_include_directories(mohocore PUBLIC src)
target_link_libraries(mohocore PUBLIC gpglua zlibinflate)
target_link_libraries(mohocore PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_executable(moho64 src/app/main.cpp)
target_link_libraries(moho64 PRIVATE mohocore)
if(WIN32)
  target_link_libraries(moho64 PRIVATE shell32)
endif()
if(MINGW)
  target_link_options(moho64 PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(slot_sort_test tests/slot_sort_test.cpp)
  target_include_directories(slot_sort_test PRIVATE src)
  add_executable(dialect_test tests/dialect_test.cpp)
  target_include_directories(dialect_test PRIVATE src)
  target_link_libraries(dialect_test PRIVATE gpglua mohocore)
  add_test(NAME slot_sort COMMAND slot_sort_test)
  add_executable(prefetch_test tests/prefetch_test.cpp)
  target_link_libraries(prefetch_test PRIVATE mohocore)
  add_test(NAME simulation_prefetch COMMAND prefetch_test)
  add_test(NAME lua_dialect COMMAND dialect_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/dialect_test.lua)
  set_tests_properties(lua_dialect PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_library(test_results STATIC tests/framework/host/results.cpp)
  target_include_directories(test_results PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(test_results PUBLIC mohocore)
  add_library(sim_test_application OBJECT src/app/main.cpp)
  target_compile_definitions(sim_test_application PRIVATE main=RunTestApplication)
  target_link_libraries(sim_test_application PRIVATE mohocore)
  add_executable(moho64_test tests/framework/host/command_main.cpp $<TARGET_OBJECTS:sim_test_application>)
  target_link_libraries(moho64_test PRIVATE test_results)
endif()

option(MOHO64_BUILD_UI "Build the software interface hosts" OFF)
if(MOHO64_BUILD_UI)
  file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS ui/*.cpp)
  list(FILTER UI_SOURCES EXCLUDE REGEX "/(app/entry|maui/clipboard)\\.cpp$")
  add_library(mohoui STATIC ${UI_SOURCES} ui/third_party/pl_mpeg/pl_mpeg.c)
  target_include_directories(mohoui PUBLIC ui ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(mohoui PUBLIC mohocore)
  if(WIN32)
    target_link_libraries(mohoui PUBLIC shell32 user32 gdi32 winmm ole32)
  endif()
  add_executable(moho64ui ui/app/entry.cpp ui/maui/clipboard.cpp)
  target_link_libraries(moho64ui PRIVATE mohoui)
  add_custom_command(TARGET moho64ui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/ui/data $<TARGET_FILE_DIR:moho64ui>/data
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data/fixes $<TARGET_FILE_DIR:moho64ui>/data/fixes)
  if(BUILD_TESTING)
    file(GLOB TEST_FRAMEWORK_SOURCES CONFIGURE_DEPENDS tests/framework/host/*.cpp)
    list(FILTER TEST_FRAMEWORK_SOURCES EXCLUDE REGEX "/(main|command_main|results)\\.cpp$")
    add_library(mohotest STATIC ${TEST_FRAMEWORK_SOURCES})
    target_link_libraries(mohotest PUBLIC mohoui test_results)
    add_executable(moho64ui_test tests/framework/host/main.cpp)
    target_link_libraries(moho64ui_test PRIVATE mohotest)
    add_dependencies(moho64ui_test moho64ui)
    add_executable(framework_host tests/framework/selftests/runtime_host.cpp)
    target_link_libraries(framework_host PRIVATE mohotest)
    add_executable(ui_state_isolation tests/framework/selftests/state_isolation.cpp ui/maui/clipboard.cpp)
    target_link_libraries(ui_state_isolation PRIVATE mohoui)
    add_test(NAME ui_state_isolation COMMAND ui_state_isolation)
  endif()
endif()

# Portable testing infrastructure has no game-data or desktop dependency.
if(BUILD_TESTING)
  if(TARGET moho64view)
    add_library(viewer_test_application OBJECT render/main.cpp)
    target_compile_definitions(viewer_test_application PRIVATE main=RunTestApplication)
    target_link_libraries(viewer_test_application PRIVATE mohorender)
    add_executable(moho64view_test tests/framework/host/command_main.cpp $<TARGET_OBJECTS:viewer_test_application>)
    target_link_libraries(moho64view_test PRIVATE test_results mohorender)
  endif()
  add_executable(lua_test_syntax tests/framework/selftests/lua_syntax.cpp)
  target_include_directories(lua_test_syntax PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(lua_test_syntax PRIVATE gpglua)
  file(GLOB TEST_CASE_LUA CONFIGURE_DEPENDS tests/ui/*.lua tests/fixtures/*.lua tests/fixtures/session_replay/*.lua tests/fixtures/session_replay/*/*.lua tests/replay/*.lua tests/replay/fixtures/*.lua tests/replay/selftests/*.lua ui/tests/*.lua ui/tests/maps/*/*.lua)
  add_test(NAME test_case_syntax COMMAND lua_test_syntax ${TEST_CASE_LUA} ${CMAKE_CURRENT_SOURCE_DIR}/tests/framework/lua/framework.lua)
  add_test(NAME portable_lua_framework COMMAND dialect_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/framework/selftests/framework_test.lua)
  set_tests_properties(portable_lua_framework PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME lua_session_capture COMMAND dialect_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/selftests/capture_test.lua)
  set_tests_properties(lua_session_capture PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME portable_test_runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/framework/selftests/test_runner.py)
    add_test(NAME session_replay_runner COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/replay/selftests/test_replay.py)
    if(MOHO64_BUILD_UI)
      add_test(NAME portable_host_contracts COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/framework/selftests/test_host.py $<TARGET_FILE:framework_host>)
    endif()
  endif()
endif()
