#!/usr/bin/env bash
# Typed boundary for verification/matrices/native_live_evaluation.yaml.
# Exact historical wrapper: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 7f843e6a7b92cbf5a471474f9cc6536a5538d48d.
# Preserve at restoration: explicit output directory and port, Python/RoboPlan
# artifact discovery, the native matrix identity, bounded local-process
# readiness, and argument forwarding. The former auto-Kernel Composer launch
# is absent until the resource-addressed native client is qualified.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: native scenario qualification returns with the qualified headless Program and Kernel clients."
manyforge_fail_composable_cell_runtime_unavailable
