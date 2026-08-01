# shellcheck shell=bash
# Typed boundaries for the historical deployment-v0 scenario-row library.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob f6045793d753354bbe0fecf8fb153aee235b9d9e.
#
# _load_scenarios and scenario_lookup discovered deployment-v0 metadata and
# consumed the private pipe-delimited rows emitted by scenarios_loader.py.
# scenario_canonical, scenarios_for, scenario_describe, and
# scenarios_print_table owned legacy aliases/filtering/operator rendering;
# scenario_field was inseparable from that row wire. I6/N restores these
# behaviors over addressed Program/artifact facts, with path containment,
# deterministic ordering, alias warnings, filtering, and presentation covered
# by focused client tests. The old table, awk parser, and Python loader are
# absent rather than dormant below an early return.

if [[ -n "${_MANYFORGE_LIB_SCENARIOS:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_SCENARIOS=1

_manyforge_scenarios_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_scenarios_lib_dir}/composable-cell-runtime-availability.sh"

manyforge_fail_scenario_discovery_unavailable() {
    printf '%s\n' \
        "${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: scenario discovery returns with the addressed Program client at I6/N." \
        >&2
    return 78
}

_load_scenarios() {
    manyforge_fail_scenario_discovery_unavailable
}

scenario_canonical() {
    manyforge_fail_scenario_discovery_unavailable
}

scenario_lookup() {
    manyforge_fail_scenario_discovery_unavailable
}

scenario_field() {
    manyforge_fail_scenario_discovery_unavailable
}

scenarios_for() {
    manyforge_fail_scenario_discovery_unavailable
}

scenario_describe() {
    manyforge_fail_scenario_discovery_unavailable
}

scenarios_print_table() {
    manyforge_fail_scenario_discovery_unavailable
}

unset _manyforge_scenarios_lib_dir
