#!/usr/bin/env bash
# Start one inactive, sole-owner Lite6 process for attended qualification.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
CONTAINER=${MANYFORGE_LITE6_CONTAINER:-manyforge-lite6-supervised}
IMAGE=${MANYFORGE_REAL_LITE6_IMAGE:-manyforge-ros-jazzy-real-lite6:experimental}
CONFIRMATION=${MANYFORGE_LITE6_SUPERVISED_CONFIRM:-}
ROBOT_IP=${MANYFORGE_LITE6_ROBOT_IP:-}
ROS_DOMAIN=${MANYFORGE_LITE6_ROS_DOMAIN_ID:-${ROS_DOMAIN_ID:-99}}
OWNER_LABEL=org.manyforge.xarm-transport-owner
OWNER_ID=lite6-a:sole-owner
SUPERVISED_CONTRACT_VERSION=1
PROVIDER_INSTANCE_ID=${MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID:-}
CELL_SUPERVISOR_COMPONENT_EPOCH=${MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH:-}

if [[ "${CONFIRMATION}" != "START_SUPERVISED_OWNER_INACTIVE" ]]; then
    echo "Set MANYFORGE_LITE6_SUPERVISED_CONFIRM=START_SUPERVISED_OWNER_INACTIVE" >&2
    exit 2
fi
if [[ -z "${ROBOT_IP}" ]]; then
    echo "MANYFORGE_LITE6_ROBOT_IP must name the attended Lite6 address" >&2
    exit 2
fi
if [[ ! "${ROS_DOMAIN}" =~ ^[0-9]+$ ]] || ((ROS_DOMAIN > 232)); then
    echo "MANYFORGE_LITE6_ROS_DOMAIN_ID must be 0..232" >&2
    exit 2
fi
if [[ -n "$(git -C "${REPO_ROOT}" status --porcelain=v1 --untracked-files=all)" ]]; then
    echo "ManyForge source must be clean before physical qualification" >&2
    exit 2
fi
if [[ "$(docker image inspect "${IMAGE}" --format '{{index .Config.Labels "org.manyforge.experimental.real-lite6"}}' 2>/dev/null || true)" != "true" ]]; then
    echo "Image is not a ManyForge experimental real-Lite6 image: ${IMAGE}" >&2
    exit 2
fi
if [[ "$(docker image inspect "${IMAGE}" --format '{{index .Config.Labels "org.manyforge.xarm.supervised-contract"}}' 2>/dev/null || true)" != "${SUPERVISED_CONTRACT_VERSION}" ]]; then
    echo "Image does not provide xArm supervised contract v${SUPERVISED_CONTRACT_VERSION}: ${IMAGE}" >&2
    echo "Rebuild it with scripts/xarm/build-real-lite6-image.sh." >&2
    exit 2
fi
if docker inspect "${CONTAINER}" >/dev/null 2>&1; then
    echo "Container name already exists: ${CONTAINER}" >&2
    exit 2
fi
if [[ -n "$(docker ps --filter "label=${OWNER_LABEL}=${OWNER_ID}" --format '{{.Names}}')" ]]; then
    echo "A labeled Lite6 transport owner is already running" >&2
    exit 2
fi
if [[ -z "${PROVIDER_INSTANCE_ID}" ]]; then
    if [[ ! -r /proc/sys/kernel/random/uuid ]]; then
        echo "Cannot generate a lifecycle-provider process identity" >&2
        exit 2
    fi
    read -r PROVIDER_SESSION_ID </proc/sys/kernel/random/uuid
    PROVIDER_INSTANCE_ID="lite6-a:provider:${PROVIDER_SESSION_ID}"
fi
if [[ -z "${CELL_SUPERVISOR_COMPONENT_EPOCH}" ]]; then
    read -r CELL_SUPERVISOR_SESSION_ID </proc/sys/kernel/random/uuid
    CELL_SUPERVISOR_COMPONENT_EPOCH="preflight-supervisor-${CELL_SUPERVISOR_SESSION_ID}"
