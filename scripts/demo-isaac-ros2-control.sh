#!/usr/bin/env bash
# demo-isaac-ros2-control.sh — backwards-compat shim for scripts/launch.sh.
#
# Forwards verbs (start/stop/status/restart/smoke/preflight) and selects the
# isaac-sim + isaac-ur10e-light-scene scenario. Env-var overrides
# (ARM_ACTION, GRIPPER_ACTION, ROS_DISCOVERY_TIMEOUT_S, …) are unchanged
# and read by scripts/lib/isaac.sh.
#
# The legacy `load|recover` verbs no longer have a separate code path —
# `start` is idempotent (the new launcher re-loads program/deployment via
# /api/program/load with forceDiscardOverrides=true on every start).

set -euo pipefail

case "${1:-}" in
    load|recover)
        echo "  note: '$1' is deprecated; mapping to 'start'." >&2
        set -- start "${@:2}"
        ;;
esac

exec "$(dirname "$0")/launch.sh" \
    --lane isaac-sim \
    --assistant off \
    --scenario isaac-ur10e-light-scene \
    --non-interactive \
    --yes \
    "$@"
