# shellcheck shell=bash
# Bounded, owner-neutral readiness presentation and polling.
#
# Exact historical source:
#   manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
#   blob 0f3951e1a4ae2f6ab60072b433a7563845dc3596
#
# The callback API remains function-name based: no command string is parsed or
# evaluated. This helper owns only a bounded wait and presentation. It never
# decides what a component is, whether a runtime fact is authoritative, or
# whether cleanup is safe. Those facts return from I4/H0 and lane adapters.

if [[ -n "${_MANYFORGE_LIB_WAIT:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_WAIT=1

_manyforge_wait_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=runner.sh
source "${_manyforge_wait_lib_dir}/runner.sh"
unset _manyforge_wait_lib_dir

readonly _MANYFORGE_WAIT_MAX_TIMEOUT_SECONDS=7200
readonly _MANYFORGE_WAIT_MAX_CALLBACK_SECONDS=30
readonly _MANYFORGE_WAIT_MAX_LOG_SECONDS=2
readonly _MANYFORGE_WAIT_MAX_CALLBACK_BLOCKS=256
readonly _MANYFORGE_WAIT_MAX_TEXT_CHARACTERS=2048

print_truncated() {
    if (( $# != 1 )); then
        return 2
    fi
    local value="${1:0:_MANYFORGE_WAIT_MAX_TEXT_CHARACTERS}"
    local columns="${COLUMNS:-120}"
    if [[ ! "${columns}" =~ ^[0-9]+$ ]] || (( columns < 46 || columns > 512 )); then
        columns=120
    fi
    local maximum=$((columns - 6))
    local LC_ALL=C
    value="$(printf '%s' "${value}" | tr -cd '\11\40-\176')"
    printf '%.*s' "${maximum}" "${value}"
}

_wait_progress_is_tty() {
    if (( $# != 0 )); then
        return 2
    fi
    case "${MANYFORGE_PROGRESS_MODE:-auto}" in
        plain) return 1 ;;
        auto) [[ -t 2 ]] ;;
        *) return 1 ;;
    esac
}

# wait_with_progress LABEL TIMEOUT_S HINT LOG_FN PROBE_FN
#
# PROBE_FN is required; LOG_FN may be empty. Each callback runs in a subshell,
# has a file-output ceiling, and runs in a distinct process group that is
# terminated when its per-call budget ends.
# The whole helper runs in a subshell so traps and temporary state cannot leak
# into its caller. Process ownership and product cleanup are deliberately
# outside this helper; callbacks must not detach into another session/group.
wait_with_progress() (
    if (( $# != 5 )); then
        fail "wait_with_progress expects LABEL TIMEOUT_S HINT LOG_FN PROBE_FN"
        return 2
    fi
    local label="$1"
    local timeout_s="$2"
    local hint="$3"
    local log_fn="$4"
    local probe_fn="$5"
    local LC_ALL=C

    if (( ${#label} == 0 || ${#label} > _MANYFORGE_WAIT_MAX_TEXT_CHARACTERS )) \
        || [[ "${label}" =~ [[:cntrl:]] ]]; then
        fail "wait_with_progress label is empty, too long, or contains control characters"
        return 2
    fi
    if (( ${#hint} > _MANYFORGE_WAIT_MAX_TEXT_CHARACTERS )) \
        || [[ "${hint}" =~ [[:cntrl:]] ]]; then
        fail "wait_with_progress hint is too long or contains control characters"
        return 2
    fi
    if [[ ! "${timeout_s}" =~ ^[0-9]+$ ]] \
        || (( timeout_s < 1 || timeout_s > _MANYFORGE_WAIT_MAX_TIMEOUT_SECONDS )); then
        fail "wait_with_progress timeout must be 1..${_MANYFORGE_WAIT_MAX_TIMEOUT_SECONDS} seconds"
        return 2
    fi
    if [[ ! "${probe_fn}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] \
        || ! declare -F "${probe_fn}" >/dev/null 2>&1; then
        fail "wait_with_progress probe function is missing or invalid"
        return 2
    fi
    if [[ -n "${log_fn}" ]] \
        && { [[ ! "${log_fn}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]] \
            || ! declare -F "${log_fn}" >/dev/null 2>&1; }; then
        fail "wait_with_progress log function is invalid"
        return 2
    fi
    case "${MANYFORGE_PROGRESS_MODE:-auto}" in
        auto | plain) ;;
        *)
            fail "MANYFORGE_PROGRESS_MODE must be auto or plain"
            return 2
            ;;
    esac
    local required_tool
    for required_tool in mktemp ps rm sleep tail tr; do
        if ! command -v "${required_tool}" >/dev/null 2>&1; then
            fail "wait_with_progress requires ${required_tool}"
            return 2
        fi
    done

    local temporary_root="${TMPDIR:-/tmp}"
    if (( ${#temporary_root} == 0 || ${#temporary_root} > 1024 )) \
        || [[ ! -d "${temporary_root}" || -L "${temporary_root}" ]]; then
        fail "wait_with_progress temporary directory is invalid"
        return 2
    fi
    local temporary_directory=""
    temporary_directory="$(mktemp -d "${temporary_root%/}/manyforge-wait.XXXXXXXX")" \
        || { fail "wait_with_progress could not create private temporary state"; return 2; }
    local probe_output_file="${temporary_directory}/probe-output"
    local log_output_file="${temporary_directory}/log-output"
    : >"${probe_output_file}" || { rm -rf -- "${temporary_directory}"; return 2; }
    : >"${log_output_file}" || { rm -rf -- "${temporary_directory}"; return 2; }

    # Job control gives each background callback subshell a distinct process
    # group while preserving direct access to the caller's functions and
    # dynamically scoped shell variables. This is isolated inside the outer
    # wait subshell and cannot change the caller's job-control mode.
    set -m || {
        rm -rf -- "${temporary_directory}"
        fail "wait_with_progress cannot isolate callback process groups"
        return 2
    }

    local _manyforge_wait_active_pid=""
    local _manyforge_wait_active_pgid=""
    local _manyforge_wait_active_gate=""
    _manyforge_wait_terminate_group() {
        local process_id="$1"
        local process_group="$2"
        if [[ "${process_group}" =~ ^[1-9][0-9]*$ ]]; then
            builtin kill -TERM -- "-${process_group}" 2>/dev/null || true
            local attempt
            for ((attempt = 0; attempt < 20; attempt++)); do
                builtin kill -0 -- "-${process_group}" 2>/dev/null || break
                sleep 0.05
            done
            builtin kill -KILL -- "-${process_group}" 2>/dev/null || true
        elif [[ "${process_id}" =~ ^[1-9][0-9]*$ ]]; then
            builtin kill -KILL -- "${process_id}" 2>/dev/null || true
        fi
        if [[ "${process_id}" =~ ^[1-9][0-9]*$ ]]; then
            builtin wait "${process_id}" 2>/dev/null || true
        fi
    }
    _manyforge_wait_cleanup() {
        if [[ "${_manyforge_wait_active_pid}" =~ ^[1-9][0-9]*$ ]]; then
            _manyforge_wait_terminate_group \
                "${_manyforge_wait_active_pid}" \
                "${_manyforge_wait_active_pgid}"
        fi
        rm -f -- "${_manyforge_wait_active_gate}" 2>/dev/null || true
        rm -rf -- "${temporary_directory}"
    }
    trap _manyforge_wait_cleanup EXIT
    trap 'exit 130' HUP INT TERM

    _manyforge_wait_invoke() {
        local callback="$1"
        local output_file="$2"
        local budget="$3"
        : > "${output_file}" || return 125
        _manyforge_wait_active_gate="$(
            mktemp "${temporary_directory}/callback-gate.XXXXXXXX"
        )" || return 125
        (
            while [[ -e "${_manyforge_wait_active_gate}" ]]; do
                sleep 0.01
            done
            ulimit -f "${_MANYFORGE_WAIT_MAX_CALLBACK_BLOCKS}" >/dev/null 2>&1 \
                || exit 125
            "${callback}"
        ) >"${output_file}" 2>&1 &
        _manyforge_wait_active_pid=$!
        local observed_pgid=""
        observed_pgid="$(
            command ps -o pgid= -p "${_manyforge_wait_active_pid}" 2>/dev/null \
                | command tr -d '[:space:]'
        )"
        if [[ ! "${observed_pgid}" =~ ^[1-9][0-9]*$ ]] \
            || [[ "${observed_pgid}" != "${_manyforge_wait_active_pid}" ]]; then
            _manyforge_wait_terminate_group "${_manyforge_wait_active_pid}" ""
            _manyforge_wait_active_pid=""
            _manyforge_wait_active_pgid=""
            rm -f -- "${_manyforge_wait_active_gate}"
            _manyforge_wait_active_gate=""
            return 125
        fi
        _manyforge_wait_active_pgid="${observed_pgid}"
        rm -f -- "${_manyforge_wait_active_gate}" || {
            _manyforge_wait_terminate_group \
                "${_manyforge_wait_active_pid}" \
                "${_manyforge_wait_active_pgid}"
            _manyforge_wait_active_pid=""
            _manyforge_wait_active_pgid=""
            _manyforge_wait_active_gate=""
            return 125
        }
        _manyforge_wait_active_gate=""

        local callback_start=${SECONDS}
        while [[ "$(builtin jobs -pr)" == "${_manyforge_wait_active_pid}" ]]; do
            if (( SECONDS - callback_start >= budget )); then
                _manyforge_wait_terminate_group \
                    "${_manyforge_wait_active_pid}" \
                    "${_manyforge_wait_active_pgid}"
                _manyforge_wait_active_pid=""
                _manyforge_wait_active_pgid=""
                return 124
            fi
            sleep 0.1
        done
        local callback_status=0
        builtin wait "${_manyforge_wait_active_pid}" || callback_status=$?
        if builtin kill -0 -- "-${_manyforge_wait_active_pgid}" 2>/dev/null; then
            _manyforge_wait_terminate_group "" "${_manyforge_wait_active_pgid}"
            callback_status=125
        fi
        _manyforge_wait_active_pid=""
        _manyforge_wait_active_pgid=""
        return "${callback_status}"
    }

    _manyforge_wait_log_snippet() {
        local snippet=""
        [[ -n "${log_fn}" ]] || return 0
        if _manyforge_wait_invoke \
            "${log_fn}" "${log_output_file}" "${_MANYFORGE_WAIT_MAX_LOG_SECONDS}"; then
            snippet="$(tail -n 1 -- "${log_output_file}")"
        else
            snippet="[log callback failed or exceeded its budget]"
        fi
        print_truncated "${snippet}"
    }

    local start=${SECONDS}
    local deadline=$((start + timeout_s))
    local next_log_at=0
    local progress_line_drawn=0
    local elapsed=0

    _manyforge_wait_end_progress_line() {
        if _wait_progress_is_tty && (( progress_line_drawn > 0 )); then
            printf '\r\033[K' >&2
            progress_line_drawn=0
        fi
    }

    _manyforge_wait_render() {
        local current_elapsed="$1"
        if _wait_progress_is_tty; then
            printf '\r\033[K  elapsed %ds (timeout %ds)' \
                "${current_elapsed}" "${timeout_s}" >&2
            if [[ -n "${log_fn}" ]]; then
                printf ' — ' >&2
                _manyforge_wait_log_snippet >&2
            fi
            progress_line_drawn=1
            return 0
        fi
        if (( current_elapsed < next_log_at )); then
            return 0
        fi
        printf '  …still waiting (%ds elapsed, timeout %ds)' \
            "${current_elapsed}" "${timeout_s}" >&2
        if [[ -n "${log_fn}" ]]; then
            printf ' — ' >&2
            _manyforge_wait_log_snippet >&2
        fi
        printf '\n' >&2
        next_log_at=$((current_elapsed + 30))
    }

    _manyforge_wait_dump_failure() {
        _manyforge_wait_end_progress_line
        elapsed=$((SECONDS - start))
        fail "${label} — timed out after ${elapsed}s (limit ${timeout_s}s)"
        if [[ -n "${hint}" ]]; then
            info "${hint}"
        fi
        if [[ -s "${probe_output_file}" ]]; then
            info "last probe (${probe_fn}) stdout/stderr:"
            tail -n 40 -- "${probe_output_file}" | while IFS= read -r line; do
                printf '    ' >&2
                print_truncated "${line}" >&2
                printf '\n' >&2
            done
        fi
        if [[ -n "${log_fn}" ]]; then
            info "last bounded log sample from ${log_fn}:"
            if _manyforge_wait_invoke \
                "${log_fn}" "${log_output_file}" "${_MANYFORGE_WAIT_MAX_LOG_SECONDS}"; then
                tail -n 40 -- "${log_output_file}" | while IFS= read -r line; do
                    printf '    ' >&2
                    print_truncated "${line}" >&2
                    printf '\n' >&2
                done
            else
                info "log callback failed or exceeded its budget"
            fi
        fi
    }

    printf '%s\n' "${label}" >&2
    if [[ -n "${hint}" ]]; then
        printf '  %s\n' "${hint}" >&2
    fi

    while true; do
        local remaining=$((deadline - SECONDS))
        if (( remaining <= 0 )); then
            _manyforge_wait_dump_failure
            return 1
        fi
        local callback_budget="${_MANYFORGE_WAIT_MAX_CALLBACK_SECONDS}"
        if (( callback_budget > remaining )); then
            callback_budget="${remaining}"
        fi
        if _manyforge_wait_invoke \
            "${probe_fn}" "${probe_output_file}" "${callback_budget}"; then
            elapsed=$((SECONDS - start))
            _manyforge_wait_end_progress_line
            pass "${label} — ready after ${elapsed}s"
            return 0
        fi
        if (( SECONDS >= deadline )); then
            _manyforge_wait_dump_failure
            return 1
        fi
        elapsed=$((SECONDS - start))
        _manyforge_wait_render "${elapsed}"
        sleep 1
    done
)
