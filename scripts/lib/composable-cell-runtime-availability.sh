# shellcheck shell=bash
# Temporary fail-closed boundary for runtime surfaces intentionally removed by
# the active composable-cell rebuild. Delete this helper when those surfaces
# are restored and qualified; it is not a compatibility switch.

# This file is an authority boundary, so it must not trust an inherited include
# guard or function definition.  Re-sourcing is harmless; always overwrite the
# fixed values and function before a gated caller can invoke it.

MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE="composable_cell_runtime_unavailable"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: native planning and execution adapters are intentionally removed during the active rebuild."

manyforge_fail_composable_cell_runtime_unavailable() {
    builtin printf '%s\n' "${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE}" >&2
    return 78
}
