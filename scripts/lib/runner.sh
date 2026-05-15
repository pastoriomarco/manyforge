# shellcheck shell=bash
# scripts/lib/runner.sh — primitive output + command helpers.
#
# Sourced by every other lib module and by launch.sh. Defines:
#   step / info / pass / warn / fail / run / run_capture
#
# Convention: helpers print to stderr unless they're producing data the
# caller intends to capture. step/info/pass/warn/fail are operator
# messages and go to stderr so they don't pollute stdout pipelines.

if [[ -n "${_MANYFORGE_LIB_RUNNER:-}" ]]; then return 0; fi
_MANYFORGE_LIB_RUNNER=1

if [[ -t 2 ]]; then
    _C_RESET=$'\033[0m'
    _C_BOLD=$'\033[1m'
    _C_BLUE=$'\033[34m'
    _C_GREEN=$'\033[32m'
    _C_YELLOW=$'\033[33m'
    _C_RED=$'\033[31m'
    _C_DIM=$'\033[2m'
else
    _C_RESET=""
    _C_BOLD=""
    _C_BLUE=""
    _C_GREEN=""
    _C_YELLOW=""
    _C_RED=""
    _C_DIM=""
fi

# step "[3/7] Heading text"  — section banner.
step() {
    printf '\n%s==> %s%s\n' "${_C_BLUE}${_C_BOLD}" "$*" "${_C_RESET}" >&2
}

# info "informational message"
info() {
    printf '    %s%s%s\n' "${_C_DIM}" "$*" "${_C_RESET}" >&2
}

# pass "✓ ready"
pass() {
    printf '%s  ✓ %s%s\n' "${_C_GREEN}" "$*" "${_C_RESET}" >&2
}

# warn "non-fatal warning"
warn() {
    printf '%s  ! %s%s\n' "${_C_YELLOW}" "$*" "${_C_RESET}" >&2
}

# fail "error" — prints, does not exit. Caller decides.
fail() {
    printf '%s  ✗ %s%s\n' "${_C_RED}" "$*" "${_C_RESET}" >&2
}

# run "<command...>"  — echo+execute; bash -c so quoting collapses
# cleanly when callers pass already-quoted command strings.
run() {
    info "\$ $*"
    bash -c "$*"
}

# run_capture "<command...>"  — execute, return stdout, swallow stderr.
# Useful for probes (`docker ps`, `port_listeners`, …) where stderr
# noise would derail the surrounding logic.
run_capture() {
    bash -c "$*" 2>/dev/null || true
}

# Install an ERR trap that prints which step failed.
# Callers (launch.sh) set _CURRENT_STEP_LABEL before each major step.
install_error_trap() {
    trap '__manyforge_on_error $? $LINENO' ERR
}

__manyforge_on_error() {
    local code="$1"
    local line="$2"
    fail "Step '${_CURRENT_STEP_LABEL:-unknown}' failed (exit ${code} at line ${line})."
    if [[ -n "${_CURRENT_STEP_LOG:-}" && -f "${_CURRENT_STEP_LOG}" ]]; then
        printf '%s  --- tail of %s ---%s\n' "${_C_DIM}" "${_CURRENT_STEP_LOG}" "${_C_RESET}" >&2
        tail -n 40 "${_CURRENT_STEP_LOG}" >&2 || true
    fi
    info "Partial state has been left in place. Inspect with 'scripts/launch.sh status' and tear down with 'scripts/launch.sh stop'."
}
