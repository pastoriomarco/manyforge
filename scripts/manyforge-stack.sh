#!/usr/bin/env bash
# manyforge-stack.sh — Legacy mux/plugin stack helper
#
# Usage:
#   ./scripts/manyforge-stack.sh up                     # legacy ManyForge only
#   ./scripts/manyforge-stack.sh up --with-nemoclaw    # legacy mux/plugin stack
#   ./scripts/manyforge-stack.sh down [--stop-container]
#   ./scripts/manyforge-stack.sh status
#   ./scripts/manyforge-stack.sh smoke [direct|mux|all]
#   ./scripts/manyforge-stack.sh plugin <list|status|deploy|rollback>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
source "${SCRIPT_DIR}/manyforge-direct-route-common.sh"

MANYFORGE_PORT="${MANYFORGE_PORT:-9000}"
MUX_PROXY_PORT="${MUX_PROXY_PORT:-8888}"
DIRECT_ROUTE_PORT="${MANYFORGE_DIRECT_ROUTE_PORT:-$(manyforge_direct_route_port)}"
CONTAINER_PREFIX="${CONTAINER_PREFIX:-manyforge-dev}"
PLUGIN_ARTIFACT_DIR="${MANYFORGE_PLUGIN_ARTIFACT_DIR:-$HOME/.manyforge/plugin-artifacts}"
PLUGIN_STATE_DIR="${MANYFORGE_PLUGIN_STATE_DIR:-$HOME/.manyforge/plugin-state}"
SMOKE_STATE_DIR="${SMOKE_STATE_DIR:-$HOME/.manyforge/smoke}"

# ── Terminal colors ────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; NC=''
fi

pass()   { echo -e "${GREEN}  ✓${NC}  $*"; }
fail()   { echo -e "${RED}  ✗${NC}  $*"; }
warn()   { echo -e "${YELLOW}  !${NC}  $*"; }
info()   { echo -e "       $*"; }
header() { echo -e "\n${BOLD}$*${NC}"; }

# ── Helpers ───────────────────────────────────────────────────────────────────

find_container() {
    docker ps --filter "name=${CONTAINER_PREFIX}" --format '{{.Names}}' 2>/dev/null | head -1
}

container_is_running() {
    local name
    name=$(find_container)
    [[ -n "${name}" ]]
}

composer_is_healthy() {
    curl -sf --max-time 3 "http://localhost:${MANYFORGE_PORT}/api/health" >/dev/null 2>&1
}

mux_proxy_is_running() {
    pgrep -f "manyforge-mux-proxy.py" >/dev/null 2>&1
}

thor_runtime_summary_json() {
    local nemoclaw_thor="${NEMOCLAW_THOR:-$HOME/workspaces/dev_ws/src/NemoClaw-Thor}"
    if [[ ! -f "${nemoclaw_thor}/serving/config.sh" ]]; then
        return 1
    fi
    # shellcheck source=/dev/null
    source "${nemoclaw_thor}/serving/config.sh"
    # shellcheck source=/dev/null
    source "${nemoclaw_thor}/setup/sandbox-runtime.sh"
    load_thor_runtime_config ""
    local sandbox_name="${THOR_MANAGED_SANDBOX_NAME:-thor-v5}"
    sandbox_runtime_config_summary_json "${sandbox_name}" 2>/dev/null || true
}

provider_base_url() {
    local nemoclaw_thor="${NEMOCLAW_THOR:-$HOME/workspaces/dev_ws/src/NemoClaw-Thor}"
    if [[ ! -f "${nemoclaw_thor}/serving/config.sh" ]]; then
        return 1
    fi
    env -u THOR_LOCAL_VLLM_BASE_URL -u THOR_MANYFORGE_MUX_ENABLED bash -lc \
        "source '${nemoclaw_thor}/serving/config.sh' && load_thor_runtime_config '' >/dev/null 2>&1 && printf '%s\n' \"\${THOR_LOCAL_VLLM_BASE_URL:-}\""
}

list_versioned_plugin_archives() {
    if [[ ! -d "${PLUGIN_ARTIFACT_DIR}" ]]; then
        return 0
    fi
    find "${PLUGIN_ARTIFACT_DIR}" -maxdepth 1 -type f -name 'manyforge-mcp-*.tar.gz' ! -name 'manyforge-mcp-latest.tar.gz' -printf '%T@ %p\n' 2>/dev/null \
        | sort -nr \
        | cut -d' ' -f2-
}

