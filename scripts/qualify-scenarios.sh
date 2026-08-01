#!/usr/bin/env bash
# Typed boundary for verification/matrices/core_qualification.yaml.
# Exact historical wrapper: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob b8283a6ae55ee95acbad764a1c95e717296c18f2.
# Preserve at restoration: matrix/output/catalog/port selection, workspace
# identity, bounded readiness, result propagation, and artifact retention. The
# former Python Composer plus mock-Kernel product selector is intentionally
# absent and is never a compatibility or qualification lane.

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: core scenario qualification returns with the qualified headless Program and Kernel clients."
manyforge_fail_composable_cell_runtime_unavailable
