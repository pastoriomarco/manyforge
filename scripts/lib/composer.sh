# shellcheck shell=bash
# scripts/lib/composer.sh — Composer-side helpers (health, program load, status).
#
# All endpoints are pinned by COMPOSER_URL (default http://127.0.0.1:9000).

if [[ -n "${_MANYFORGE_LIB_COMPOSER:-}" ]]; then return 0; fi
_MANYFORGE_LIB_COMPOSER=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"
source "$(dirname "${BASH_SOURCE[0]}")/preflight.sh"
source "$(dirname "${BASH_SOURCE[0]}")/wait.sh"

COMPOSER_PORT="${COMPOSER_PORT:-9000}"
COMPOSER_URL="${COMPOSER_URL:-http://127.0.0.1:${COMPOSER_PORT}}"
HMI_PORT="${HMI_PORT:-8081}"
HMI_URL="${HMI_URL:-http://127.0.0.1:${HMI_PORT}}"

# HEALTH_TIMEOUT_S → COMPOSER_READY_TIMEOUT_S deprecation bridge.
# Existing runbooks may set HEALTH_TIMEOUT_S; honour it for one minor
# release cycle while pointing operators at the per-service var.
if [[ -n "${HEALTH_TIMEOUT_S:-}" ]] && [[ -z "${COMPOSER_READY_TIMEOUT_S:-}" ]]; then
    warn "HEALTH_TIMEOUT_S is deprecated; using it as COMPOSER_READY_TIMEOUT_S. Set COMPOSER_READY_TIMEOUT_S directly to suppress this warning."
    COMPOSER_READY_TIMEOUT_S="${HEALTH_TIMEOUT_S}"
fi
COMPOSER_READY_TIMEOUT_S="${COMPOSER_READY_TIMEOUT_S:-600}"
HMI_PORT_READY_TIMEOUT_S="${HMI_PORT_READY_TIMEOUT_S:-${HMI_STARTUP_WAIT_S:-30}}"

legacy_viser_hmi_enabled() {
    local raw="${MANYFORGE_ENABLE_LEGACY_VISER_HMI:-${ENABLE_LEGACY_VISER_HMI:-false}}"
    case "${raw,,}" in
        1|true|yes|on) return 0 ;;
        *) return 1 ;;
    esac
}

# composer_ready / composer_logs — function-name handles for wait.sh.
# composer_logs is lane-aware: the manyforge-only lane runs Composer as a
# detached `docker run` container; the isaac-sim lane uses the `composer-ros`
# compose service. The launcher sets LANE before sourcing this module's wait
# callers, so we can switch on it here.
composer_ready() {
    curl -fsS "${COMPOSER_URL}/api/health" >/dev/null 2>&1
}

composer_logs() {
    case "${LANE:-manyforge-only}" in
        isaac-sim)
            if declare -p COMPOSE_CMD >/dev/null 2>&1; then
                "${COMPOSE_CMD[@]}" logs --tail 20 composer-ros 2>&1 || true
            fi
            ;;
        *)
            docker logs --tail 20 "${COMPOSER_CONTAINER:-manyforge-e2e-composer}" 2>&1 || true
            ;;
    esac
}

hmi_port_ready() {
    port_is_listening "${HMI_PORT}"
}

wait_for_composer() {
    local hint="First-run cold-cache builds can take several minutes (C++ + roboplan inside the dev container). Override with COMPOSER_READY_TIMEOUT_S=NN."
    wait_with_progress \
        "Composer at ${COMPOSER_URL}" \
        "${COMPOSER_READY_TIMEOUT_S}" \
        "${hint}" \
        composer_logs \
        composer_ready
}

wait_for_hmi() {
    if ! legacy_viser_hmi_enabled; then
        info "Legacy Viser HMI disabled; native Composer viewer is the active viewer."
        return 0
    fi
    # HMI binds only after a program load, so a slow HMI is not a fatal
    # condition for `start` — Composer is up, the runtime-status popup
    # reports the rest. Preserve the historical `return 0` semantics.
    local hint="HMI binds within ~1-2 s of load_program completing. If this times out, check Composer logs (docker logs ${COMPOSER_CONTAINER:-manyforge-e2e-composer}) for a load failure."
    wait_with_progress \
        "Viser HMI on ${HMI_URL}" \
        "${HMI_PORT_READY_TIMEOUT_S}" \
        "${hint}" \
        "" \
        hmi_port_ready \
        || warn "Viser HMI port ${HMI_PORT} not listening after ${HMI_PORT_READY_TIMEOUT_S}s — Composer may not have loaded a program yet."
    return 0
}

