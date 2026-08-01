#!/usr/bin/env bash
# Typed boundary for the former detached runtime-collision baseline runner.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 4b5dffe72562d8dcf7399fc0df3d17908eb817ee.  The bounded artifact
# wrapper and parity verifier remain executable; the removed process/affinity
# runner targeted the deleted runtime harness and returns at I6/N + I8/P5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
