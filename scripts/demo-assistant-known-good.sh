#!/usr/bin/env bash
# Historical assistant demo entrypoint retained for truthful runbook links.
# The former compatibility forwarding behavior is removed; restoration gate: N/I6.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/composable-cell-runtime-availability.sh"
manyforge_fail_composable_cell_runtime_unavailable