current_plugin_metadata_json() {
    if [[ -f "${PLUGIN_STATE_DIR}/manyforge-mcp-current.json" ]]; then
        cat "${PLUGIN_STATE_DIR}/manyforge-mcp-current.json"
    fi
}

latest_smoke_json() {
    if [[ -f "${SMOKE_STATE_DIR}/latest.json" ]]; then
        cat "${SMOKE_STATE_DIR}/latest.json"
    fi
}

# ── Commands ──────────────────────────────────────────────────────────────────

cmd_up() {
    local with_nemoclaw=false

    for arg in "$@"; do
        case "${arg}" in
            --with-nemoclaw) with_nemoclaw=true ;;
        esac
    done

    echo -e "\n${BOLD}=== ManyForge Stack ===${NC}"

    # ── [1] Container ──
    header ">>> [1/$(${with_nemoclaw} && echo 3 || echo 1)] ManyForge container"

    local container_name
    container_name=$(find_container)

    if [[ -n "${container_name}" ]]; then
        info "Container ${container_name} already running"
    else
        info "Starting ManyForge container..."
        container_name="${CONTAINER_PREFIX}-mcp"
        docker run -d \
            --name "${container_name}" \
            -v "${MANYFORGE_ROOT}:/workspace" \
            -w /workspace \
            -p "${MANYFORGE_PORT}:${MANYFORGE_PORT}" \
            manyforge-dev \
            bash -lc "python -m manyforge_composer \
                --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
                --host 0.0.0.0 --port ${MANYFORGE_PORT} \
                $(${with_nemoclaw} && echo '--mcp-http') \
                2>&1" \
            >/dev/null 2>&1
        sleep 3
    fi

    # Wait for health
    local attempts=0
    while ! composer_is_healthy && [[ $attempts -lt 15 ]]; do
        sleep 2
        attempts=$((attempts + 1))
    done

    if composer_is_healthy; then
        pass "http://localhost:${MANYFORGE_PORT}/api/health — healthy"
    else
        fail "Composer did not become healthy"
        info "Check logs: docker logs ${container_name}"
        exit 1
    fi

    if ! ${with_nemoclaw}; then
        echo -e "\n${BOLD}=== Stack ready (deterministic mode) ===${NC}"
        echo "  ManyForge:  http://localhost:${MANYFORGE_PORT}"
        echo "  No NemoClaw integration (use --with-nemoclaw to enable)"
        return
    fi

    # ── [2] NemoClaw prerequisites ──
    header ">>> [2/3] NemoClaw prerequisites"

    local nemoclaw_thor="${NEMOCLAW_THOR:-$HOME/workspaces/dev_ws/src/NemoClaw-Thor}"
    if [[ ! -f "${nemoclaw_thor}/serving/config.sh" ]]; then
        fail "NemoClaw-Thor not found at ${nemoclaw_thor}"
        exit 1
    fi

    source "${nemoclaw_thor}/serving/config.sh"
    load_thor_runtime_config ""
    pass "Config loaded from $(thor_config_file)"

    local sandbox_name="${THOR_MANAGED_SANDBOX_NAME:-thor-v5}"
    local sandbox_phase
    sandbox_phase=$(openshell sandbox list 2>/dev/null | grep "${sandbox_name}" | awk '{print $NF}')
    sandbox_phase=$(echo "${sandbox_phase}" | sed 's/\x1b\[[0-9;]*m//g')
    if [[ "${sandbox_phase}" == "Ready" ]]; then
        pass "Sandbox: ${sandbox_name} (Ready)"
    else
        fail "Sandbox: ${sandbox_name} (${sandbox_phase})"
        exit 1
    fi

    # Check vLLM
    if curl -sf --max-time 3 "http://localhost:8000/v1/models" >/dev/null 2>&1; then
        local model_id
        model_id=$(curl -sf "http://localhost:8000/v1/models" 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(d['data'][0]['id'] if d.get('data') else 'unknown')
except: print('unknown')
" 2>/dev/null)
        pass "vLLM: ${model_id} on port 8000"
    else
        warn "vLLM not reachable on port 8000 (agent LLM calls may fail)"
    fi

    info "Legacy dedicated-route / mux transport will be selected by configure-manyforge-provider.sh"

    # ── [3] Integration ──
    header ">>> [3/3] ManyForge ↔ NemoClaw integration"
    MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true \
        "${SCRIPT_DIR}/configure-manyforge-provider.sh"

    echo -e "\n${BOLD}=== Stack ready ===${NC}"
    echo "  ManyForge:  http://localhost:${MANYFORGE_PORT} (MCP HTTP enabled)"
    echo "  Sandbox:    ${sandbox_name}"
    echo ""
    echo "  Test:"
    echo "    openshell sandbox connect ${sandbox_name}"
    echo "    openclaw agent --local --json -m \"Reply with only the names of any tools available to you whose names start with manyforge_. If none are available, reply none.\""
}

