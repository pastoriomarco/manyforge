#!/usr/bin/env bash
# Host-side build wrapper for manyforge_core
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE_DIR="$(cd "$SCRIPT_DIR/../manyforge_core" && pwd)"
BUILD_DIR="$CORE_DIR/build"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. "$@"
make -j"$(nproc)"
