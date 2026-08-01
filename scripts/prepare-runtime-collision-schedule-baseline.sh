#!/usr/bin/env bash
# Typed boundary for the former detached Step18 runtime-schedule baseline build.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 7f0732e68f337c70a8fc5e0a5c2e86caa7276740.  Source-identity,
# artifact wrapping and parity verification remain in scripts/qualification.
# The removed body created worktrees, applied old-owner overlays, built and
# executed the deleted runtime harness; a new baseline producer returns only
# with the addressed I6/N runtime and I8/P5 collision/Stop owner.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
