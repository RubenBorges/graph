#!/usr/bin/env bash

set -euo pipefail

require_command() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "Error: required command '$1' was not found in PATH." >&2
		exit 1
	fi
}

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

readonly PRESET_NAME="dev"
readonly BUILD_DIR="build"
readonly INSTALL_DIR="dist"
readonly OUT_DIR="out"
readonly PRESETS_FILE="$SCRIPT_DIR/CMakePresets.json"

require_command cmake

if [[ ! -f "$PRESETS_FILE" ]]; then
	echo "Error: CMakePresets.json was not found in $SCRIPT_DIR." >&2
	exit 1
fi

echo "--- Cleaning old build ---"
rm -rf "$BUILD_DIR" "$INSTALL_DIR" "$OUT_DIR"

echo "--- Running CMake workflow preset '$PRESET_NAME' ---"
cmake --workflow --preset "$PRESET_NAME"

echo "--- Done! Workflow preset '$PRESET_NAME' finished. Executable should be in dist/bin/ ---"