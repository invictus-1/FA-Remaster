cmake_minimum_required(VERSION 3.20)
project(moho64 C CXX)

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
  src/core/log.cpp src/core/hostfs.cpp src/core/zip.cpp src/core/vfs.cpp
  src/script/script_state.cpp src/script/core_bindings.cpp src/script/mods.cpp
  src/script/threads.cpp src/sim/script_object.cpp src/sim/blueprints.cpp src/sim/replay.cpp
  src/sim/terrain.cpp src/sim/sim.cpp src/sim/entities.cpp src/sim/effects.cpp src/sim/bp_reflect.cpp
  src/sim/bp_derived.cpp src/sim/skeleton.cpp src/sim/motion.cpp src/sim/air.cpp src/sim/transport.cpp src/sim/navigation.cpp src/sim/hpath.cpp src/sim/landnav.cpp src/sim/formation.cpp
  src/sim/steering.cpp
  src/sim/commands.cpp src/sim/economy.cpp src/sim/build.cpp src/sim/collision.cpp
  src/sim/combat.cpp src/sim/damage.cpp src/sim/projectile.cpp src/sim/anim.cpp src/sim/intel.cpp)
target_include_directories(mohocore PUBLIC src)
target_link_libraries(mohocore PUBLIC gpglua zlibinflate)

add_executable(moho64 src/app/main.cpp)
target_link_libraries(moho64 PRIVATE mohocore)
if(WIN32)
  target_link_libraries(moho64 PRIVATE shell32)
endif()
if(MINGW)
  target_link_options(moho64 PRIVATE -static)
endif()

add_executable(dialect_test tests/dialect_test.cpp)
target_include_directories(dialect_test PRIVATE src)
target_link_libraries(dialect_test PRIVATE gpglua)
