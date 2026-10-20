#!/bin/sh
# Native CMake build with deterministic floating point flags and header tracking.
# Extra arguments go to CMake, e.g. ./build.sh -DMOHO64_BUILD_UI=ON -DMOHO64_FETCH_SDL3=ON
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${MOHO64_BUILD_DIR:-"$ROOT/build-sh"}
cmake -S "$ROOT" -B "$OUT" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$OUT" --parallel "${MOHO64_JOBS:-4}"
