#!/usr/bin/env bash
# Unavailable boundary for the real-Lite6 supervised preflight client.

set -euo pipefail

printf '%s\n' \
    'composable_cell_runtime_unavailable: real-Lite6 supervised preflight requires I7/L0 + I10/R qualification' \
    >&2
exit 78
