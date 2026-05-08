#!/usr/bin/env bash
# demo-assistant-known-good.sh — Cosmos-first ManyForge assistant demo launcher.
#
# Usage:
#   ./scripts/demo-assistant-known-good.sh start
#   ./scripts/demo-assistant-known-good.sh restart-bridge
#   ./scripts/demo-assistant-known-good.sh reload-composer
#   ./scripts/demo-assistant-known-good.sh smoke
#   ./scripts/demo-assistant-known-good.sh preflight
#   ./scripts/demo-assistant-known-good.sh status
#   ./scripts/demo-assistant-known-good.sh stop
#
# Defaults target the current single-model assistant candidate:
#   cosmos-reason2-8b

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NEMOCLAW_THOR="${NEMOCLAW_THOR:-${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor}"

MODEL_PROFILE="${MODEL_PROFILE:-cosmos-reason2-8b}"
SAMPLING_PROFILE="${SAMPLING_PROFILE:-cosmos-bridge-default}"
DEMO_DEPLOYMENT_PATH="${DEMO_DEPLOYMENT_PATH:-/workspace/examples/assistant_modes_scene_authoring.deployment.yaml}"
DEMO_PROGRAM_PATH="${DEMO_PROGRAM_PATH:-/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml}"
VLLM_CONTAINER="${VLLM_CONTAINER:-manyforge-e2e-vllm}"
COMPOSER_CONTAINER="${COMPOSER_CONTAINER:-manyforge-e2e-composer}"
LOG_DIR="${LOG_DIR:-/tmp/manyforge-assistant-e2e}"
BRIDGE_PID_FILE="${BRIDGE_PID_FILE:-${LOG_DIR}/known-good-bridge.pid}"
BRIDGE_LOG="${BRIDGE_LOG:-${LOG_DIR}/known-good-bridge.log}"
BRIDGE_AUDIT_LOG="${BRIDGE_AUDIT_LOG:-${LOG_DIR}/known-good-bridge-audit.jsonl}"
COMPOSER_URL="${COMPOSER_URL:-http://127.0.0.1:9000}"
UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL:-http://127.0.0.1:8000/v1}"
# OpenClaw (sandboxed gateway) is the default assistant-provider lane:
# openclaw_assistant_bridge on :8200, persistent OpenClaw gateway in the
# my-assistant sandbox, mode-scoped MCP wrapper, full bounded-autonomy
# audit chain. The direct-vLLM lane stays available as a backup (set
# ASSISTANT_PROVIDER=nemoclaw) — useful for fast local iteration when
# sandbox isolation is not needed.
#
# Default switch verified by the 2026-05-06 lane-parity probe (after
# the trio of fixes — vLLM `T=0.6, top_p=0.95` + `enable_thinking=false`
# server defaults, MCP wrapper validates required args, schemas ship
# worked examples). All 5 tasks pass on both lanes within 1.3× of each
# other; OpenClaw is *faster* on scene_inspect and scene_add. See
# `manyforge/docs/LANE-COMPARISON-direct-vs-openclaw.md` §9 for the
# full diagnosis and the before/after numbers.
ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER:-openclaw}"
# Bridge URL is provider-derived: nemoclaw -> :8100 (manyforge_assistant_bridge
# here), openclaw -> :8200 (openclaw_assistant_bridge in NemoClaw-Thor).
case "${ASSISTANT_PROVIDER}" in
    openclaw) BRIDGE_URL="${BRIDGE_URL:-http://127.0.0.1:8200}" ;;
    nemoclaw|*) BRIDGE_URL="${BRIDGE_URL:-http://127.0.0.1:8100}" ;;
