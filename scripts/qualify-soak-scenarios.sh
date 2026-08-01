#!/usr/bin/env bash
# Typed boundary for verification/matrices/soak_qualification.yaml.
# Exact historical wrapper: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 827162eaa4c3fcc93096e2451ceb20816539a96b.
# Preserve at restoration: matrix/output/port selection, bounded local-process
# readiness, soak result propagation, and durable artifacts. The former
# Python Composer plus mock-Kernel soak lane is intentionally absent.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: soak scenario qualification returns with the qualified headless Program and Kernel clients."
manyforge_fail_composable_cell_runtime_unavailable
