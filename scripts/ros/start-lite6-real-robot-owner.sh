#!/usr/bin/env bash
# Unavailable boundary for the real-Lite6 runtime owner selection.

set -euo pipefail

printf '%s\n' \
    'composable_cell_runtime_unavailable: real-Lite6 owner startup requires I4/M0 + I7/L0 + I10/R; no legacy process owner exists' \
    >&2
exit 78
