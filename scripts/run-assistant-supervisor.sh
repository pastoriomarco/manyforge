#!/usr/bin/env bash
# scripts/run-assistant-supervisor.sh — convenience launcher for the
# manyforge_assistant_bridge.supervisor (foreground, for debugging).
#
# In normal operation the supervisor is started in detached form by
# scripts/launch.sh (via lib/assistant.sh:start_assistant_supervisor).
# This wrapper is for manual inspection: tail its logs, exit with Ctrl-C.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VENV_PYTHON="${MANYFORGE_ROOT}/manyforge_assistant_bridge/.venv/bin/python"
if [[ -x "${VENV_PYTHON}" ]]; then
    PY="${VENV_PYTHON}"
else
    PY="python3"
fi

cd "${MANYFORGE_ROOT}"
exec "${PY}" -m manyforge_assistant_bridge.supervisor
