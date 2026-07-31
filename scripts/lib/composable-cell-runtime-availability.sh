# shellcheck shell=bash
# Temporary fail-closed boundary for runtime surfaces intentionally removed by
# the active composable-cell rebuild. Delete this helper when those surfaces
# are restored and qualified; it is not a compatibility switch.

if [[ -n "${_MANYFORGE_COMPOSABLE_CELL_RUNTIME_AVAILABILITY:-}" ]]; then
    return 0
fi
_MANYFORGE_COMPOSABLE_CELL_RUNTIME_AVAILABILITY=1

MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="Composable-cell runtime unavailable: native planning and execution adapters are intentionally removed during the active rebuild."

manyforge_fail_composable_cell_runtime_unavailable() {
    printf '%s\n' "${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE}" >&2
    return 78
}
