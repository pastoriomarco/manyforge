#!/usr/bin/env bash
# Typed boundary for the former aggregate controlled-Stop qualification.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 29bcf30c80cd4453e84e05779e8356e4a22144cb.  That body combined
# deleted Composer/CycleManager, NativeKernelBundle, RoboPlan and ROS factory
# suites.  Current focused Core, collision, Event/History and client tests stay
# in their owning package lanes; the aggregate returns only after I8/P5.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
