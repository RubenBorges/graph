#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

readonly BUILD_DIR="build"
readonly INSTALL_DIR="dist"
readonly OUT_DIR="out"

echo "--- Cleaning old build ---"
rm -rf "$BUILD_DIR" "$INSTALL_DIR" "$OUT_DIR"

echo "--- Done! Removed $BUILD_DIR, $INSTALL_DIR, and $OUT_DIR if they existed. ---"