esac
# 300s composer timeout: the OpenClaw lane has legitimate runs in the
# 100-200s range on multi-tool turns; 180s timed out one of the comparison
# tests. Direct lane stays well under the limit; this is just headroom.
ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S:-300}"
ASSISTANT_MAX_TURNS="${ASSISTANT_MAX_TURNS:-16}"
DROP_CACHES="${DROP_CACHES:-true}"
PRESERVE_OPENSHELL="${PRESERVE_OPENSHELL:-true}"
REBUILD_FRONTEND_ON_COMPOSER_RELOAD="${REBUILD_FRONTEND_ON_COMPOSER_RELOAD:-true}"
PROVISION_OPENCLAW_SANDBOX="${PROVISION_OPENCLAW_SANDBOX:-true}"

usage() {
    cat <<EOF
Usage: $0 {start|restart-bridge|reload-composer|restart-composer|smoke|preflight|status|stop|restart}

Environment overrides:
  MODEL_PROFILE=${MODEL_PROFILE}
  SAMPLING_PROFILE=${SAMPLING_PROFILE}
  DEMO_DEPLOYMENT_PATH=${DEMO_DEPLOYMENT_PATH}
  DEMO_PROGRAM_PATH=${DEMO_PROGRAM_PATH}
  NEMOCLAW_THOR=${NEMOCLAW_THOR}
  DROP_CACHES=${DROP_CACHES}
  ASSISTANT_PROVIDER=${ASSISTANT_PROVIDER}        # 'openclaw' (default — sandboxed gateway) | 'nemoclaw' (direct vLLM, backup)
  BRIDGE_URL=${BRIDGE_URL}                         # auto-derived from provider
  ASSISTANT_TIMEOUT_S=${ASSISTANT_TIMEOUT_S}
  ASSISTANT_MAX_TURNS=${ASSISTANT_MAX_TURNS}
  PRESERVE_OPENSHELL=${PRESERVE_OPENSHELL}
  PROVISION_OPENCLAW_SANDBOX=${PROVISION_OPENCLAW_SANDBOX}   # apply manyforge-composer preset/skill/MCP at start
  REBUILD_FRONTEND_ON_COMPOSER_RELOAD=${REBUILD_FRONTEND_ON_COMPOSER_RELOAD}

Provider lanes (both preserve the same bounded-autonomy contract on the
ManyForge side — mode-scoped MCP wrapper + /api/assistant/bridge/tools/{toolId}
+ audit envelope):

  openclaw  (default)    Persistent OpenClaw gateway in NemoClaw sandbox.
                         Bridge: NemoClaw-Thor/manyforge/openclaw_assistant_bridge
                         on :8200. Sandbox isolation + bounded-autonomy
                         per-tool audit. Lane parity verified 2026-05-06:
                         all tasks within 1.3x of direct lane (faster on
                         some). 'start' provisions the sandbox automatically;
                         set PROVISION_OPENCLAW_SANDBOX=false to skip.
  nemoclaw  (backup)     Direct vLLM via in-tree manyforge_assistant_bridge on
                         :8100. Sub-2s warm calls; the bridge runs the tool
                         loop in-process. Useful when you want to iterate
                         without the sandbox layer, or when OpenClaw gateway
                         is offline.

By default, stop/restart preserves the OpenShell gateway because the gateway
owns the live sandbox state. Set PRESERVE_OPENSHELL=false only when you
intentionally want to delete and recreate that gateway.

Use reload-composer after Composer/backend/frontend code changes. It rebuilds
the frontend bundle, restarts only the Composer/HMI container, reloads the demo
artifacts, and leaves vLLM, the assistant bridge, and OpenShell running.

Use restart-bridge after bridge code or bridge-budget changes. It restarts only
the assistant bridge and leaves Composer, vLLM, and OpenShell running.
EOF
}

container_running() {
    local name="$1"
    docker ps --format '{{.Names}}' | grep -qx "${name}"
}

