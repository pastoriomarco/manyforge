#!/usr/bin/env bash
# Resolve one exact, manifest-verified supervised Lite6 runtime artifact.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
OUTPUT_ROOT=${MANYFORGE_REAL_LITE6_OUTPUT_ROOT:-${WORKSPACE}/.manyforge-var/real-lite6}
ROBOT_IP=${MANYFORGE_LITE6_ROBOT_IP:-}
ROBOT_INSTANCE_ID=${MANYFORGE_LITE6_ROBOT_INSTANCE_ID:-lite6-a}
TRANSPORT_OWNER_ID=${MANYFORGE_LITE6_TRANSPORT_OWNER_ID:-${ROBOT_INSTANCE_ID}:sole-owner}
LIFECYCLE_PROVIDER_INSTANCE_ID=${MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID:-}
AUTHORITY_PROFILE=${MANYFORGE_LITE6_AUTHORITY_PROFILE:-local_operator}
CELL_ID=${MANYFORGE_REAL_ROBOT_CELL_ID:-lite6-a}
CELL_SUPERVISOR_COMPONENT_EPOCH=${MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH:-}
CELL_SUPERVISOR_HEARTBEAT_PATH=${MANYFORGE_CELL_SUPERVISOR_HEARTBEAT_PATH:-${OUTPUT_ROOT}/cell-supervisor-heartbeat.json}
CELL_PATH=${MANYFORGE_REAL_LITE6_CELL_PATH:-${WORKSPACE}/examples/lite6_fixed_pneumatic_commissioning.real-robot-cell.yaml}
DEPLOYMENT_TEMPLATE=${MANYFORGE_REAL_LITE6_DEPLOYMENT_TEMPLATE:-${WORKSPACE}/examples/lite6_fixed_pneumatic_commissioning.deployment.template.yaml}
PROGRAM_PATH=${MANYFORGE_REAL_LITE6_PROGRAM_PATH:-${WORKSPACE}/examples/lite6_fixed_pneumatic_commissioning.program.yaml}
CONFIRMATION=${MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM:-}

if [[ -z "${ROBOT_IP}" ]]; then
    echo "MANYFORGE_LITE6_ROBOT_IP is required" >&2
    exit 2
fi
if [[ "${CONFIRMATION}" != "RESOLVE_SUPERVISED_RUNTIME" ]]; then
    echo "Set MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM=RESOLVE_SUPERVISED_RUNTIME" >&2
    exit 2
fi
if [[ -z "${LIFECYCLE_PROVIDER_INSTANCE_ID}" ]]; then
    echo "MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID is required" >&2
    exit 2
fi
if [[ -z "${CELL_SUPERVISOR_COMPONENT_EPOCH}" ]]; then
    echo "MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH is required" >&2
    exit 2
fi

MANYFORGE_REAL_LITE6_OUTPUT_ROOT="${OUTPUT_ROOT}" \
    "${WORKSPACE}/scripts/ros/generate-lite6-fixed-pneumatic-model.sh"

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

RUNTIME_ROOT="${OUTPUT_ROOT}/resolved/${ROBOT_INSTANCE_ID}"
ros2 run manyforge_xarm_adapter manyforge-xarm-resolve-supervised-runtime \
    --binding-manifest "${OUTPUT_ROOT}/binding-manifest.json" \
    --model-root "${OUTPUT_ROOT}/model" \
    --robot-ip "${ROBOT_IP}" \
    --robot-instance-id "${ROBOT_INSTANCE_ID}" \
    --transport-owner-id "${TRANSPORT_OWNER_ID}" \
    --lifecycle-provider-instance-id "${LIFECYCLE_PROVIDER_INSTANCE_ID}" \
    --authority-profile "${AUTHORITY_PROFILE}" \
    --cell-id "${CELL_ID}" \
    --cell-supervisor-component-epoch "${CELL_SUPERVISOR_COMPONENT_EPOCH}" \
    --cell-supervisor-heartbeat-path "${CELL_SUPERVISOR_HEARTBEAT_PATH}" \
    --output-urdf "${RUNTIME_ROOT}/runtime.urdf" \
    --output-manifest "${RUNTIME_ROOT}/runtime-binding.json"

ros2 run manyforge_xarm_adapter manyforge-xarm-resolve-real-robot-deployment \
    --cell "${CELL_PATH}" \
    --deployment-template "${DEPLOYMENT_TEMPLATE}" \
    --program "${PROGRAM_PATH}" \
    --model-root "${OUTPUT_ROOT}/model" \
    --runtime-binding "${RUNTIME_ROOT}/runtime-binding.json" \
    --output-deployment "${RUNTIME_ROOT}/commissioning.deployment.yaml" \
    --output-record "${RUNTIME_ROOT}/runtime-record.json"

echo "Resolved supervised Lite6 runtime and deployment under ${RUNTIME_ROOT}."
