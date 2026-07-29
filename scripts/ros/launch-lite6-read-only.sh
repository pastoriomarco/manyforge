#!/usr/bin/env bash
# Start only the generated model, read-only driver, and observation monitor.

set -euo pipefail

WORKSPACE=${WORKSPACE:-/workspace}
BUILD_ROOT=${MANYFORGE_REAL_LITE6_BUILD_ROOT:-/tmp/manyforge-build/real-lite6}
OUTPUT_ROOT=${MANYFORGE_REAL_LITE6_OUTPUT_ROOT:-${WORKSPACE}/.manyforge-var/real-lite6}
CONNECT=${MANYFORGE_LITE6_CONNECT_READ_ONLY:-OFF}
ROBOT_IP=${MANYFORGE_LITE6_ROBOT_IP:-}
CONFIRMATION=${MANYFORGE_LITE6_READ_ONLY_CONFIRM:-}

case "${CONNECT,,}" in
    on|true|yes|1)
        if [[ "${CONFIRMATION}" != "CONNECT_READ_ONLY" ]]; then
            echo "Set MANYFORGE_LITE6_READ_ONLY_CONFIRM=CONNECT_READ_ONLY to connect." >&2
            exit 2
        fi
        if [[ -z "${ROBOT_IP}" ]]; then
            echo "MANYFORGE_LITE6_ROBOT_IP is required to connect." >&2
            exit 2
        fi
        CONNECT=true
        ;;
    off|false|no|0)
        CONNECT=false
        ROBOT_IP=
        ;;
    *)
        echo "MANYFORGE_LITE6_CONNECT_READ_ONLY must be ON or OFF." >&2
        exit 2
        ;;
esac

"${WORKSPACE}/scripts/ros/generate-lite6-fixed-pneumatic-model.sh"

set +u
source "${BUILD_ROOT}/install/setup.bash"
set -u

LAUNCH_ARGUMENTS=(
    "connect_to_robot:=${CONNECT}"
    "model_urdf_path:=${OUTPUT_ROOT}/model/lite6_fixed_pneumatic.model.urdf"
)
if [[ "${CONNECT}" == true ]]; then
    LAUNCH_ARGUMENTS+=(
        "robot_ip:=${ROBOT_IP}"
    )
fi

exec ros2 launch manyforge_xarm_adapter lite6_real_read_only.launch.py \
    "${LAUNCH_ARGUMENTS[@]}"
