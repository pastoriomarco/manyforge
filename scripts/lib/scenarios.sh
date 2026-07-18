# shellcheck shell=bash
# scripts/lib/scenarios.sh — named (lane, assistant, program, deployment) bundles.
#
# A scenario is a four-tuple: lane × assistant × program path × deployment path.
# The launcher exposes `--scenario NAME` as shorthand, or `--program / --deployment`
# for free-form invocations.
#
# Scenario names, aliases, and pairings are discovered from deployment YAML.

if [[ -n "${_MANYFORGE_LIB_SCENARIOS:-}" ]]; then return 0; fi
_MANYFORGE_LIB_SCENARIOS=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

_SCENARIO_TABLE=()
_SCENARIO_ALIASES=()
_SCENARIOS_LOADED=0

_load_scenarios() {
    if [[ "${_SCENARIOS_LOADED}" == "1" ]]; then
        return 0
    fi
    local loader="${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}/scripts/lib/scenarios_loader.py"
    local output row aliases alias
    if ! output="$(python3 "${loader}")"; then
        fail "Scenario discovery failed; validate the deployment compatibility metadata"
        return 1
    fi
    mapfile -t _SCENARIO_TABLE <<<"${output}"
    if [[ ${#_SCENARIO_TABLE[@]} -eq 0 || -z "${_SCENARIO_TABLE[0]}" ]]; then
        fail "Scenario discovery returned no named scenarios"
        return 1
    fi
    for row in "${_SCENARIO_TABLE[@]}"; do
        aliases="$(scenario_field "${row}" 7)"
        if [[ -z "${aliases}" ]]; then
            continue
        fi
        while IFS= read -r alias; do
            [[ -n "${alias}" ]] && _SCENARIO_ALIASES+=("${alias}|${row%%|*}")
        done < <(tr ',' '\n' <<<"${aliases}")
    done
    _SCENARIOS_LOADED=1
}

# scenario_canonical NAME — resolve alias to canonical; echo canonical
# (or the input unchanged if not an alias). Emits a one-time deprecation
# warning per launch invocation via the _MANYFORGE_ALIAS_WARNED sentinel.
scenario_canonical() {
    _load_scenarios || return 1
    local name="$1"
    local row alias canonical
    for row in "${_SCENARIO_ALIASES[@]}"; do
        alias="${row%%|*}"
        canonical="${row#*|}"
        if [[ "${alias}" == "${name}" ]]; then
            if [[ -z "${_MANYFORGE_ALIAS_WARNED:-}" ]]; then
                warn "scenario name '${alias}' is deprecated; use '${canonical}' (this is the only warning this run)"
                export _MANYFORGE_ALIAS_WARNED=1
            fi
            printf '%s\n' "${canonical}"
            return 0
        fi
    done
    printf '%s\n' "${name}"
}

# scenario_lookup NAME  — echoes the matching pipe-delimited row to stdout.
# Returns 1 if no match.
scenario_lookup() {
    _load_scenarios || return 1
    local name="$1"
    # Resolve any deprecated alias to its canonical name first; this emits
    # a one-time deprecation warning per launch invocation via
    # scenario_canonical's _MANYFORGE_ALIAS_WARNED sentinel.
    name="$(scenario_canonical "${name}")"
    local row
    for row in "${_SCENARIO_TABLE[@]}"; do
        if [[ "${row%%|*}" == "${name}" ]]; then
            printf '%s\n' "${row}"
            return 0
        fi
    done
    return 1
}

scenario_field() {
    local row="$1"
    local idx="$2"   # 1=name, 2=lane, 3=assistant, 4=program, 5=deployment, 6=description
    printf '%s\n' "${row}" | awk -F'|' -v i="${idx}" '{print $i}'
}

# scenarios_for LANE ASSISTANT  — list scenario names compatible with the
# selected lane + assistant axis. Use ASSISTANT="any" to skip the filter.
scenarios_for() {
    _load_scenarios || return 1
    local lane="$1"
    local assistant="$2"
    local row name row_lane row_assistant
    for row in "${_SCENARIO_TABLE[@]}"; do
        name="$(scenario_field "${row}" 1)"
        row_lane="$(scenario_field "${row}" 2)"
        row_assistant="$(scenario_field "${row}" 3)"
        if [[ "${row_lane}" != "${lane}" ]]; then
            continue
        fi
        if [[ "${assistant}" != "any" && "${row_assistant}" != "${assistant}" ]]; then
            continue
        fi
        printf '%s\n' "${name}"
    done
}

# scenario_describe NAME  — pretty-print to stderr.
scenario_describe() {
    local row="$1"
    info "  name:        $(scenario_field "${row}" 1)"
    info "  lane:        $(scenario_field "${row}" 2)"
    info "  assistant:   $(scenario_field "${row}" 3)"
    info "  program:     $(scenario_field "${row}" 4)"
    info "  deployment:  $(scenario_field "${row}" 5)"
    info "  notes:       $(scenario_field "${row}" 6)"
}

scenarios_print_table() {
    _load_scenarios || return 1
    printf '\n  %-36s %-15s %-11s %s\n' "scenario" "lane" "assistant" "description" >&2
    printf '  %-36s %-15s %-11s %s\n' "--------" "----" "---------" "-----------" >&2
    local row name lane assistant description
    for row in "${_SCENARIO_TABLE[@]}"; do
        name="$(scenario_field "${row}" 1)"
        lane="$(scenario_field "${row}" 2)"
        assistant="$(scenario_field "${row}" 3)"
        description="$(scenario_field "${row}" 6)"
        printf '  %-36s %-15s %-11s %s\n' "${name}" "${lane}" "${assistant}" "${description}" >&2
    done
    printf '\n' >&2
}
