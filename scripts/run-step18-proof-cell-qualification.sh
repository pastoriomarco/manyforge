#!/usr/bin/env bash
# Typed boundary for the former aggregate Step18 proof-cell producer.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 0baa480b11d653cde0e12ab346fbab060a428774.  Pure proof wrapping,
# manifest generation and verification remain executable, and dedicated
# retained Core/collision lanes stay separate.  This deleted-owner aggregate
# returns only after I6/N and I8/P5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
