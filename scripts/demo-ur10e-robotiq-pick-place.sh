#!/usr/bin/env bash
# demo-ur10e-robotiq-pick-place.sh — backwards-compat shim for scripts/launch.sh.

set -euo pipefail
exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant off \
    --scenario ur10e-pick-place \
    --non-interactive \
    --yes \
    "$@"
