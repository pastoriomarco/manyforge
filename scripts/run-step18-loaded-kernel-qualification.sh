#!/usr/bin/env bash
# Typed boundary for the former loaded-Kernel Step18 producer.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 51d1982f8855adaf26c076df811defc553c18fbc.  The restored artifact
# verifier retains the 30x3 evidence contract.  The removed body built and
# loaded the deleted aggregate Kernel/runtime and returns only at I8/P5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
