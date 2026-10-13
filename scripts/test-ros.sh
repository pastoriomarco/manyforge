#!/usr/bin/env bash
# Run ROS adapter tests in the official full ROS Lyrical image.
#
# Intended usage:
#   docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh

set -euo pipefail

WORKSPACE="${WORKSPACE:-/workspace}"
ROS_DISTRO="${ROS_DISTRO:-lyrical}"

if [ -f "/opt/ros/${ROS_DISTRO}/setup.bash" ]; then
    # shellcheck disable=SC1090
    set +u
    source "/opt/ros/${ROS_DISTRO}/setup.bash"
    set -u
fi

if env | grep -Fq "${WORKSPACE}/install"; then
    echo "Canonical ROS tests refuse the source-mounted workspace install overlay." >&2
    env | grep -F "${WORKSPACE}/install" >&2
    exit 2
fi

export PYTHONPYCACHEPREFIX="${PYTHONPYCACHEPREFIX:-/tmp/manyforge-pycache}"
export PYTHONPATH="${WORKSPACE}/manyforge_ros:${WORKSPACE}/manyforge_behavior:${WORKSPACE}/manyforge_medkit_adapter:${WORKSPACE}:${PYTHONPATH:-}"

cd "$WORKSPACE"
pytest manyforge_ros/tests/ manyforge_medkit_adapter/tests/ -v
