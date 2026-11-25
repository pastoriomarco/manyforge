#!/usr/bin/env bash
# Full ROS runtime qualification depends on the native planning/execution
# adapter that was removed. Fail explicitly rather than
# loading a stale module from a persistent build cache.

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
# shellcheck source=lib/runtime-availability.sh
source "${WORKSPACE}/scripts/lib/runtime-availability.sh"
manyforge_fail_runtime_unavailable
