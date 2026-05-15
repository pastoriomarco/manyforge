#!/usr/bin/env bash
# demo.sh — backwards-compat shim for scripts/launch.sh.
#
# Originally this was a one-shot inline composer launcher; it now delegates
# to the unified launcher in --non-interactive --yes mode. The default
# scenario is `ur5-pick-place` (UR5, no AI). For other scenarios use the
# unified launcher directly: `scripts/launch.sh --list-scenarios`.
#
# Legacy `--simple` flag (the 3-step test fixture) is preserved by stripping
# it from $@ and switching the forwarded --scenario before exec. Works
# regardless of argument order:
#   ./scripts/demo.sh --simple preflight
#   ./scripts/demo.sh preflight --simple

set -euo pipefail

# Position-independent --simple detection. Build a cleaned arg array
# without --simple, and toggle the target scenario.
scenario=ur5-pick-place
args=()
for arg in "$@"; do
    case "$arg" in
        --simple) scenario=ur5-simple-pick-place ;;
        *) args+=("$arg") ;;
    esac
done

exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant off \
    --scenario "${scenario}" \
    --non-interactive \
    --yes \
    "${args[@]}"
