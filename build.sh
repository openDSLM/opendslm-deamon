#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

if [ ! -d "$BUILD_DIR" ]; then
    echo "[build] configuring build directory: $BUILD_DIR"
    meson setup "$BUILD_DIR" "$SCRIPT_DIR" "$@"
else
    echo "[build] reconfiguring existing build directory"
    meson setup "$BUILD_DIR" "$SCRIPT_DIR" --reconfigure "$@"
fi

echo "[build] compiling targets"
meson compile -C "$BUILD_DIR"

echo "[build] complete"
