#!/usr/bin/env bash
# Unavailable boundary for the Isaac topic-based control product lane.

set -euo pipefail

printf '%s\n' \
    'composable_cell_runtime_unavailable: Isaac topic-based control requires I5/H0 + I6/N + I8/P5 + I9/I; no historical launch fallback exists' \
    >&2
exit 78
