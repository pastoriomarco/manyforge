#!/usr/bin/env bash
# Full ROS runtime qualification depends on the native planning/execution
# adapter removed by the composable-cell cutover. Fail explicitly rather than
# loading a stale module from a persistent build cache.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${WORKSPACE}/scripts/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
