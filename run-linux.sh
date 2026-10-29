#!/bin/sh
# Run the Linux game with the dependencies built locally for this checkout.
set -eu
PROJECT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$PROJECT_DIR/build-tools/sdl/lib:$PROJECT_DIR/build-tools/dxc/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$PROJECT_DIR/build-ui/moho64ui" --log "$PROJECT_DIR/build-ui/game.log" "$@"
