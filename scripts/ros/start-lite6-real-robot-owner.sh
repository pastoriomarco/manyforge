#!/usr/bin/env bash
# Start one observation-only Lite6 ros2_control owner process set.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
OUTPUT_ROOT=${MANYFORGE_REAL_LITE6_OUTPUT_ROOT:?MANYFORGE_REAL_LITE6_OUTPUT_ROOT is required}
RUNTIME_ROOT="${OUTPUT_ROOT}/resolved/lite6-a"
READY_MARKER="${OUTPUT_ROOT}/owner-ready"

rm -f -- "${READY_MARKER}"
"${WORKSPACE}/scripts/ros/build-real-lite6-runtime.sh"
"${WORKSPACE}/scripts/ros/generate-lite6-fixed-pneumatic-model.sh"
"${WORKSPACE}/scripts/ros/resolve-lite6-supervised-runtime.sh"

set +u
source "/opt/ros/${ROS_DISTRO:-jazzy}/setup.bash"
source /opt/manyforge/xarm/install/setup.bash
source "${BUILD_ROOT}/install/setup.bash"
set -u

runtime_urdf="$(<"${RUNTIME_ROOT}/runtime.urdf")"
pids=()
cleanup() {
    local pid
    rm -f -- "${READY_MARKER}"
    for pid in "${pids[@]}"; do
        kill -TERM "${pid}" 2>/dev/null || true
    done
    for pid in "${pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

ros2 run robot_state_publisher robot_state_publisher \
    --ros-args \
    -r __node:=lite6_robot_state_publisher \
    -p robot_description:="${runtime_urdf}" &
pids+=("$!")

ros2 run controller_manager ros2_control_node \
    --ros-args \
    --params-file "${OUTPUT_ROOT}/model/ros2_controllers.yaml" \
    -p hardware_components_initial_state.inactive:="[lite6_system]" \
    -p hardware_components_initial_state.shutdown_on_initial_state_failure:=true &
pids+=("$!")

controller_ready=0
for _attempt in $(seq 1 100); do
    if ros2 service type /controller_manager/list_controllers \
        >/dev/null 2>&1; then
        controller_ready=1
        break
    fi
    sleep 0.1
done
if [[ "${controller_ready}" != "1" ]]; then
    echo "controller_manager did not expose its management services" >&2
    exit 1
fi

# Load both controllers but leave them inactive. The lifecycle provider
# activates/deactivates the pair as one supervised product transition.
ros2 run controller_manager spawner \
    joint_state_broadcaster lite6_traj_controller \
    --inactive \
    --controller-manager /controller_manager \
    --controller-manager-timeout 10

ros2 run manyforge_xarm_adapter \
    manyforge-xarm-validate-live-controller-parameters \
    --runtime-binding "${RUNTIME_ROOT}/runtime-binding.json" \
    --output "${OUTPUT_ROOT}/live-controller-parameter-validation.json" \
    --timeout-s 10

ros2 run manyforge_xarm_adapter \
    manyforge-xarm-run-execution-admission-provider \
    --runtime-binding "${RUNTIME_ROOT}/runtime-binding.json" &
pids+=("$!")

admission_topic="$(
    python3 - "${RUNTIME_ROOT}/runtime-binding.json" <<'PY'
import json
from pathlib import Path
import sys

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
topic = payload.get("execution_admission", {}).get("topic")
if not isinstance(topic, str) or not topic.startswith("/"):
    raise SystemExit("runtime binding lacks an absolute admission topic")
print(topic)
PY
)"
provider_ready=0
for _attempt in $(seq 1 100); do
    if ros2 topic type "${admission_topic}" \
        >/dev/null 2>&1; then
        provider_ready=1
        break
    fi
    sleep 0.1
done
if [[ "${provider_ready}" != "1" ]]; then
    echo "execution-admission provider did not expose its status topic" >&2
    exit 1
fi
touch "${READY_MARKER}"

set +e
wait -n "${pids[@]}"
status=$?
set -e
if [[ "${status}" -eq 0 ]]; then
    # Every managed process is expected to be long-lived.
    status=1
fi
exit "${status}"
