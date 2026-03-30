#!/usr/bin/env bash
# Host-side test wrapper for manyforge_core
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(cd "$SCRIPT_DIR/../manyforge_core/build" && pwd)"

cd "$BUILD_DIR"
ctest --output-on-failure "$@"
