#!/usr/bin/env bash
# The ROS-backed Composer runtime is intentionally unavailable.
# Do not probe or load deleted native artifacts.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
# shellcheck source=../lib/runtime-availability.sh
source "${WORKSPACE}/scripts/lib/runtime-availability.sh"
manyforge_fail_runtime_unavailable
