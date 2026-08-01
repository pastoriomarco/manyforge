# shellcheck shell=bash
# Bounded, owner-neutral operator presentation helpers.
#
# Exact historical source:
#   manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
#   blob d31708e592aa2cece604ccfca9bd37e7b64e2ac8
#
# Retained responsibility: render short step/info/success/warning/failure
# messages to stderr without interpreting their contents.
#
# Removed responsibility: the historical run/run_capture helpers executed
# arbitrary shell strings (and run_capture swallowed failures); the ERR trap
# inferred launcher step/process state and tailed an arbitrary path. Runtime
# execution, recovery, and owner-issued status return through I4/H0/N and the
# lane-specific I9/I10 gates. This file is presentation only.

if [[ -n "${_MANYFORGE_LIB_RUNNER:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_RUNNER=1

readonly _MANYFORGE_PRESENT_MAX_ARGUMENTS=8
readonly _MANYFORGE_PRESENT_MAX_CHARACTERS=4096

if [[ -t 2 ]]; then
    readonly _MANYFORGE_COLOR_RESET=$'\033[0m'
    readonly _MANYFORGE_COLOR_BOLD=$'\033[1m'
    readonly _MANYFORGE_COLOR_BLUE=$'\033[34m'
    readonly _MANYFORGE_COLOR_GREEN=$'\033[32m'
    readonly _MANYFORGE_COLOR_YELLOW=$'\033[33m'
    readonly _MANYFORGE_COLOR_RED=$'\033[31m'
    readonly _MANYFORGE_COLOR_DIM=$'\033[2m'
else
    readonly _MANYFORGE_COLOR_RESET=""
    readonly _MANYFORGE_COLOR_BOLD=""
    readonly _MANYFORGE_COLOR_BLUE=""
    readonly _MANYFORGE_COLOR_GREEN=""
    readonly _MANYFORGE_COLOR_YELLOW=""
    readonly _MANYFORGE_COLOR_RED=""
    readonly _MANYFORGE_COLOR_DIM=""
fi

_manyforge_present() {
    if (( $# < 2 || $# > _MANYFORGE_PRESENT_MAX_ARGUMENTS + 1 )); then
        printf 'manyforge presentation error: expected 1..%d message arguments\n' \
            "${_MANYFORGE_PRESENT_MAX_ARGUMENTS}" >&2
        return 2
    fi
    local kind="$1"
    shift

    local IFS=' '
    local message="$*"
    local LC_ALL=C
    if (( ${#message} > _MANYFORGE_PRESENT_MAX_CHARACTERS )); then
        printf 'manyforge presentation error: message exceeds %d characters\n' \
            "${_MANYFORGE_PRESENT_MAX_CHARACTERS}" >&2
        return 2
    fi
    if [[ "${message}" =~ [[:cntrl:]] ]]; then
        printf 'manyforge presentation error: message contains control characters\n' >&2
        return 2
    fi

    case "${kind}" in
        step)
            printf '\n%s==> %s%s\n' \
                "${_MANYFORGE_COLOR_BLUE}${_MANYFORGE_COLOR_BOLD}" \
                "${message}" "${_MANYFORGE_COLOR_RESET}" >&2
            ;;
        info)
            printf '    %s%s%s\n' \
                "${_MANYFORGE_COLOR_DIM}" "${message}" \
                "${_MANYFORGE_COLOR_RESET}" >&2
            ;;
        pass)
            printf '%s  ✓ %s%s\n' \
                "${_MANYFORGE_COLOR_GREEN}" "${message}" \
                "${_MANYFORGE_COLOR_RESET}" >&2
            ;;
        warn)
            printf '%s  ! %s%s\n' \
                "${_MANYFORGE_COLOR_YELLOW}" "${message}" \
                "${_MANYFORGE_COLOR_RESET}" >&2
            ;;
        fail)
            printf '%s  ✗ %s%s\n' \
                "${_MANYFORGE_COLOR_RED}" "${message}" \
                "${_MANYFORGE_COLOR_RESET}" >&2
            ;;
        *)
            printf 'manyforge presentation error: unknown message kind\n' >&2
            return 2
            ;;
    esac
}

step() {
    _manyforge_present step "$@"
}

info() {
    _manyforge_present info "$@"
}

pass() {
    _manyforge_present pass "$@"
}

warn() {
    _manyforge_present warn "$@"
}

fail() {
    _manyforge_present fail "$@"
}
