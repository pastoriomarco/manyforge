# shellcheck shell=bash
# scripts/lib/prompts.sh — interactive prompts with non-interactive fallback.
#
# Mirrors the NemoClaw onboard.js `promptOrDefault(question, envVar, default)`
# shape. Non-interactive mode is selected by:
#   - MANYFORGE_NON_INTERACTIVE=1, or
#   - --non-interactive flag (callers set MANYFORGE_NON_INTERACTIVE first), or
#   - stdin is not a TTY.
#
# In non-interactive mode each prompt uses ${env_var} if set, else the
# default, and prints "[non-interactive] question → value" to stderr so
# the resolved selection is auditable.

if [[ -n "${_MANYFORGE_LIB_PROMPTS:-}" ]]; then return 0; fi
_MANYFORGE_LIB_PROMPTS=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

# Returns 0 if running non-interactively.
is_non_interactive() {
    [[ "${MANYFORGE_NON_INTERACTIVE:-0}" == "1" ]] && return 0
    [[ -t 0 ]] || return 0
    return 1
}

# prompt_or_default "Question? " ENV_VAR "default"
# Echoes the resolved value to stdout. Diagnostic to stderr.
prompt_or_default() {
    local question="$1"
    local env_var="$2"
    local default_value="${3:-}"

    if is_non_interactive; then
        local resolved=""
        if [[ -n "${env_var}" ]]; then
            resolved="${!env_var:-}"
        fi
        if [[ -z "${resolved}" ]]; then
            resolved="${default_value}"
        fi
        info "[non-interactive] ${question} → ${resolved}"
        printf '%s\n' "${resolved}"
        return 0
    fi

    local prompt_text="${question}"
    if [[ -n "${default_value}" ]]; then
        prompt_text="${question} [${default_value}] "
    fi
    local response
    read -r -p "${prompt_text}" response
    if [[ -z "${response}" ]]; then
        response="${default_value}"
    fi
    printf '%s\n' "${response}"
}

# pick_from_list LABEL DEFAULT_INDEX OPTION1 [OPTION2 …]
# Renders a numbered menu, returns the selected option to stdout.
# In non-interactive mode the default index is used silently (with audit).
pick_from_list() {
    local label="$1"
    local default_idx="$2"
    shift 2
    local options=("$@")
    local count="${#options[@]}"

    if (( count == 0 )); then
        fail "pick_from_list: no options supplied for '${label}'"
        return 1
    fi
    if (( default_idx < 1 || default_idx > count )); then
        default_idx=1
    fi

    if is_non_interactive; then
        local resolved="${options[$((default_idx - 1))]}"
        info "[non-interactive] ${label} → ${resolved} (default)"
        printf '%s\n' "${resolved}"
        return 0
    fi

    printf '\n  %s%s%s\n' "${_C_BOLD}" "${label}" "${_C_RESET}" >&2
    local i
    for (( i = 0; i < count; i++ )); do
        printf '    %d) %s\n' $((i + 1)) "${options[i]}" >&2
    done
    printf '\n' >&2
    local response
    read -r -p "  Choose [${default_idx}]: " response
    if [[ -z "${response}" ]]; then
        response="${default_idx}"
    fi
    if ! [[ "${response}" =~ ^[0-9]+$ ]] || (( response < 1 || response > count )); then
        warn "Invalid selection '${response}'; using default (${options[$((default_idx - 1))]})"
        response="${default_idx}"
    fi
    printf '%s\n' "${options[$((response - 1))]}"
}

# confirm "Continue?"  — returns 0 for yes, 1 for no.
# Non-interactive treats --yes / MANYFORGE_ASSUME_YES=1 as automatic yes.
confirm() {
    local question="$1"
    if is_non_interactive || [[ "${MANYFORGE_ASSUME_YES:-0}" == "1" ]]; then
        info "[non-interactive] ${question} → yes"
        return 0
    fi
    local response
    read -r -p "  ${question} [Y/n] " response
    case "${response,,}" in
        ""|y|yes) return 0 ;;
        *) return 1 ;;
    esac
}
