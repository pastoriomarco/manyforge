# shellcheck shell=bash
# Retained Isaac cache identity helper and typed product boundaries.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob cfe20a23a5a26d13f09cff879c11bb0ffc718c65.
#
# isaac_build_cache_volume_for_image_id remains a pure immutable-image cache
# derivation. Historical image pinning/builds move to deployment/Manager
# ownership at I3/I4. ROS shell execution, fixed topics/actions, readiness
# waits, logs, compose startup, and smoke orchestration return only with the
# addressed Isaac adapter at I/I9. Product cleanup is also unavailable:
# scripts/launch.sh cannot authenticate historical PID/container names and
# future cleanup must consume owner-issued identities at I4/I5/I9.

if [[ -n "${_MANYFORGE_LIB_ISAAC:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_ISAAC=1

_manyforge_isaac_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_isaac_lib_dir}/composable-cell-runtime-availability.sh"

ISAAC_RUNTIME_IMAGE_ID=""
ISAAC_BUILD_CACHE_VOLUME=""

isaac_build_cache_volume_for_image_id() {
    local image_id="${1:-}"
    [[ "${image_id}" =~ ^sha256:[0-9a-f]{64}$ ]] || return 1
    printf 'manyforge_ros-full-build-cache-%s\n' "${image_id#sha256:}"
}

manyforge_fail_isaac_runtime_unavailable() {
    local message
    message="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}:"
    message+=" Isaac launch returns with the addressed adapter at I/I9."
    printf '%s\n' \
        "${message}" \
        >&2
    return 78
}

pin_isaac_runtime_image() {
    manyforge_fail_isaac_runtime_unavailable
}

ros_shell() {
    manyforge_fail_isaac_runtime_unavailable
}

build_ros2_control_runtime() {
    manyforge_fail_isaac_runtime_unavailable
}

isaac_topic_control_logs() {
    manyforge_fail_isaac_runtime_unavailable
}

isaac_joint_states_ready() {
    manyforge_fail_isaac_runtime_unavailable
}

arm_action_ready() {
    manyforge_fail_isaac_runtime_unavailable
}

gripper_action_ready() {
    manyforge_fail_isaac_runtime_unavailable
}

arm_controller_state_ready() {
    manyforge_fail_isaac_runtime_unavailable
}

wait_for_isaac_joint_states() {
    manyforge_fail_isaac_runtime_unavailable
}

wait_for_arm_action() {
    manyforge_fail_isaac_runtime_unavailable
}

wait_for_gripper_action() {
    manyforge_fail_isaac_runtime_unavailable
}

wait_for_arm_controller_state() {
    manyforge_fail_isaac_runtime_unavailable
}

isaac_image_present() {
    manyforge_fail_isaac_runtime_unavailable
}

isaac_image_runtime_ready() {
    manyforge_fail_isaac_runtime_unavailable
}

maybe_build_isaac_image() {
    manyforge_fail_isaac_runtime_unavailable
}

start_isaac_services() {
    manyforge_fail_isaac_runtime_unavailable
}

stop_isaac_services() {
    manyforge_fail_isaac_runtime_unavailable
}

unset _manyforge_isaac_lib_dir
