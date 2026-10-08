cmake_minimum_required(VERSION 3.20)
project(moho64 C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# GPG-dialect Lua 5.0 (see docs/lua-dialect.md)
file(GLOB LUA_SOURCES third_party/lua/*.c)
add_library(gpglua STATIC ${LUA_SOURCES})
target_include_directories(gpglua PUBLIC third_party/lua)
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
  src/sim/terrain.cpp src/sim/sim.cpp src/sim/entities.cpp src/sim/effects.cpp)
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
