#!/usr/bin/env bash
# Historical shape-pick demo entrypoint retained for truthful runbook links.
# The former compatibility forwarding behavior is removed; restoration gate: N.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
