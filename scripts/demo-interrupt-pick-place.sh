#!/usr/bin/env bash
# demo-interrupt-pick-place.sh — backwards-compat shim for scripts/launch.sh.

set -euo pipefail
exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant off \
    --scenario ur5-interrupt-pick-place \
    --non-interactive \
    --yes \
    "$@"
