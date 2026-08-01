# shellcheck shell=bash
# Typed boundaries for the historical Composer runtime launcher helpers.
#
# Exact source: manyforge@08222188293df8e12f90dc72e204c21f22f92ea1,
# blob 40cb4124eee7d998ad18e820b4a369208a52a411.
#
# Historical responsibility mapping:
#   composer_ready, composer_logs, wait_for_composer
#       -> owner-issued headless status and bounded client waits at H0/I5;
#   load_program
#       -> addressed Program/deployment client at N/I6;
#   runtime_status_json, runtime_status_summary
#       -> pure presentation of H0 owner facts, without fixed old routes; and
#   deployment_requires_assistant
#       -> compiler-retained assistant settings at I5, not v0 YAML inference.
#
# No fixed Composer URL, old route, HTTP client, Docker log probe, YAML loader,
# process wait remains. Exact launcher cleanup is available through launch.sh
# stop and does not source this library.

if [[ -n "${_MANYFORGE_LIB_COMPOSER:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_COMPOSER=1

_manyforge_composer_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_composer_lib_dir}/composable-cell-runtime-availability.sh"

manyforge_fail_composer_client_unavailable() {
    local message
    message="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}:"
    message+=" Composer runtime clients return with qualified headless owner APIs."
    printf '%s\n' \
        "${message}" \
        >&2
    return 78
}

composer_ready() {
    manyforge_fail_composer_client_unavailable
}

composer_logs() {
    manyforge_fail_composer_client_unavailable
}

wait_for_composer() {
    manyforge_fail_composer_client_unavailable
}

load_program() {
    manyforge_fail_composer_client_unavailable
}

runtime_status_json() {
    manyforge_fail_composer_client_unavailable
}

runtime_status_summary() {
    manyforge_fail_composer_client_unavailable
}

deployment_requires_assistant() {
    manyforge_fail_composer_client_unavailable
}

unset _manyforge_composer_lib_dir
