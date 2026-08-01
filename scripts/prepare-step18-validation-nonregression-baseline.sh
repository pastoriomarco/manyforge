#!/usr/bin/env bash
# Typed boundary for the former detached Step18 validation baseline build.
#
# Historical source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 4717ac5fa1c0b853e5fe2b83052d614baea0d031.  Pure manifest,
# identity, proof-artifact and non-regression verifiers remain executable.
# The removed body patched and built deleted contact/policy/runtime owners;
# its replacement belongs to I6/N plus I8/P5 after those C++ owners qualify.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
