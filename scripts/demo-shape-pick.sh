#!/usr/bin/env bash
# demo-shape-pick.sh — backwards-compat shim for scripts/launch.sh.
#
# Original per-demo CLI flags (--requested-shape, --selection-request,
# --rebuild, etc.) are no longer parsed here; use the unified launcher
# overrides (REBUILD=, MODEL_PROFILE=, --program/--deployment, …) or
# edit the deployment YAML directly.

set -euo pipefail
exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant on \
    --scenario ur5-semantic-shape-pick \
    --non-interactive \
    --yes \
    "$@"
