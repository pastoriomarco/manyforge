#!/usr/bin/env bash
# The legacy native/ros2_control runtime was deliberately removed during the
# composable-cell cutover. Keep this entrypoint fail-closed until its target
# adapter is restored and qualified.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
# shellcheck source=../lib/composable-cell-runtime-availability.sh
source "${WORKSPACE}/scripts/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
