#!/usr/bin/env bash
# Unavailable boundary for the real-Lite6 read-only product client.

set -euo pipefail

printf '%s\n' \
    'composable_cell_runtime_unavailable: real-Lite6 read-only launch requires I10/R retained-product and vendor-observation qualification' \
    >&2
exit 78
