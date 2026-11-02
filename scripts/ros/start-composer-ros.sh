#!/usr/bin/env bash
# The ROS-backed Composer runtime is intentionally unavailable during the
# composable-cell cutover. Do not probe or load deleted native artifacts.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
# shellcheck source=../lib/composable-cell-runtime-availability.sh
source "${WORKSPACE}/scripts/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
