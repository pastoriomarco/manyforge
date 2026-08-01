# shellcheck shell=bash
# Retained real-robot cache identity helper and typed product boundaries.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 62da0b3f83d60750195e7d2154732a46f34262f2.
#
# real_robot_build_cache_volume_for_image_id remains an owner-neutral immutable
# image cache derivation. Historical resolver validation, image-label checks,
# fixed Lite6 identity, Python cell-supervisor ownership, Docker startup,
# Composer runtime-status coupling, admission/arming, logs, and fallback
# teardown return only through the Manager/lifecycle/real-robot gates
# I4, H0/I5, L0/I7, P5/I8, and R/I10.
#
# Product cleanup is also unavailable. scripts/launch.sh cannot authenticate
# historical PID/container names; future cleanup must consume owner-issued
# identities at I4/I5/I10. This library retains no hardware, network,
# filesystem, process, or Docker body.

if [[ -n "${_MANYFORGE_LIB_REAL_ROBOT:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_REAL_ROBOT=1

_manyforge_real_robot_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_real_robot_lib_dir}/composable-cell-runtime-availability.sh"

REAL_ROBOT_BUILD_CACHE_VOLUME=""

real_robot_build_cache_volume_for_image_id() {
    local image_id="${1:-}"
    [[ "${image_id}" =~ ^sha256:[0-9a-f]{64}$ ]] || return 1
    printf 'manyforge_real-lite6-build-cache-%s\n' "${image_id#sha256:}"
}

manyforge_fail_real_robot_runtime_unavailable() {
    local message
    message="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}:"
    message+=" real-robot launch returns after lifecycle, Stop, and R/I10."
    printf '%s\n' \
        "${message}" \
        >&2
    return 78
}

real_robot_preflight() {
    manyforge_fail_real_robot_runtime_unavailable
}

start_real_robot_lane() {
    manyforge_fail_real_robot_runtime_unavailable
}

stop_real_robot_lane() {
    manyforge_fail_real_robot_runtime_unavailable
}

real_robot_logs() {
    manyforge_fail_real_robot_runtime_unavailable
}

unset _manyforge_real_robot_lib_dir
