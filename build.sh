#!/bin/sh
# Plain build without CMake (Linux/macOS with g++ or clang++). Output: build-sh/moho64
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}; CXX=${CXX:-c++}; OUT=build-sh
mkdir -p $OUT/obj
for f in third_party/lua/*.c third_party/zlib/adler32.c third_party/zlib/crc32.c third_party/zlib/inffast.c \
         third_party/zlib/inflate.c third_party/zlib/inftrees.c third_party/zlib/zutil.c; do
  o=$OUT/obj/$(basename $f .c).o
  [ "$o" -nt "$f" ] || $CC -O2 -w -Ithird_party/lua -Ithird_party/zlib -c "$f" -o "$o"
done
for f in src/core/*.cpp src/script/*.cpp src/app/main.cpp; do
  o=$OUT/obj/$(basename $f .cpp).o
  $CXX -std=c++20 -O2 -Isrc -Ithird_party/lua -Ithird_party/zlib -c "$f" -o "$o"
done
$CXX -o $OUT/moho64 $OUT/obj/*.o -lm
echo "built $OUT/moho64"
