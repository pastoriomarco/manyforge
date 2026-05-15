#!/usr/bin/env bash
# demo-assistant-known-good.sh — backwards-compat shim for scripts/launch.sh.
#
# Forwards verbs (start/stop/status/restart/smoke/preflight) and selects the
# manyforge-only + assistant=on + ur10e-scene-authoring scenario. The
# previously-supported `restart-bridge` and `reload-composer` verbs are
# now covered by `restart` (full restart). Env-var overrides
# (MODEL_PROFILE, ASSISTANT_PROVIDER, …) are unchanged and read by
# scripts/lib/assistant.sh.

set -euo pipefail

case "${1:-}" in
    restart-bridge|reload-composer|restart-composer)
        echo "  note: '$1' is deprecated; mapping to 'restart'." >&2
        set -- restart "${@:2}"
        ;;
esac

exec "$(dirname "$0")/launch.sh" \
    --lane manyforge-only \
    --assistant on \
    --scenario ur10e-scene-authoring \
    --non-interactive \
    --yes \
    "$@"