cmd_down() {
    local stop_container=false

    for arg in "$@"; do
        case "${arg}" in
            --stop-container) stop_container=true ;;
        esac
    done

    header "Stopping ManyForge stack"

    # Stop mux proxy
    if mux_proxy_is_running; then
        pkill -f "manyforge-mux-proxy.py" 2>/dev/null || true
        pass "Mux proxy stopped"
    fi
    if manyforge_direct_route_proxy_is_running; then
        stop_manyforge_direct_route_proxy
        pass "Dedicated route proxy stopped"
    fi

    local container_name
    container_name=$(find_container)

    if [[ -z "${container_name}" ]]; then
        info "No ManyForge container running"
        return
    fi

    if ${stop_container}; then
        docker stop "${container_name}" >/dev/null 2>&1 || true
        docker rm "${container_name}" >/dev/null 2>&1 || true
        pass "Container ${container_name} stopped and removed"
    else
        # Just stop the Composer process
        docker exec "${container_name}" pkill -f "manyforge_composer" 2>/dev/null || true
        pass "Composer process stopped (container still running)"
    fi
}

cmd_status() {
    echo ""

    # ManyForge
    local container_name
    container_name=$(find_container)
    if [[ -n "${container_name}" ]] && composer_is_healthy; then
        echo -e "${GREEN}●${NC} ManyForge    healthy   http://localhost:${MANYFORGE_PORT}"
        echo "  Container:   ${container_name}"

        # Check MCP HTTP
        local tool_count
        tool_count=$(curl -sf --max-time 5 \
            -X POST "http://localhost:${MANYFORGE_PORT}/api/mcp" \
            -H "Content-Type: application/json" \
            -H "X-Principal-ID: system:status-check" \
            -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' 2>/dev/null | \
            python3 -c "import json,sys; print(len(json.load(sys.stdin).get('result',{}).get('tools',[])))" 2>/dev/null || echo "0")
        if [[ "${tool_count}" -gt 0 ]]; then
            echo "  MCP HTTP:    enabled   /api/mcp (${tool_count} tools)"
        else
            echo "  MCP HTTP:    disabled"
        fi
    elif [[ -n "${container_name}" ]]; then
        echo -e "${YELLOW}●${NC} ManyForge    container running but Composer unhealthy"
        echo "  Container:   ${container_name}"
    else
        echo -e "${RED}●${NC} ManyForge    not running"
    fi

    # Dedicated route proxy
    if manyforge_direct_route_proxy_is_running; then
        local route_container
        route_container="$(manyforge_direct_route_container_name)"
        echo "  Direct TLS:  experimental (${route_container}) on port ${DIRECT_ROUTE_PORT}"
    else
        echo "  Direct TLS:  experimental (not running)"
    fi

    # Mux proxy
    if mux_proxy_is_running; then
        local mux_pid
        mux_pid=$(pgrep -f "manyforge-mux-proxy.py" | head -1)
        echo "  Mux proxy:   running (PID ${mux_pid}) on port ${MUX_PROXY_PORT}"
        local mux_health
        mux_health=$(curl -sf --max-time 5 "http://localhost:${MUX_PROXY_PORT}/v1/models/manyforge/api/health" 2>/dev/null || echo "")
        if [[ -n "${mux_health}" ]]; then
            echo "  Mux health:  host round-trip OK"
        else
            echo "  Mux health:  host round-trip failed"
        fi
    else
        echo "  Mux proxy:   not running"
    fi

    # vLLM
    echo ""
    if curl -sf --max-time 3 "http://localhost:8000/v1/models" >/dev/null 2>&1; then
        local model_id
        model_id=$(curl -sf "http://localhost:8000/v1/models" 2>/dev/null | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(d['data'][0]['id'] if d.get('data') else 'unknown')
except: print('unknown')
" 2>/dev/null)
        echo -e "${GREEN}●${NC} vLLM         healthy   http://localhost:8000/v1"
        echo "  Model:       ${model_id}"
    else
        echo -e "${RED}●${NC} vLLM         not reachable on port 8000"
    fi

    # Sandbox
    echo ""
    local sandbox_name="${THOR_MANAGED_SANDBOX_NAME:-thor-v5}"
    local sandbox_phase
    sandbox_phase=$(openshell sandbox list 2>/dev/null | grep "${sandbox_name}" | awk '{print $NF}' || echo "Unknown")
    sandbox_phase=$(echo "${sandbox_phase}" | sed 's/\x1b\[[0-9;]*m//g')

    if [[ "${sandbox_phase}" == "Ready" ]]; then
        echo -e "${GREEN}●${NC} Sandbox      healthy   ${sandbox_name} (Ready)"

        # Check workspace files
        for fname in AGENTS.md SOUL.md; do
            if openshell sandbox exec -n "${sandbox_name}" -- test -f "/sandbox/${fname}" 2>/dev/null; then
                echo "  ${fname}:$(printf '%*s' $((13 - ${#fname})) '')present"
            else
                echo "  ${fname}:$(printf '%*s' $((13 - ${#fname})) '')missing"
            fi
        done

        # Check skills
        local skill_count
        skill_count=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -c 'ls -d /sandbox/.agents/skills/*/ 2>/dev/null | wc -l' 2>/dev/null || echo "0")
        echo "  Skills:      ${skill_count} installed"

        # Check workspace plugin discovery
        local plugin_state
        plugin_state=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'HOME=/sandbox openclaw plugins list 2>/dev/null | grep -q "manyforge-mcp" && echo "loaded" || echo "missing"' \
            2>/dev/null || echo "missing")
        echo "  Plugin:      manyforge-mcp ${plugin_state}"

        # Check gateway using the OpenClaw status surface rather than process lookup
        local gateway_status
        gateway_status=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'HOME=/sandbox openclaw gateway status 2>/dev/null || true' \
            2>/dev/null || true)
        if printf '%s\n' "${gateway_status}" | grep -q "RPC probe: ok"; then
            echo "  Gateway:     running (RPC probe OK)"
        elif [[ -n "${gateway_status}" ]]; then
            echo "  Gateway:     degraded"
        else
            echo "  Gateway:     unknown"
        fi

        local plugin_info
        plugin_info=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'HOME=/sandbox openclaw plugins info manyforge-mcp 2>/dev/null || true' \
            2>/dev/null || true)
        if [[ -n "${plugin_info}" ]]; then
            local plugin_origin plugin_version plugin_install install_path
            plugin_origin=$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Origin:/ {print $2; exit}')
            plugin_version=$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Version:/ {print $2; exit}')
            plugin_install=$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Install:/ {print $2; exit}')
            install_path=$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Install path:/ {print $2; exit}')
            echo "  Plugin info: origin=${plugin_origin:-unknown} version=${plugin_version:-unknown}"
            if [[ -n "${plugin_install}" ]]; then
                echo "  Plugin install: ${plugin_install} ${install_path:+(${install_path})}"
            fi
        fi

        local artifact_metadata
        artifact_metadata=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'cat /sandbox/.manyforge/plugins/manyforge-mcp-current.json 2>/dev/null || true' \
            2>/dev/null || true)
        if [[ -n "${artifact_metadata}" ]]; then
            local artifact_version artifact_sha artifact_name
            artifact_version=$(printf '%s' "${artifact_metadata}" | python3 -c 'import json,sys; data=json.load(sys.stdin); print(data.get("plugin_version",""))' 2>/dev/null || true)
            artifact_sha=$(printf '%s' "${artifact_metadata}" | python3 -c 'import json,sys; data=json.load(sys.stdin); print(data.get("artifact_sha256",""))' 2>/dev/null || true)
            artifact_name=$(printf '%s' "${artifact_metadata}" | python3 -c 'import json,sys; data=json.load(sys.stdin); print(data.get("archive_name",""))' 2>/dev/null || true)
            if [[ -n "${artifact_version}" ]]; then
                echo "  Artifact:    ${artifact_name:-current} version=${artifact_version} sha=${artifact_sha:0:12}"
            fi
        fi

        local sandbox_base_url sandbox_api
        sandbox_base_url=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'python3 -c "import json,pathlib; config=json.loads(pathlib.Path(\"/sandbox/.openclaw/openclaw.json\").read_text()); print(config.get(\"models\", {}).get(\"providers\", {}).get(\"inference\", {}).get(\"baseUrl\", \"\"))"' \
            2>/dev/null || echo "")
        sandbox_api=$(openshell sandbox exec -n "${sandbox_name}" -- \
            bash -lc 'python3 -c "import json,pathlib; config=json.loads(pathlib.Path(\"/sandbox/.openclaw/openclaw.json\").read_text()); print(config.get(\"models\", {}).get(\"providers\", {}).get(\"inference\", {}).get(\"api\", \"\"))"' \
            2>/dev/null || echo "")
        if [[ -n "${sandbox_base_url}" ]]; then
            echo "  Sandbox URL: ${sandbox_base_url}"
        fi
        if [[ -n "${sandbox_api}" ]]; then
            echo "  Sandbox API: ${sandbox_api}"
        fi

        local provider_url
        provider_url="$(provider_base_url || true)"
        if [[ -n "${provider_url}" ]]; then
            echo "  Provider URL: ${provider_url}"
        fi

        if mux_proxy_is_running; then
            local mux_probe
            mux_probe=$(openshell sandbox exec -n "${sandbox_name}" -- \
                bash -lc 'curl -sf --max-time 10 https://inference.local/v1/models/manyforge/api/health' \
                2>/dev/null || echo "")
            if [[ -n "${mux_probe}" ]]; then
                echo "  Sandbox mux: inference.local → ManyForge OK"
            else
                echo "  Sandbox mux: inference.local → ManyForge failed"
            fi
        fi

        local smoke_json
        smoke_json="$(latest_smoke_json || true)"
        if [[ -n "${smoke_json}" ]]; then
            local smoke_status smoke_mode smoke_timestamp
            smoke_status=$(printf '%s' "${smoke_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("status",""))' 2>/dev/null || true)
            smoke_mode=$(printf '%s' "${smoke_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("mode",""))' 2>/dev/null || true)
            smoke_timestamp=$(printf '%s' "${smoke_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("timestamp_utc",""))' 2>/dev/null || true)
            if [[ -n "${smoke_status}" ]]; then
                echo "  Last smoke:  ${smoke_status} (${smoke_mode:-unknown}) at ${smoke_timestamp:-unknown}"
            fi
        fi
    else
        echo -e "${RED}●${NC} Sandbox      ${sandbox_name} (${sandbox_phase})"
    fi

    echo ""
}

cmd_smoke() {
    shift || true
    "${SCRIPT_DIR}/manyforge-openclaw-smoke.sh" "${1:-all}"
}

cmd_plugin() {
    local action="${2:-status}"
    case "${action}" in
        list)
            header "ManyForge Plugin Artifacts"
            if [[ ! -d "${PLUGIN_ARTIFACT_DIR}" ]]; then
                fail "Artifact directory not found: ${PLUGIN_ARTIFACT_DIR}"
                return 1
            fi
            local latest_json
            latest_json="$(cat "${PLUGIN_ARTIFACT_DIR}/manyforge-mcp-latest.json" 2>/dev/null || true)"
            if [[ -n "${latest_json}" ]]; then
                echo "Latest:"
                printf '%s' "${latest_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("  {}  version={}  sha={}".format(d["archive_name"], d["plugin_version"], d["artifact_sha256"][:12]))'
            fi
            echo "History:"
            local archives
            archives="$(list_versioned_plugin_archives || true)"
            if [[ -z "${archives}" ]]; then
                echo "  none"
            else
                while IFS= read -r archive; do
                    [[ -n "${archive}" ]] || continue
                    local meta="${archive%.tar.gz}.json"
                    if [[ -f "${meta}" ]]; then
                        python3 - "${meta}" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
print("  {}  version={}  sha={}".format(d["archive_name"], d["plugin_version"], d["artifact_sha256"][:12]))
PY
                    else
                        echo "  $(basename "${archive}")"
                    fi
                done <<< "${archives}"
            fi
            ;;
        status)
            header "ManyForge Plugin Runtime"
            local current_json
            current_json="$(current_plugin_metadata_json || true)"
            if [[ -n "${current_json}" ]]; then
                printf '%s' "${current_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("Current artifact: {}  version={}  sha={}".format(d.get("archive_name","unknown"), d.get("plugin_version","unknown"), d.get("artifact_sha256","")[:12]))'
            else
                echo "Current artifact: unknown"
            fi
            local smoke_json
            smoke_json="$(latest_smoke_json || true)"
            if [[ -n "${smoke_json}" ]]; then
                printf '%s' "${smoke_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print("Last smoke: {}  mode={}  at={}".format(d.get("status","unknown"), d.get("mode","unknown"), d.get("timestamp_utc","unknown")))'
            else
                echo "Last smoke: unknown"
            fi
            ;;
        deploy)
            shift 2 || true
            if [[ "${1:-}" == "--artifact" ]]; then
                if [[ -z "${2:-}" ]]; then
                    fail "--artifact requires a path"
                    return 1
                fi
                MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true \
                    "${SCRIPT_DIR}/configure-manyforge-provider.sh" --artifact "${2}"
            elif [[ "${1:-}" == "--use-current-artifact" ]]; then
                MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true \
                    "${SCRIPT_DIR}/configure-manyforge-provider.sh" --use-current-artifact
            else
                MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true \
                    "${SCRIPT_DIR}/configure-manyforge-provider.sh"
            fi
            ;;
        rollback)
            local steps="${3:-1}"
            local archives=()
            while IFS= read -r archive; do
                [[ -n "${archive}" ]] && archives+=("${archive}")
            done < <(list_versioned_plugin_archives)
            if [[ "${#archives[@]}" -eq 0 ]]; then
                fail "No versioned plugin artifacts available for rollback"
                return 1
            fi
            local current_json current_archive_name current_index target_index target_archive
            current_json="$(current_plugin_metadata_json || true)"
            current_archive_name=""
            if [[ -n "${current_json}" ]]; then
                current_archive_name="$(printf '%s' "${current_json}" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("archive_name",""))' 2>/dev/null || true)"
            fi
            current_index=0
            if [[ -n "${current_archive_name}" ]]; then
                local i
                for i in "${!archives[@]}"; do
                    if [[ "$(basename "${archives[$i]}")" == "${current_archive_name}" ]]; then
                        current_index="${i}"
                        break
                    fi
                done
            fi
            target_index=$((current_index + steps))
            if [[ "${#archives[@]}" -le "${target_index}" ]]; then
                fail "Not enough versioned plugin artifacts for rollback from current index ${current_index} by ${steps} step(s)"
                return 1
            fi
            target_archive="${archives[${target_index}]}"
            info "Rolling back to $(basename "${target_archive}")"
            MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true \
                "${SCRIPT_DIR}/configure-manyforge-provider.sh" --artifact "${target_archive}"
            ;;
        *)
            echo "Usage: $0 plugin {list|status|deploy [--artifact PATH|--use-current-artifact]|rollback [steps]}" >&2
            return 1
            ;;
    esac
}

# ── Main ──────────────────────────────────────────────────────────────────────

case "${1:-}" in
    up)
        shift
        cmd_up "$@"
        ;;
    down)
        shift
        cmd_down "$@"
        ;;
    status)
        cmd_status
        ;;
    smoke)
        cmd_smoke "$@"
        ;;
    plugin)
        cmd_plugin "$@"
        ;;
    *)
        echo "Usage: $0 {up [--with-nemoclaw] | down [--stop-container] | status | smoke [direct|mux|all] | plugin {list|status|deploy [--artifact PATH|--use-current-artifact]|rollback [steps]}}"
        exit 1
        ;;
esac