# load_program PROGRAM_PATH DEPLOYMENT_PATH
# POSTs /api/program/load with forceDiscardOverrides=true so probe runs and
# repeated launcher invocations start from a clean state.
load_program() {
    local program_path="$1"
    local deployment_path="$2"
    step "Loading program/deployment into Composer"
    info "  program:    ${program_path}"
    info "  deployment: ${deployment_path}"
    local payload response_file http_status
    payload="$(PROGRAM_PATH="${program_path}" DEPLOYMENT_PATH="${deployment_path}" python3 - <<'PY'
import json
import os
print(json.dumps({
    "path": os.environ["PROGRAM_PATH"],
    "deploymentPath": os.environ["DEPLOYMENT_PATH"],
    "forceDiscardOverrides": True,
}))
PY
)"
    response_file="$(mktemp)"
    http_status="$(curl -sS -o "${response_file}" -w '%{http_code}' \
        -X POST "${COMPOSER_URL}/api/program/load" \
        -H 'Content-Type: application/json' \
        --data-binary "${payload}" || true)"
    if [[ "${http_status}" -lt 200 || "${http_status}" -ge 300 ]]; then
        fail "Composer artifact load failed with HTTP ${http_status}:"
        cat "${response_file}" >&2
        echo >&2
        rm -f "${response_file}"
        return 1
    fi
    rm -f "${response_file}"
    pass "Program/deployment loaded."
}

# runtime_status_json — print the merged /api/runtime/status response.
# Used by `launch.sh status` and by the final summary.
runtime_status_json() {
    curl -fsS "${COMPOSER_URL}/api/runtime/status" 2>/dev/null || true
}

# runtime_status_summary — short human summary (readiness pill text + AI flag).
#
# Important: pass the JSON via an env var, not via a pipe. `python3 - <<PY`
# binds the heredoc as the script source, which shadows any piped stdin —
# so `printf … | python3 - <<PY ; json.load(sys.stdin)` reads empty stdin.
runtime_status_summary() {
    local json
    json="$(runtime_status_json)"
    if [[ -z "${json}" ]]; then
        echo "  (status endpoint unreachable)"
        return 0
    fi
    MANYFORGE_RUNTIME_STATUS_JSON="${json}" python3 <<'PY'
import json
import os

raw = os.environ.get("MANYFORGE_RUNTIME_STATUS_JSON", "")
try:
    data = json.loads(raw) if raw else {}
except json.JSONDecodeError as exc:
    print(f"  (status endpoint returned non-JSON: {exc})")
    raise SystemExit(0)
print(f"  readiness:        {data.get('readiness','?')} ({data.get('summaryLabel','?')})")
print(f"  profile:          {data.get('profileLabel','?')} [{data.get('profileKind','?')}]")
print(f"  authority:        {data.get('authority','?')}")
print(f"  can_render:       {data.get('canRender',False)}")
print(f"  can_plan:         {data.get('canPlan',False)}")
print(f"  can_execute:      {data.get('canExecute',False)}")
external = data.get('externalProviders') or []
print(f"  external rows:    {', '.join(external) if external else '(none)'}")
PY
}

# Probe whether a deployment YAML opts into the assistant.
# Reads the file on the *host* side; the YAML paths in scenarios.sh are
# /workspace/* (container view), so this requires the path to also be
# reachable from the host (which is true for files under examples/).
deployment_requires_assistant() {
    local deployment_path="$1"
    local host_path
    # Translate /workspace/examples/... -> ${MANYFORGE_ROOT}/examples/...
    if [[ "${deployment_path}" == /workspace/* ]]; then
        host_path="${MANYFORGE_ROOT:?MANYFORGE_ROOT not set}/${deployment_path#/workspace/}"
    else
        host_path="${deployment_path}"
    fi
    if [[ ! -f "${host_path}" ]]; then
        warn "Deployment YAML not found at ${host_path}; cannot verify assistant requirement"
        return 1
    fi
    python3 - "${host_path}" <<'PY'
import sys
try:
    import yaml
except ImportError:
    sys.exit(2)
path = sys.argv[1]
with open(path) as fh:
    data = yaml.safe_load(fh) or {}
assistant = (data.get("assistant") or {})
enabled = bool(assistant.get("enabled", False))
sys.exit(0 if enabled else 1)
PY
}
