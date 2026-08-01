# shellcheck shell=bash
# Retained launcher resource helpers and typed runtime boundaries.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 4d7e0ecc6a1be6aabd123417e03141f52da6755a.
#
# Retained owner-neutral behavior:
#   native_composer_build_cache_volume_for_image_id
#       immutable image-id to isolated cache-name derivation; and
#   prepare_native_composer_rt_docker_args
#       validated resource-profile to container-argument translation.
#
# Removed responsibility mapping:
#   resolve_native_composer_image
#       -> Manager/deployment image resolution at I4;
#   compose_set_lane, compose_services_for_lane
#       -> Manager-owned process graph at I4 and addressed clients at N/I6;
#   start_vllm_status_watcher, start_composer_dev_container
#       -> owner-issued headless status/process ownership at I4/H0; and
#   stop_known_conflicts, stop_vllm_status_watcher,
#   stop_composer_dev_container
#       -> owner-issued process/container identities and cleanup clients at
#          I4/I5/I9/I10. scripts/launch.sh stop is also unavailable because
#          historical PID files and names cannot authenticate ownership.
#
# No Docker probe, fixed compose topology, process, build, frontend, Python
# runtime, or broad cleanup body remains in this library.

if [[ -n "${_MANYFORGE_LIB_COMPOSE:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_COMPOSE=1

_manyforge_compose_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=realtime-resources.sh
source "${_manyforge_compose_lib_dir}/realtime-resources.sh"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_compose_lib_dir}/composable-cell-runtime-availability.sh"

declare -a NATIVE_COMPOSER_RT_DOCKER_ARGS=()

native_composer_build_cache_volume_for_image_id() {
    local image_id="${1:-}"
    [[ "${image_id}" =~ ^sha256:[0-9a-f]{64}$ ]] || return 1
    printf 'manyforge_native-composer-build-cache-%s\n' "${image_id#sha256:}"
}

prepare_native_composer_rt_docker_args() {
    NATIVE_COMPOSER_RT_DOCKER_ARGS=()
    if ! manyforge_rt_profile_enabled; then
        return 0
    fi
    export MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=false
    if ! manyforge_validate_rt_resource_profile; then
        return 1
    fi
    NATIVE_COMPOSER_RT_DOCKER_ARGS=(
        --cap-add SYS_NICE
        --ulimit rtprio=99:99
        -e "MANYFORGE_RT_RESOURCE_PROFILE=${MANYFORGE_RT_RESOURCE_PROFILE}"
        -e "MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT=${MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT}"
        -e "MANYFORGE_NRT_CPU_AFFINITY=${MANYFORGE_NRT_CPU_AFFINITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY=${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY=${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY}"
        -e "MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY=${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY}"
        -e "MANYFORGE_RT_SCHED_POLICY=${MANYFORGE_RT_SCHED_POLICY}"
        -e "MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY=${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY}"
        -e "MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY=${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}"
        -e "MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY=${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY}"
        -e "MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY=${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}"
        -e "MANYFORGE_RT_STOP_CONTROL_PRIORITY=${MANYFORGE_RT_STOP_CONTROL_PRIORITY}"
        -e "MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY=${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY}"
    )
}

manyforge_fail_launcher_runtime_unavailable() {
    local message
    message="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}:"
    message+=" fixed compose ownership returns with Manager and qualified clients."
    printf '%s\n' \
        "${message}" \
        >&2
    return 78
}

resolve_native_composer_image() {
    manyforge_fail_launcher_runtime_unavailable
}

compose_set_lane() {
    manyforge_fail_launcher_runtime_unavailable
}

compose_services_for_lane() {
    manyforge_fail_launcher_runtime_unavailable
}

stop_known_conflicts() {
    manyforge_fail_launcher_runtime_unavailable
}

start_vllm_status_watcher() {
    manyforge_fail_launcher_runtime_unavailable
}

stop_vllm_status_watcher() {
    manyforge_fail_launcher_runtime_unavailable
}

start_composer_dev_container() {
    manyforge_fail_launcher_runtime_unavailable
}

stop_composer_dev_container() {
    manyforge_fail_launcher_runtime_unavailable
}

unset _manyforge_compose_lib_dir
