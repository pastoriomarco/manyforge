#!/usr/bin/env bash
# Unavailable product qualification selector for the real-Lite6 runtime.

set -euo pipefail

printf '%s\n' \
    'composable_cell_runtime_unavailable: the real-Lite6 runtime matrix returns at I10/R after its retained products and generic lifecycle path qualify' \
    >&2
exit 78
