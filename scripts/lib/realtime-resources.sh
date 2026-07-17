# shellcheck shell=bash
# Opt-in target resource profile shared by native and Isaac/ROS launchers.
#
# This is startup/admission plumbing, never a runtime control path. The strict
# profile partitions ManyForge/ROS application threads before they are created
# and causes malformed or overlapping assignments to fail before a robot
# runtime starts. Linux still makes the final policy/affinity decision; native
# workers and controller_manager independently fail closed when those requests
# are rejected. This does not by itself claim host-wide CPU/IRQ isolation or a
# PREEMPT_RT kernel; the qualification probe records those separately.

if [[ -n "${_MANYFORGE_LIB_REALTIME_RESOURCES:-}" ]]; then return 0; fi
_MANYFORGE_LIB_REALTIME_RESOURCES=1
_MANYFORGE_MAX_CPU_INDEX=1023

manyforge_rt_profile_enabled() {
    [[ "${MANYFORGE_RT_RESOURCE_PROFILE:-off}" == "strict" ]]
}

_manyforge_expand_cpu_set() {
    local value="$1"
    local label="$2"
    local token first last cpu
    local -a expanded=()
    IFS=',' read -r -a tokens <<<"${value}"
    for token in "${tokens[@]}"; do
        token="${token//[[:space:]]/}"
        if [[ "${token}" =~ ^[0-9]+$ ]]; then
            if ((${#token} > 4)); then
                printf '%s contains an out-of-range CPU token: %s\n' \
                    "${label}" "${token}" >&2
                return 2
            fi
            cpu="$((10#${token}))"
            if ((cpu > _MANYFORGE_MAX_CPU_INDEX)); then
                printf '%s selects CPU %s above the supported maximum %s\n' \
                    "${label}" "${cpu}" "${_MANYFORGE_MAX_CPU_INDEX}" >&2
                return 2
            fi
            expanded+=("${cpu}")
        elif [[ "${token}" =~ ^([0-9]+)-([0-9]+)$ ]]; then
            if ((${#BASH_REMATCH[1]} > 4 || ${#BASH_REMATCH[2]} > 4)); then
                printf '%s contains an out-of-range CPU token: %s\n' \
                    "${label}" "${token}" >&2
                return 2
            fi
            first="$((10#${BASH_REMATCH[1]}))"
            last="$((10#${BASH_REMATCH[2]}))"
            if (( last < first )); then
                printf '%s contains a descending CPU range: %s\n' "${label}" "${token}" >&2
                return 2
            fi
            if ((last > _MANYFORGE_MAX_CPU_INDEX)); then
                printf '%s selects CPU %s above the supported maximum %s\n' \
                    "${label}" "${last}" "${_MANYFORGE_MAX_CPU_INDEX}" >&2
                return 2
            fi
            for ((cpu = first; cpu <= last; ++cpu)); do
                expanded+=("${cpu}")
            done
        else
            printf '%s contains an invalid CPU token: %s\n' "${label}" "${token:-<empty>}" >&2
            return 2
        fi
    done
    if ((${#expanded[@]} == 0)); then
        printf '%s must select at least one CPU\n' "${label}" >&2
        return 2
    fi
    printf '%s\n' "${expanded[@]}" | sort -n -u
}

_manyforge_cpu_sets_overlap() {
    local left="$1"
    local right="$2"
    local left_cpu right_cpu
    while IFS= read -r left_cpu; do
        while IFS= read -r right_cpu; do
            if [[ "${left_cpu}" == "${right_cpu}" ]]; then
                return 0
            fi
        done < <(_manyforge_expand_cpu_set "${right}" "CPU set")
    done < <(_manyforge_expand_cpu_set "${left}" "CPU set")
    return 1
}

_manyforge_require_disjoint_cpu_sets() {
    local left_name="$1"
    local left="$2"
    local right_name="$3"
    local right="$4"
    if _manyforge_cpu_sets_overlap "${left}" "${right}"; then
        printf '%s (%s) must be disjoint from %s (%s)\n' \
            "${left_name}" "${left}" "${right_name}" "${right}" >&2
        return 2
    fi
}

_manyforge_validate_priority() {
    local name="$1"
    local value="$2"
    if [[ ! "${value}" =~ ^[0-9]+$ ]] || ((${#value} > 2)) || \
       ((10#${value} < 1 || 10#${value} > 99)); then
        printf '%s must be an integer in [1, 99]\n' "${name}" >&2
        return 2
    fi
}

manyforge_validate_rt_resource_profile() {
    local profile="${MANYFORGE_RT_RESOURCE_PROFILE:-off}"
    local controller_manager_present="${MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT:-true}"
    if [[ "${profile}" == "off" || -z "${profile}" ]]; then
        return 0
    fi
    if [[ "${profile}" != "strict" ]]; then
        printf 'MANYFORGE_RT_RESOURCE_PROFILE must be off or strict\n' >&2
        return 2
    fi

    case "${controller_manager_present}" in
        true|false) ;;
        *)
            printf 'MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT must be true or false\n' >&2
            return 2
            ;;
    esac
    export MANYFORGE_RT_CONTROLLER_MANAGER_PRESENT="${controller_manager_present}"

    : "${MANYFORGE_NRT_CPU_AFFINITY:?strict RT profile requires MANYFORGE_NRT_CPU_AFFINITY}"
    if [[ "${controller_manager_present}" == "true" ]]; then
        : "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY:?strict RT profile requires MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}"
    fi
    : "${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY:?strict RT profile requires MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}"
    : "${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY:?strict RT profile requires MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}"

    export MANYFORGE_RT_SCHED_POLICY="${MANYFORGE_RT_SCHED_POLICY:-FIFO}"
    export MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY="${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY:-50}"
    export MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY="${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY:-45}"
    export MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY="${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY:-40}"
    export MANYFORGE_RT_STOP_CONTROL_PRIORITY="${MANYFORGE_RT_STOP_CONTROL_PRIORITY:-35}"
    export MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY="${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY:-${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}}"
    export MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY="${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY:-true}"

    case "${MANYFORGE_RT_SCHED_POLICY}" in
        FIFO|SCHED_FIFO|RR|SCHED_RR) ;;
        *)
            printf 'MANYFORGE_RT_SCHED_POLICY must be FIFO or RR\n' >&2
            return 2
            ;;
    esac
    case "${MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY}" in
        true|false) ;;
        *)
            printf 'MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY must be true or false\n' >&2
            return 2
            ;;
    esac

    _manyforge_validate_priority MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY "${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY}" || return $?
    _manyforge_validate_priority MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY "${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY}" || return $?
    _manyforge_validate_priority MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY "${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY}" || return $?
    _manyforge_validate_priority MANYFORGE_RT_STOP_CONTROL_PRIORITY "${MANYFORGE_RT_STOP_CONTROL_PRIORITY}" || return $?
    if ! ((
        10#${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY} > 10#${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY} &&
        10#${MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY} > 10#${MANYFORGE_RT_STOP_CONTROL_PRIORITY}
    )); then
        if [[ "${controller_manager_present}" == "true" ]]; then
            printf 'RT priorities must satisfy controller_manager > controller_command > runtime_safety > stop_control\n' >&2
        else
            printf 'RT priorities must satisfy controller_command > runtime_safety > stop_control\n' >&2
        fi
        return 2
    fi
    if [[ "${controller_manager_present}" == "true" ]] && ! ((
        10#${MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY} > 10#${MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY}
    )); then
        printf 'RT priorities must satisfy controller_manager > controller_command > runtime_safety > stop_control\n' >&2
        return 2
    fi

    local cpu_variable cpu_value
    local -a cpu_variables=(
        MANYFORGE_NRT_CPU_AFFINITY \
        MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY \
        MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY \
        MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY
    )
    if [[ "${controller_manager_present}" == "true" ]]; then
        cpu_variables+=(MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY)
    fi
    for cpu_variable in "${cpu_variables[@]}"; do
        cpu_value="${!cpu_variable}"
        _manyforge_expand_cpu_set "${cpu_value}" "${cpu_variable}" >/dev/null || return $?
        if ! taskset --cpu-list "${cpu_value}" true >/dev/null 2>&1; then
            printf '%s selects a CPU unavailable to this launch context: %s\n' \
                "${cpu_variable}" "${cpu_value}" >&2
            return 2
        fi
    done

    # NRT work must not share any reserved RT capacity. Controller-manager,
    # ManyForge command, and RuntimeSafety are also pairwise isolated. The rare
    # StopControl worker may deliberately share ControllerCommand capacity on a
    # constrained target, but never RuntimeSafety or controller_manager.
    _manyforge_require_disjoint_cpu_sets NRT "${MANYFORGE_NRT_CPU_AFFINITY}" controller_command "${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}" || return $?
    _manyforge_require_disjoint_cpu_sets NRT "${MANYFORGE_NRT_CPU_AFFINITY}" runtime_safety "${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}" || return $?
    _manyforge_require_disjoint_cpu_sets NRT "${MANYFORGE_NRT_CPU_AFFINITY}" stop_control "${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY}" || return $?
    if [[ "${controller_manager_present}" == "true" ]]; then
        _manyforge_require_disjoint_cpu_sets NRT "${MANYFORGE_NRT_CPU_AFFINITY}" controller_manager "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}" || return $?
        _manyforge_require_disjoint_cpu_sets controller_manager "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}" controller_command "${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}" || return $?
        _manyforge_require_disjoint_cpu_sets controller_manager "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}" runtime_safety "${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}" || return $?
        _manyforge_require_disjoint_cpu_sets controller_manager "${MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY}" stop_control "${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY}" || return $?
    fi
    _manyforge_require_disjoint_cpu_sets controller_command "${MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY}" runtime_safety "${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}" || return $?
    _manyforge_require_disjoint_cpu_sets runtime_safety "${MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY}" stop_control "${MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY}" || return $?
}

manyforge_exec_with_nrt_affinity() {
    if manyforge_rt_profile_enabled; then
        manyforge_validate_rt_resource_profile
        exec taskset --cpu-list "${MANYFORGE_NRT_CPU_AFFINITY}" "$@"
    fi
    exec "$@"
}
