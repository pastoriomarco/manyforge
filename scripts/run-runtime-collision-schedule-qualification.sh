#!/usr/bin/env bash
# Typed boundary for the former current-candidate runtime-collision runner.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 01415f5e74dfa0b979d98c13425c084be339eaa5.  Pure source identity,
# artifact wrapping and schedule verification remain executable.  The removed
# build/process/affinity body returns with the addressed I6/N runtime and the
# single I8/P5 collision/Stop owner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
