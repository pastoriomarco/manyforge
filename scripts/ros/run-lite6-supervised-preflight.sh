#!/usr/bin/env bash
# Observe the bounded ROS boundary of an already-running supervised owner.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
ROBOT_INSTANCE_ID=${MANYFORGE_LITE6_ROBOT_INSTANCE_ID:-lite6-a}
OWNER_ID=${MANYFORGE_LITE6_TRANSPORT_OWNER_ID:-${ROBOT_INSTANCE_ID}:sole-owner}
ROS_NAMESPACE=${MANYFORGE_LITE6_ROS_NAMESPACE:-}
HARDWARE_NAMESPACE=${MANYFORGE_LITE6_HARDWARE_NAMESPACE:-ufactory}
DURATION_MS=${MANYFORGE_LITE6_SUPERVISED_PREFLIGHT_DURATION_MS:-5000}
SAMPLE_PERIOD_MS=${MANYFORGE_LITE6_SUPERVISED_PREFLIGHT_SAMPLE_PERIOD_MS:-20}

if [[ -z "${OWNER_ID}" || -z "${HARDWARE_NAMESPACE}" ]]; then
    echo "The supervised owner and hardware namespace must be non-empty" >&2
    exit 2
fi
ROS_NAMESPACE=${ROS_NAMESPACE#/}
ROS_NAMESPACE=${ROS_NAMESPACE%/}
SUPERVISED_ROOT="/${HARDWARE_NAMESPACE}/supervised_lifecycle"
if [[ -n "${ROS_NAMESPACE}" ]]; then
    SUPERVISED_ROOT="/${ROS_NAMESPACE}${SUPERVISED_ROOT}"
fi

if [[ "${MANYFORGE_SKIP_REAL_LITE6_BUILD:-0}" != "1" ]]; then
    "${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
fi

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

exec ros2 run manyforge_xarm_control lite6_supervised_preflight \
    --owner-id "${OWNER_ID}" \
    --state-topic "${SUPERVISED_ROOT}/state" \
    --execute-service "${SUPERVISED_ROOT}/execute" \
    --command-gate-service "${SUPERVISED_ROOT}/set_command_gate" \
    --shutdown-controller-service "${SUPERVISED_ROOT}/shutdown_controller" \
    --duration-ms "${DURATION_MS}" \
    --sample-period-ms "${SAMPLE_PERIOD_MS}"
