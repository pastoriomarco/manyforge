#!/usr/bin/env bash
# Typed boundary for the former detached Step18 proof baseline runner.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 04fd8c71a44e3f1a1e1db95576ff574d17b8ba99.  Proof wrapping and
# non-regression verification remain executable.  The removed process runner
# exercised patched deleted owners and returns only with I6/N + I8/P5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
