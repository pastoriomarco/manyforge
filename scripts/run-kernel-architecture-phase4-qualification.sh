#!/usr/bin/env bash
# Typed boundary for the former Kernel Architecture Phase4 aggregate.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 52c7b76e072ac2633a99bb13127d1c2834e7e6da.  Dedicated retained
# Core/collision sanitizer and structural checks remain separate and current.
# The removed aggregate launched deleted native, Composer, visual and Isaac
# owners, so it returns only after I8/P5 and I9/I.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
