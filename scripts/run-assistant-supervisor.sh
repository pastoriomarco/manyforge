#!/usr/bin/env bash
# Typed boundary for the historical assistant status-feeder wrapper.
#
# Exact implementation source:
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# 3daf0d53a35ba250d825dc45d66d3a575c04f884.
#
# Its interpreter selection and foreground process lifecycle return as bounded
# operator workflow support. Its Composer runtime-status feeder becomes a thin
# owner-issued status read exists at H0/I5, but this operational supervisor
# returns only with assistant/provider operation at N/I6. No venv probe,
# working-directory change, import, or process launch occurs now.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${SCRIPT_DIR}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: assistant supervision"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE+=" returns at N/I6 with the qualified assistant/provider client."
manyforge_fail_composable_cell_runtime_unavailable
