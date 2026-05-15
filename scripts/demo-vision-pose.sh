#!/usr/bin/env bash
# demo-vision-pose.sh — backwards-compat shim for scripts/launch.sh.

set -euo pipefail
exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant on \
    --scenario ur5-vision-pose-pick \
    --non-interactive \
    --yes \
    "$@"