bridge_pid_alive() {
    if [[ ! -f "${BRIDGE_PID_FILE}" ]]; then
        return 1
    fi
    local pid
    pid="$(cat "${BRIDGE_PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
        return 0
    fi
    rm -f "${BRIDGE_PID_FILE}"
    return 1
}

port_listeners() {
    local port="$1"
    ss -ltnp | grep -E ":${port}\\b" || true
}

port_is_listening() {
    local port="$1"
    [[ -n "$(port_listeners "${port}")" ]]
}

expected_port_owner() {
    local port="$1"
    case "${port}" in
        8000) container_running "${VLLM_CONTAINER}" ;;
        8080) container_running "openshell-cluster-nemoclaw" ;;
        8081) container_running "${COMPOSER_CONTAINER}" ;;
        8100) bridge_pid_alive ;;        # nemoclaw provider lane (vLLM direct)
        8200) bridge_pid_alive ;;        # openclaw provider lane (gateway HTTP)
        9000) container_running "${COMPOSER_CONTAINER}" ;;
        *) return 1 ;;
    esac
}

preflight() {
    local errors=0
    local port lines
    for port in 8000 8080 8081 8100 8200 9000; do
        if ! port_is_listening "${port}"; then
            continue
        fi
        if expected_port_owner "${port}"; then
            continue
        fi
        echo "Unexpected listener on port ${port}; refusing to start duplicate demo services." >&2
        lines="$(port_listeners "${port}")"
        [[ -n "${lines}" ]] && echo "${lines}" >&2
        errors=1
    done
    if (( errors != 0 )); then
        cat >&2 <<EOF

Run './scripts/demo-assistant-known-good.sh stop' to clean known demo services.
If a manual Composer, bridge, vLLM, or OpenShell instance owns one of these
ports, stop it explicitly before starting the known-good demo.
EOF
        return 1
    fi
    echo "Preflight ok: no unmanaged listeners on ports 8000, 8080, 8081, 8100, or 9000."
}

preflight_composer_ports() {
    local errors=0
    local port lines
    for port in 8081 9000; do
        if ! port_is_listening "${port}"; then
            continue
        fi
        if container_running "${COMPOSER_CONTAINER}"; then
            continue
        fi
        echo "Unexpected listener on Composer port ${port}; refusing to start duplicate Composer services." >&2
        lines="$(port_listeners "${port}")"
        [[ -n "${lines}" ]] && echo "${lines}" >&2
        errors=1
    done
    if (( errors != 0 )); then
        cat >&2 <<EOF

Stop the process that owns Composer/HMI ports 9000 or 8081, then retry:
  ./scripts/demo-assistant-known-good.sh reload-composer
EOF
        return 1
    fi
}

step() {
    printf '\n==> %s\n' "$*"
}

ensure_dirs() {
    mkdir -p "${LOG_DIR}"
}

drop_caches() {
    if [[ "${DROP_CACHES}" != "true" ]]; then
        return
    fi
    sync
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
}

print_truncated() {
    local value="${1//$'\r'/ }"
    local columns="${COLUMNS:-120}"
    if ! [[ "${columns}" =~ ^[0-9]+$ ]]; then
        columns=120
    fi
    local max=$((columns - 6))
    if (( max < 40 )); then
        max=40
    fi
    printf '%.*s' "${max}" "${value}"
}