fi

exec docker run --rm --init --interactive --tty \
    --name "${CONTAINER}" \
    --label "${OWNER_LABEL}=${OWNER_ID}" \
    --network host \
    --ipc host \
    -e "ROS_DOMAIN_ID=${ROS_DOMAIN}" \
    -e ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST \
    -e "MANYFORGE_LITE6_ROBOT_IP=${ROBOT_IP}" \
    -e MANYFORGE_LITE6_RUNTIME_BINDING_CONFIRM=RESOLVE_SUPERVISED_RUNTIME \
    -e MANYFORGE_LITE6_ROBOT_INSTANCE_ID=lite6-a \
    -e "MANYFORGE_LITE6_TRANSPORT_OWNER_ID=${OWNER_ID}" \
    -e "MANYFORGE_LITE6_LIFECYCLE_PROVIDER_INSTANCE_ID=${PROVIDER_INSTANCE_ID}" \
    -e MANYFORGE_LITE6_AUTHORITY_PROFILE=local_operator \
    -e MANYFORGE_REAL_ROBOT_CELL_ID=lite6-a \
    -e "MANYFORGE_CELL_SUPERVISOR_COMPONENT_EPOCH=${CELL_SUPERVISOR_COMPONENT_EPOCH}" \
    -e MANYFORGE_CELL_SUPERVISOR_HEARTBEAT_PATH=/tmp/manyforge-real-lite6/cell-supervisor-heartbeat.json \
    -e MANYFORGE_REAL_LITE6_OUTPUT_ROOT=/tmp/manyforge-real-lite6 \
    -v "${REPO_ROOT}:/workspace:ro" \
    -w /workspace \
    "${IMAGE}" \
    bash -lc '
      set -euo pipefail
      ./scripts/ros/build-real-lite6-runtime.sh
      ./scripts/ros/generate-lite6-fixed-pneumatic-model.sh
      ./scripts/ros/resolve-lite6-supervised-runtime.sh
      set +u
      source /opt/ros/jazzy/setup.bash
      source /opt/manyforge/xarm/install/setup.bash
      source /tmp/manyforge-build/real-lite6/install/setup.bash
      set -u
      runtime_urdf="$(</tmp/manyforge-real-lite6/resolved/lite6-a/runtime.urdf)"
      ros2 run robot_state_publisher robot_state_publisher \
        --ros-args \
        -r __node:=lite6_robot_state_publisher \
        -p robot_description:="${runtime_urdf}" &
      state_publisher_pid=$!
      sleep 1
      ros2 run controller_manager ros2_control_node \
        --ros-args \
        --params-file /tmp/manyforge-real-lite6/model/ros2_controllers.yaml \
        -p hardware_components_initial_state.inactive:="[lite6_system]" \
        -p hardware_components_initial_state.shutdown_on_initial_state_failure:=true &
      controller_manager_pid=$!
      ros2 run manyforge_xarm_adapter \
        manyforge-xarm-run-execution-admission-provider \
        --runtime-binding \
        /tmp/manyforge-real-lite6/resolved/lite6-a/runtime-binding.json &
      admission_provider_pid=$!
      cleanup() {
        kill -TERM \
          "${admission_provider_pid}" \
          "${controller_manager_pid}" \
          "${state_publisher_pid}" \
          2>/dev/null || true
        wait "${admission_provider_pid}" 2>/dev/null || true
        wait "${controller_manager_pid}" 2>/dev/null || true
        wait "${state_publisher_pid}" 2>/dev/null || true
      }
      trap cleanup EXIT INT TERM
      set +e
      wait -n \
        "${admission_provider_pid}" \
        "${controller_manager_pid}" \
        "${state_publisher_pid}"
      owner_status=$?
      set -e
      if [[ "${owner_status}" -eq 0 ]]; then
        owner_status=1
      fi
      exit "${owner_status}"
    '
