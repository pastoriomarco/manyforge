# shellcheck shell=bash
# Bounded, owner-neutral interactive selection helpers.
#
# Exact historical source:
#   manyforge@08222188293df8e12f90dc72e204c21f22f92ea1
#   blob 96ac84aab753a1f2eefb50eb6f81d555f48a30db
#
# These helpers retain presentation and deterministic non-interactive
# selection only. They do not know lanes, components, deployments, processes,
# ports, or runtime state. The old launcher's concrete wizard choices return
# only as thin clients over owner-issued H0/N/I9/I10 facts.

if [[ -n "${_MANYFORGE_LIB_PROMPTS:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_PROMPTS=1

_manyforge_prompts_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=runner.sh
source "${_manyforge_prompts_lib_dir}/runner.sh"
unset _manyforge_prompts_lib_dir

readonly _MANYFORGE_PROMPT_MAX_QUESTION_CHARACTERS=512
readonly _MANYFORGE_PROMPT_MAX_VALUE_CHARACTERS=2048
readonly _MANYFORGE_PROMPT_MAX_OPTIONS=64
readonly _MANYFORGE_PROMPT_MAX_OPTION_CHARACTERS=512

_manyforge_prompt_is_safe_text() {
    if (( $# != 2 )); then
        return 2
    fi
    local value="$1"
    local maximum="$2"
    local LC_ALL=C
    (( ${#value} <= maximum )) || return 1
    [[ ! "${value}" =~ [[:cntrl:]] ]]
}

_manyforge_prompt_read_bounded() {
    if (( $# != 3 )); then
        return 2
    fi
    local prompt_text="$1"
    local maximum="$2"
    local output_name="$3"

    if [[ ! "${output_name}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]]; then
        return 2
    fi
    local -n output_value="${output_name}"
    local captured=""

    if ! IFS= read -r -n "$((maximum + 1))" -p "${prompt_text}" captured; then
        fail "Prompt input ended before a response was read"
        return 2
    fi
    printf '\n' >&2
    if ! _manyforge_prompt_is_safe_text "${captured}" "${maximum}"; then
        fail "Prompt response is too long or contains terminal control characters"
        return 2
    fi
    output_value="${captured}"
}

# Returns 0 if running non-interactively.
is_non_interactive() {
    if (( $# != 0 )); then
        return 2
    fi
    [[ "${MANYFORGE_NON_INTERACTIVE:-0}" == "1" ]] && return 0
    [[ -t 0 ]] || return 0
    return 1
}

# prompt_or_default "Question? " ENV_VAR "default"
# Prints the selected value to stdout and diagnostics to stderr.
prompt_or_default() {
    if (( $# < 2 || $# > 3 )); then
        fail "prompt_or_default expects QUESTION ENV_VAR [DEFAULT]"
        return 2
    fi
    local question="$1"
    local env_var="$2"
    local default_value="${3:-}"

    if ! _manyforge_prompt_is_safe_text \
        "${question}" "${_MANYFORGE_PROMPT_MAX_QUESTION_CHARACTERS}"; then
        fail "prompt_or_default question is too long or contains control characters"
        return 2
    fi
    if [[ -n "${env_var}" ]] \
        && [[ ! "${env_var}" =~ ^[A-Za-z_][A-Za-z0-9_]{0,127}$ ]]; then
        fail "prompt_or_default received an invalid environment variable name"
        return 2
    fi
    if ! _manyforge_prompt_is_safe_text \
        "${default_value}" "${_MANYFORGE_PROMPT_MAX_VALUE_CHARACTERS}"; then
        fail "prompt_or_default default is too long or contains control characters"
        return 2
    fi

    if is_non_interactive; then
        local resolved=""
        if [[ -n "${env_var}" ]]; then
            resolved="${!env_var:-}"
        fi
        if [[ -z "${resolved}" ]]; then
            resolved="${default_value}"
        fi
        if ! _manyforge_prompt_is_safe_text \
            "${resolved}" "${_MANYFORGE_PROMPT_MAX_VALUE_CHARACTERS}"; then
            fail "prompt_or_default selected value is too long or contains control characters"
            return 2
        fi
        info "[non-interactive] ${question} → ${resolved}"
        printf '%s\n' "${resolved}"
        return 0
    fi

    local prompt_text="${question}"
    if [[ -n "${default_value}" ]]; then
        prompt_text="${question} [${default_value}] "
    fi
    local response=""
    _manyforge_prompt_read_bounded \
        "${prompt_text}" "${_MANYFORGE_PROMPT_MAX_VALUE_CHARACTERS}" response \
        || return $?
    if [[ -z "${response}" ]]; then
        response="${default_value}"
    fi
    printf '%s\n' "${response}"
}

# pick_from_list LABEL DEFAULT_INDEX OPTION1 [OPTION2 ...]
pick_from_list() {
    if (( $# < 3 )); then
        fail "pick_from_list expects LABEL DEFAULT_INDEX OPTION..."
        return 2
    fi
    local label="$1"
    local default_idx="$2"
    shift 2
    local options=("$@")
    local count="${#options[@]}"

    if ! _manyforge_prompt_is_safe_text \
        "${label}" "${_MANYFORGE_PROMPT_MAX_QUESTION_CHARACTERS}"; then
        fail "pick_from_list label is too long or contains control characters"
        return 2
    fi
    if (( count == 0 || count > _MANYFORGE_PROMPT_MAX_OPTIONS )); then
        fail "pick_from_list requires 1..${_MANYFORGE_PROMPT_MAX_OPTIONS} options"
        return 2
    fi
    local option
    for option in "${options[@]}"; do
        if [[ -z "${option}" ]] || ! _manyforge_prompt_is_safe_text \
            "${option}" "${_MANYFORGE_PROMPT_MAX_OPTION_CHARACTERS}"; then
            fail "pick_from_list option is empty, too long, or contains control characters"
            return 2
        fi
    done
    if [[ ! "${default_idx}" =~ ^[0-9]+$ ]] \
        || (( default_idx < 1 || default_idx > count )); then
        default_idx=1
    fi

    if is_non_interactive; then
        local resolved="${options[$((default_idx - 1))]}"
        info "[non-interactive] ${label} → ${resolved} (default)"
        printf '%s\n' "${resolved}"
        return 0
    fi

    printf '\n  %s%s%s\n' \
        "${_MANYFORGE_COLOR_BOLD}" "${label}" \
        "${_MANYFORGE_COLOR_RESET}" >&2
    local i
    for ((i = 0; i < count; i++)); do
        printf '    %d) %s\n' "$((i + 1))" "${options[i]}" >&2
    done
    printf '\n' >&2

    local response=""
    _manyforge_prompt_read_bounded \
        "  Choose [${default_idx}]: " 16 response || return $?
    if [[ -z "${response}" ]]; then
        response="${default_idx}"
    fi
    if [[ ! "${response}" =~ ^[0-9]+$ ]] \
        || (( response < 1 || response > count )); then
        warn "Invalid selection '${response}'; using default (${options[$((default_idx - 1))]})"
        response="${default_idx}"
    fi
    printf '%s\n' "${options[$((response - 1))]}"
}

# confirm "Continue?" returns 0 for yes and 1 for no.
confirm() {
    if (( $# != 1 )); then
        fail "confirm expects exactly one question"
        return 2
    fi
    local question="$1"
    if ! _manyforge_prompt_is_safe_text \
        "${question}" "${_MANYFORGE_PROMPT_MAX_QUESTION_CHARACTERS}"; then
        fail "confirm question is too long or contains control characters"
        return 2
    fi
    if is_non_interactive || [[ "${MANYFORGE_ASSUME_YES:-0}" == "1" ]]; then
        info "[non-interactive] ${question} → yes"
        return 0
    fi

    local response=""
    _manyforge_prompt_read_bounded \
        "  ${question} [Y/n] " 16 response || return $?
    case "${response,,}" in
        "" | y | yes) return 0 ;;
        *) return 1 ;;
    esac
}