wait_json() {
    local url="$1"
    local timeout_s="${2:-60}"
    local label="${3:-Waiting for ${url}}"
    local hint="${4:-}"
    local log_container="${5:-}"
    local start=$SECONDS
    local deadline=$((start + timeout_s))
    local next_log_at=0
    local elapsed rendered_lines=0

    render_wait() {
        local current_elapsed="$1"
        local line
        local log_lines=()
        if [[ -n "${log_container}" ]]; then
            while IFS= read -r line; do
                log_lines+=("${line}")
            done < <(docker logs --tail 3 "${log_container}" 2>&1 || true)
        fi

        if [[ -t 1 ]]; then
            if (( rendered_lines > 0 )); then
                printf '\033[%sA\033[J' "${rendered_lines}"
            fi
            rendered_lines=0
            printf '%s\n' "${label}"
            rendered_lines=$((rendered_lines + 1))
            if [[ -n "${hint}" ]]; then
                printf '  %s\n' "${hint}"
                rendered_lines=$((rendered_lines + 1))
            fi
            printf '  Loading, please wait... elapsed %ds\n' "${current_elapsed}"
            rendered_lines=$((rendered_lines + 1))
            if [[ -n "${log_container}" ]]; then
                printf '  Latest %s logs:\n' "${log_container}"
                rendered_lines=$((rendered_lines + 1))
                if (( ${#log_lines[@]} == 0 )); then
                    printf '    (no logs yet)\n'
                    rendered_lines=$((rendered_lines + 1))
                else
                    for line in "${log_lines[@]}"; do
                        printf '    '
                        print_truncated "${line}"
                        printf '\n'
                        rendered_lines=$((rendered_lines + 1))
                    done
                fi
            fi
            return
        fi

        if (( current_elapsed < next_log_at )); then
            return
        fi
        printf '%s\n' "${label}"
        if [[ -n "${hint}" ]]; then
            printf '  %s\n' "${hint}"
        fi
        printf '  Loading, please wait... elapsed %ds\n' "${current_elapsed}"
        if [[ -n "${log_container}" ]]; then
            printf '  Latest %s logs:\n' "${log_container}"
            if (( ${#log_lines[@]} == 0 )); then
                printf '    (no logs yet)\n'
            else
                for line in "${log_lines[@]}"; do
                    printf '    '
                    print_truncated "${line}"
                    printf '\n'
                done
            fi
        fi
        next_log_at=$((current_elapsed + 30))
    }

    until curl -fsS "${url}" >/dev/null 2>&1; do
        if (( SECONDS >= deadline )); then
            if [[ -t 1 ]] && (( rendered_lines > 0 )); then
                printf '\033[%sA\033[J' "${rendered_lines}"
            fi
            echo "Timed out waiting for ${label}" >&2
            return 1
        fi
        elapsed=$((SECONDS - start))
        render_wait "${elapsed}"
        sleep 1
    done
    elapsed=$((SECONDS - start))
    if [[ -t 1 ]] && (( rendered_lines > 0 )); then
        printf '\033[%sA\033[J' "${rendered_lines}"
    fi
    printf '%s\n' "${label}"
    printf '  ready after %ds\n' "${elapsed}"
}

served_model_id() {
    curl -fsS "${UPSTREAM_BASE_URL}/models" 2>/dev/null | python3 -c '
import json
import sys

try:
    data = json.load(sys.stdin)
    models = data.get("data") or []
    if models:
        print(models[0].get("id", ""))
except Exception:
    pass
'
}

print_json_if_available() {
    local url="$1"
    local raw
    raw="$(curl -fsS "${url}" 2>/dev/null || true)"
    if [[ -z "${raw}" ]]; then
        return
    fi
    python3 -m json.tool <<<"${raw}" || true
}

bridge_python() {
    local venv_python="${MANYFORGE_ROOT}/manyforge_assistant_bridge/.venv/bin/python"
    if [[ -x "${venv_python}" ]]; then
        printf '%s\n' "${venv_python}"
    else
        printf '%s\n' "python3"
    fi
}

install_bridge_deps() {
    local venv_python="${MANYFORGE_ROOT}/manyforge_assistant_bridge/.venv/bin/python"
    if [[ -x "${venv_python}" ]]; then
        return
    fi
    (cd "${MANYFORGE_ROOT}" && python3 -m venv manyforge_assistant_bridge/.venv)
    "${venv_python}" -m pip install -r "${MANYFORGE_ROOT}/manyforge_assistant_bridge/requirements.txt"
}

build_frontend_dist() {
    if [[ "${REBUILD_FRONTEND_ON_COMPOSER_RELOAD}" != "true" ]]; then
        echo "Skipping frontend rebuild because REBUILD_FRONTEND_ON_COMPOSER_RELOAD=false."
        return
    fi
    step "Building Composer frontend bundle"
    if command -v npm >/dev/null 2>&1; then
        (cd "${MANYFORGE_ROOT}/manyforge_composer/frontend" && npm run build)
        return
    fi
    echo "Host npm not found; building frontend inside manyforge-dev container."
    docker run --rm \
        -v "${MANYFORGE_ROOT}:/workspace" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace/manyforge_composer/frontend \
        manyforge-dev:latest \
        npm run build
}

start_model() {
    step "Preparing vLLM model: ${MODEL_PROFILE}"
    if container_running "${VLLM_CONTAINER}"; then
        local current_model
        current_model="$(served_model_id || true)"
        if [[ "${current_model}" != "${MODEL_PROFILE}" ]]; then
            echo "Existing ${VLLM_CONTAINER} serves ${current_model:-unknown}; restarting for ${MODEL_PROFILE}."
            docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true
        else
            echo "Reusing ${VLLM_CONTAINER}; it already serves ${MODEL_PROFILE}."
        fi
    fi

    if ! container_running "${VLLM_CONTAINER}"; then
        echo "Starting ${VLLM_CONTAINER}; first launch can take several minutes while vLLM loads/compiles."
        docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true
        drop_caches
        (
            cd "${NEMOCLAW_THOR}"
            THOR_DETACH=1 \
            THOR_CONTAINER_NAME="${VLLM_CONTAINER}" \
            ./serving/start-model.sh "${MODEL_PROFILE}"
        )
    fi
    wait_json \
        "${UPSTREAM_BASE_URL}/models" \
        900 \
        "Waiting for vLLM /v1/models on ${UPSTREAM_BASE_URL}" \
        "Inspect full details in another terminal with: docker logs -f ${VLLM_CONTAINER}" \
        "${VLLM_CONTAINER}"
    local served
    served="$(served_model_id || true)"
    if [[ "${served}" != "${MODEL_PROFILE}" ]]; then
        echo "vLLM served model mismatch: ${served:-unknown} (expected ${MODEL_PROFILE})" >&2
        return 1
    fi
    echo "vLLM ready: ${served}"
}

configure_nemoclaw() {
    step "Configuring NemoClaw/OpenShell provider"
    local configure_log="${LOG_DIR}/configure-local-provider.log"
    if (cd "${NEMOCLAW_THOR}" && ./setup/configure-local-provider.sh "${MODEL_PROFILE}") \
        2>&1 | tee "${configure_log}"; then
        return
    fi

    if openshell sandbox list 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -Eq '^No sandboxes found\.?$'; then
        cat <<EOF
NemoClaw/OpenShell provider route was configured, but no live sandbox exists.
Continuing because the ManyForge Composer assistant path uses the host bridge
and local vLLM endpoint directly. Re-run NemoClaw onboarding later if you need
interactive OpenClaw inside the sandbox.
EOF
        return
    fi

    echo "NemoClaw/OpenShell provider configuration failed; see ${configure_log}" >&2
    return 1
}

start_composer() {
    step "Starting ManyForge Composer and HMI"
    if container_running "${COMPOSER_CONTAINER}"; then
        echo "Reusing ${COMPOSER_CONTAINER}."
        return
    fi
    preflight_composer_ports
    docker rm -f "${COMPOSER_CONTAINER}" >/dev/null 2>&1 || true
    docker run -d --rm \
        --name "${COMPOSER_CONTAINER}" \
        --network host \
        -v "${MANYFORGE_ROOT}:/workspace" \
        -v manyforge_build-cache:/tmp/manyforge-build \
        -w /workspace \
        -e MANYFORGE_ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER}" \
        -e MANYFORGE_ASSISTANT_ENDPOINT_URL="${BRIDGE_URL}/v1/manyforge/assistant" \
        -e MANYFORGE_ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
        manyforge-dev:latest \
        bash -lc "python -m manyforge_composer \
          --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
          --host ${COMPOSER_BIND_HOST:-0.0.0.0} \
          --port 9000 \
          --hmi-port 8081 \
          --mcp-http \
          --assistant-provider ${ASSISTANT_PROVIDER} \
          --assistant-endpoint ${BRIDGE_URL}/v1/manyforge/assistant \
          --assistant-timeout-s ${ASSISTANT_TIMEOUT_S}" >/dev/null
    wait_json \
        "${COMPOSER_URL}/api/infra/status" \
        120 \
        "Waiting for Composer infra status on ${COMPOSER_URL}" \
        "Inspect full details in another terminal with: docker logs -f ${COMPOSER_CONTAINER}" \
        "${COMPOSER_CONTAINER}"
}

start_bridge() {
    case "${ASSISTANT_PROVIDER}" in
        openclaw) start_bridge_openclaw ;;
        nemoclaw|*) start_bridge_nemoclaw ;;
    esac
}

start_bridge_nemoclaw() {
    step "Starting ManyForge assistant bridge (nemoclaw / direct-vLLM lane)"
    if bridge_pid_alive; then
        echo "Reusing bridge from ${BRIDGE_PID_FILE}."
        return
    fi
    if port_is_listening 8100; then
        echo "Port 8100 is already in use by an unmanaged process; refusing to start another bridge." >&2
        port_listeners 8100 >&2
        return 1
    fi
    install_bridge_deps
    local python
    python="$(bridge_python)"
    (
        cd "${MANYFORGE_ROOT}"
        env \
          BRIDGE_HOST=127.0.0.1 \
          BRIDGE_PORT=8100 \
          BRIDGE_UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL}" \
          BRIDGE_MANYFORGE_BASE_URL="${COMPOSER_URL}" \
          BRIDGE_MODEL="${MODEL_PROFILE}" \
          BRIDGE_REQUEST_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
          BRIDGE_MAX_WALL_TIME_S="${ASSISTANT_TIMEOUT_S}" \
          BRIDGE_MAX_TURNS="${ASSISTANT_MAX_TURNS}" \
          BRIDGE_AUDIT_LOG="${BRIDGE_AUDIT_LOG}" \
          setsid "${python}" -m manyforge_assistant_bridge.bridge \
          </dev/null >"${BRIDGE_LOG}" 2>&1 &
        echo $! >"${BRIDGE_PID_FILE}"
    )
    wait_json \
        "${BRIDGE_URL}/healthz" \
        60 \
        "Waiting for assistant bridge health on ${BRIDGE_URL}" \
        "Bridge log: ${BRIDGE_LOG}"
}

start_bridge_openclaw() {
    step "Starting OpenClaw assistant bridge (openclaw / persistent-gateway lane)"
    if bridge_pid_alive; then
        echo "Reusing bridge from ${BRIDGE_PID_FILE}."
        return
    fi
    if port_is_listening 8200; then
        echo "Port 8200 is already in use by an unmanaged process; refusing to start another bridge." >&2
        port_listeners 8200 >&2
        return 1
    fi
    local launcher="${NEMOCLAW_THOR}/manyforge/start-openclaw-assistant-bridge.sh"
    if [[ ! -x "${launcher}" ]]; then
        echo "openclaw bridge launcher not found at: ${launcher}" >&2
        echo "Set NEMOCLAW_THOR or fall back to ASSISTANT_PROVIDER=nemoclaw." >&2
        return 1
    fi
    # The launcher already sets PYTHONPATH, ensures venv, and runs uvicorn.
    # We pin USE_GATEWAY=true (the validated lane) and the slim agent profile.
    (
        cd "${NEMOCLAW_THOR}"
        env \
          OPENCLAW_ASSISTANT_USE_GATEWAY=true \
          OPENCLAW_ASSISTANT_AGENT=manyforge-composer \
          OPENCLAW_ASSISTANT_LOCAL=false \
          OPENCLAW_ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
          OPENCLAW_ASSISTANT_BRIDGE_HOST=127.0.0.1 \
          OPENCLAW_ASSISTANT_BRIDGE_PORT=8200 \
          setsid "${launcher}" </dev/null >"${BRIDGE_LOG}" 2>&1 &
        echo $! >"${BRIDGE_PID_FILE}"
    )
    wait_json \
        "${BRIDGE_URL}/healthz" \
        60 \
        "Waiting for openclaw assistant bridge health on ${BRIDGE_URL}" \
        "Bridge log: ${BRIDGE_LOG}"
}

provision_openclaw_sandbox() {
    if [[ "${ASSISTANT_PROVIDER}" != "openclaw" ]]; then
        return 0
    fi
    if [[ "${PROVISION_OPENCLAW_SANDBOX}" != "true" ]]; then
        echo "PROVISION_OPENCLAW_SANDBOX=${PROVISION_OPENCLAW_SANDBOX}; skipping sandbox provisioning."
        return 0
    fi
    step "Provisioning OpenClaw sandbox (manyforge-composer skill + egress preset + MCP server)"
    local provisioner="${NEMOCLAW_THOR}/manyforge/setup-manyforge-assistant.sh"
    if [[ ! -x "${provisioner}" ]]; then
        echo "openclaw provisioner not found at: ${provisioner}" >&2
        echo "Set NEMOCLAW_THOR, or set PROVISION_OPENCLAW_SANDBOX=false to skip." >&2
        return 1
    fi
    "${provisioner}" "${OPENCLAW_ASSISTANT_SANDBOX:-my-assistant}"
}

load_demo_artifacts() {
    step "Loading known-good program and assistant deployment"
    DEMO_DEPLOYMENT_PATH="${DEMO_DEPLOYMENT_PATH}" python3 - <<'PY' | curl -fsS -X POST "${COMPOSER_URL}/api/deployment/load" \
        -H "Content-Type: application/json" --data-binary @- >/dev/null
import json
import os
print(json.dumps({"path": os.environ["DEMO_DEPLOYMENT_PATH"]}))
PY
    DEMO_DEPLOYMENT_PATH="${DEMO_DEPLOYMENT_PATH}" DEMO_PROGRAM_PATH="${DEMO_PROGRAM_PATH}" python3 - <<'PY' | curl -fsS -X POST "${COMPOSER_URL}/api/program/load" \
        -H "Content-Type: application/json" --data-binary @- >/dev/null
import json
import os
print(json.dumps({
    "path": os.environ["DEMO_PROGRAM_PATH"],
    "deploymentPath": os.environ["DEMO_DEPLOYMENT_PATH"],
    "forceDiscardOverrides": True,
}))
PY
}

reload_composer() {
    ensure_dirs
    build_frontend_dist
    step "Restarting Composer/HMI only"
    docker rm -f "${COMPOSER_CONTAINER}" >/dev/null 2>&1 || true
    start_composer
    load_demo_artifacts
    cat <<EOF

Composer reloaded without restarting vLLM, the assistant bridge, or OpenShell.
Composer UI: ${COMPOSER_URL}
Viser HMI:    http://127.0.0.1:8081
EOF
}

restart_bridge() {
    ensure_dirs
    step "Restarting ManyForge assistant bridge only"
    stop_bridge
    start_bridge
    print_json_if_available "${BRIDGE_URL}/healthz"
}

start_all() {
    ensure_dirs
    preflight
    start_model
    configure_nemoclaw
    # Order matters here. The OpenClaw provisioner's pre-check probes
    # ${COMPOSER_URL}/api/assistant/modes/${ASSISTANT_MODE} for a live
    # `catalogHash` AND it does the same probe from inside the sandbox
    # via kubectl exec. Both probes fail unless:
    #   (a) Composer is running (otherwise: connection refused), AND
    #   (b) a deployment manifest with assistant_modes is loaded
    #       (otherwise: HTTP 404 on /api/assistant/modes/<mode>).
    # That means start_composer AND load_demo_artifacts must run
    # before provision_openclaw_sandbox. The original order (provision
    # before composer) only ever worked when both Composer and the
    # deployment were already up from a prior session — a cold-start
    # trap (verified 2026-05-08). Bridge starts last because it relies
    # on the OpenClaw gateway provisioned in the previous step.
    start_composer
    load_demo_artifacts
    provision_openclaw_sandbox
    start_bridge
    status
    cat <<EOF

Known-good assistant demo is ready.
Composer UI:  ${COMPOSER_URL}
Viser HMI:     http://127.0.0.1:8081
Bridge health: ${BRIDGE_URL}/healthz
Assistant provider: ${ASSISTANT_PROVIDER}
Model profile: ${MODEL_PROFILE}
Program:       ${DEMO_PROGRAM_PATH}
Deployment:    ${DEMO_DEPLOYMENT_PATH}

Open Composer, click the assistant button in the lower-right corner, and use:
  Query: Is a program loaded? Read the current program and summarize it.
  Composer Assistant: Add a small blue box named ui_demo_box at x=0.42, y=-0.12, z=0.18.
EOF
}

smoke() {
    stop_bridge
    local rc=0
    (cd "${MANYFORGE_ROOT}" && ./scripts/smoke-assistant-e2e.py \
        --profile "${SAMPLING_PROFILE}" \
        --deployment-path "${DEMO_DEPLOYMENT_PATH}" \
        --program-path "${DEMO_PROGRAM_PATH}" \
        --require-hmi \
        --strict \
        --output "${LOG_DIR}/known-good-assistant-demo.json") || rc=$?
    start_bridge || true
    return "${rc}"
}

status() {
    echo "Containers:"
    docker ps --format '  {{.Names}}\t{{.Image}}\t{{.Ports}}' \
        | grep -E "(${VLLM_CONTAINER}|${COMPOSER_CONTAINER}|openshell|nemoclaw)" || true
    echo
    echo "Listeners:"
    ss -ltnp | grep -E ':(8000|8080|8081|8100|9000)\b' || true
    echo
    echo "Preflight:"
    preflight || true
    echo
    print_json_if_available "${UPSTREAM_BASE_URL}/models"
    print_json_if_available "${COMPOSER_URL}/api/infra/status"
    print_json_if_available "${BRIDGE_URL}/healthz"
}

stop_bridge() {
    if [[ -f "${BRIDGE_PID_FILE}" ]]; then
        local pid
        pid="$(cat "${BRIDGE_PID_FILE}")"
        if [[ -n "${pid}" ]]; then
            kill "${pid}" >/dev/null 2>&1 || true
        fi
        rm -f "${BRIDGE_PID_FILE}"
    fi
    # Reap whichever lane was running. Both names match unique processes.
    pkill -f '[m]anyforge_assistant_bridge.bridge' 2>/dev/null || true
    pkill -f '[o]penclaw_assistant_bridge.service' 2>/dev/null || true
}

stop_all() {
    stop_bridge
    docker rm -f "${COMPOSER_CONTAINER}" >/dev/null 2>&1 || true
    docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true
    if [[ "${PRESERVE_OPENSHELL}" == "true" ]]; then
        echo "Preserving openshell-cluster-nemoclaw; it owns live sandbox state."
    else
        echo "Removing openshell-cluster-nemoclaw; sandbox state in that gateway will be deleted."
        docker rm -f openshell-cluster-nemoclaw >/dev/null 2>&1 || true
    fi
    drop_caches
    free -h
}

case "${1:-}" in
    start) start_all ;;
    restart-bridge) restart_bridge ;;
    reload-composer|restart-composer) reload_composer ;;
    smoke) smoke ;;
    preflight) preflight ;;
    status) status ;;
    stop) stop_all ;;
    restart) stop_all; start_all ;;
    -h|--help|help|"") usage ;;
    *)
        usage >&2
        exit 2
        ;;
esac
