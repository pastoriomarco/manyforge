# shellcheck shell=bash
# Fail-closed boundary shared by historical aggregate entrypoints whose exact
# current successors are incomplete or deliberately separate. Maintained
# ManyForge-only runtime functions now use the Component Manager, Headless,
# Program-runner, and Composer entrypoints directly. Delete this helper only
# after every caller has either acquired its own qualified successor or moved
# to its still-later restoration gate; it is not a compatibility switch.

# This file is an authority boundary, so it must not trust an inherited include
# guard or function definition.  Re-sourcing is harmless; always overwrite the
# fixed values and function before a gated caller can invoke it.

MANYFORGE_RUNTIME_UNAVAILABLE_REASON_CODE="runtime_unavailable"
MANYFORGE_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_RUNTIME_UNAVAILABLE_REASON_CODE}: this legacy aggregate entrypoint has no single qualified current replacement; use the Component Manager, Headless, Program runner, and Composer entrypoints for maintained ManyForge-only runtime functions."

manyforge_fail_runtime_unavailable() {
    builtin printf '%s\n' "${MANYFORGE_RUNTIME_UNAVAILABLE_MESSAGE}" >&2
    return 78
}
