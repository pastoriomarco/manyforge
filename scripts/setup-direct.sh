#!/usr/bin/env bash
# setup-direct.sh — Phase 2 deliverable per THREE-LANE-MIGRATION-PLAN.md.
#
# Bring the Direct lane up from a clean state:
#   1. Verify vLLM + proxy are running (the foundation; brought up by
#      ``./scripts/launch.sh start`` with ASSISTANT_PROVIDER=direct).
#   2. Verify Composer is running on :9000.
#   3. Verify the manyforge_assistant_bridge venv is provisioned at
#      manyforge_assistant_bridge/.venv with httpx + fastapi + uvicorn.
#   4. Verify the shared core (manyforge.common.projection) is on
#      MANYFORGE_THOR_ROOT and importable.
#   5. Start the bridge on :8100 if not already running.
#   6. Hit /healthz to confirm readiness.
#
# This script is idempotent: re-running on an already-healthy stack
# logs current state and returns 0.
#
# Per the canonical launcher pattern (per ``feedback_use_launch_script``
# memory entry), this is a HELPER for diagnosing a stuck Direct lane.
# The canonical bring-up remains ``./scripts/launch.sh restart``.
#
# Usage:
#   ./scripts/setup-direct.sh [--vllm-port N] [--composer-base URL]
#
# Exit codes:
#   0  ready
#   1  vLLM not reachable (foundation missing — run launcher first)
#   2  proxy not reachable
#   3  composer not reachable
#   4  bridge venv missing or unhealthy
#   5  shared core not importable
#   6  bridge failed to start
set -euo pipefail

VLLM_PORT="${VLLM_PORT:-8050}"
PROXY_PORT="${PROXY_PORT:-8000}"
COMPOSER_BASE="${COMPOSER_BASE:-http://127.0.0.1:9000}"
BRIDGE_PORT="${BRIDGE_PORT:-8100}"
MANYFORGE_THOR_ROOT="${MANYFORGE_THOR_ROOT:-/home/tndlux/workspaces/dev_ws/src/NemoClaw-Thor/manyforge}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --vllm-port) VLLM_PORT="$2"; shift 2 ;;
    --proxy-port) PROXY_PORT="$2"; shift 2 ;;
    --composer-base) COMPOSER_BASE="$2"; shift 2 ;;
    --bridge-port) BRIDGE_PORT="$2"; shift 2 ;;
    -h|--help)
      sed -n '1,35p' "$0" | sed 's/^# \?//'
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_DIR="${REPO_ROOT}/manyforge_assistant_bridge"
BRIDGE_VENV="${BRIDGE_DIR}/.venv"

green() { echo -e "\033[32m✓\033[0m $1"; }
yellow() { echo -e "\033[33m!\033[0m $1"; }
red() { echo -e "\033[31m✗\033[0m $1" >&2; }

# 1. vLLM on its raw port (behind the proxy).
echo "==> [1/6] vLLM reachable on :${VLLM_PORT}?"
if curl -sf -m 3 "http://127.0.0.1:${VLLM_PORT}/v1/models" >/dev/null 2>&1; then
  green "vLLM up"
else
  red "vLLM not reachable on :${VLLM_PORT}"
  red "  Run: ./scripts/launch.sh start --lane manyforge-only --assistant on --scenario ur10e-scene-authoring --non-interactive --yes"
  exit 1
fi

# 2. vllm-proxy in front of vLLM (the mutation profile lives here).
echo "==> [2/6] vllm-proxy reachable on :${PROXY_PORT}?"
if curl -sf -m 3 "http://127.0.0.1:${PROXY_PORT}/v1/models" >/dev/null 2>&1; then
  green "proxy up (passes through to vLLM)"
else
  red "vllm-proxy not reachable on :${PROXY_PORT}"
  red "  The launcher normally starts this. Check /tmp/manyforge-assistant-e2e/vllm-proxy.log"
  exit 2
fi

# 3. Composer.
echo "==> [3/6] Composer reachable at ${COMPOSER_BASE}?"
if curl -sf -m 3 "${COMPOSER_BASE}/api/program" >/dev/null 2>&1; then
  green "composer up"
else
  red "composer not reachable at ${COMPOSER_BASE}"
  exit 3
fi

# 4. Bridge venv.
echo "==> [4/6] Bridge venv at ${BRIDGE_VENV}?"
if [[ -x "${BRIDGE_VENV}/bin/python" ]]; then
  if "${BRIDGE_VENV}/bin/python" -c "import fastapi, httpx, uvicorn" 2>/dev/null; then
    green "venv healthy (fastapi + httpx + uvicorn importable)"
  else
    yellow "venv exists but key deps missing — running pip install"
    (cd "${BRIDGE_DIR}" && "${BRIDGE_VENV}/bin/pip" install -q -e . >/dev/null)
    green "venv repaired"
  fi
else
  yellow "venv missing — provisioning"
  (cd "${BRIDGE_DIR}" && python3 -m venv .venv && .venv/bin/pip install -q -e . >/dev/null)
  green "venv provisioned"
fi

# 5. Shared core (Phase 2 contract per THREE-LANE plan §7).
echo "==> [5/6] manyforge.common shared core importable from MANYFORGE_THOR_ROOT?"
if [[ ! -d "${MANYFORGE_THOR_ROOT}/common" ]]; then
  yellow "MANYFORGE_THOR_ROOT=${MANYFORGE_THOR_ROOT} does NOT have common/. Direct lane will fall back to local mirror copies."
  yellow "This is acceptable for production deploys that ship only dev_ws/, but degrades the cross-repo refactor."
else
  if MANYFORGE_THOR_ROOT="${MANYFORGE_THOR_ROOT}" "${BRIDGE_VENV}/bin/python" -c "
import os, sys
sys.path.insert(0, '${REPO_ROOT}')
import manyforge_assistant_bridge.bridge as br
if not br._USE_SHARED_PROJECTION:
    raise SystemExit(1)
print(' shared projection ACTIVE from ' + br._MANYFORGE_THOR_ROOT)
" 2>&1; then
    green "shared core active"
  else
    red "shared core NOT active despite MANYFORGE_THOR_ROOT being set — check the bridge's _USE_SHARED_PROJECTION block"
    exit 5
  fi
fi

# 6. Bridge running on :8100.
echo "==> [6/6] Bridge running on :${BRIDGE_PORT}?"
if curl -sf -m 3 "http://127.0.0.1:${BRIDGE_PORT}/healthz" 2>&1 | grep -q '"status":"ok"'; then
  green "bridge healthy"
else
  yellow "bridge not running — start with the launcher (preferred) or:"
  yellow "  cd ${BRIDGE_DIR} && .venv/bin/python -m manyforge_assistant_bridge.bridge"
  exit 6
fi

echo
green "Direct lane ready at http://127.0.0.1:${BRIDGE_PORT}"
echo "  vLLM:    http://127.0.0.1:${VLLM_PORT}"
echo "  Proxy:   http://127.0.0.1:${PROXY_PORT}"
echo "  Composer: ${COMPOSER_BASE}"
echo
echo "  Smoke: python3 -u /home/tndlux/workspaces/dev_ws/src/NemoClaw-Thor/manyforge/scripts/debug/smoke_corpus_runner.py"
