#!/usr/bin/env bash
# configure-manyforge-provider.sh — Wire ManyForge into a NemoClaw-Thor sandbox
#
# Prerequisites:
#   - NemoClaw-Thor configured: ./setup/configure-local-provider.sh already run
#   - ManyForge container running with Composer on port 9000 (--mcp-http)
#
# Usage:
#   ./scripts/configure-manyforge-provider.sh
#
# Integration paths:
#   preferred: sandbox main agent → installed OpenClaw plugin → direct HTTPS route on
#              https://host.openshell.internal/api/mcp
#   fallback:  sandbox main agent → installed OpenClaw plugin → inference.local →
#              mux-proxy → ManyForge
#
# Notes:
#   - We do not depend on root-level `mcp.servers` in openclaw.json for the
#     embedded main agent; this OpenClaw build rejects that config shape.
#   - The direct host HTTPS route remains experimental and is skipped by
#     default. Set MANYFORGE_ENABLE_DIRECT_ROUTE_EXPERIMENT=true to probe it.
#   - The muxed `inference.local` path remains the supported transport until a
#     direct route is reproducible from the actual sandbox process.
#
# What it does:
#   1. Verifies ManyForge Composer is reachable on host
#   2. Optionally applies ManyForge network policy for future direct host access
#   3. Ensures the mux proxy is running on the host
#   4. Builds and installs the pinned OpenClaw plugin artifact
#   5. Uploads AGENTS.md, SOUL.md, skills
#   6. Restarts OpenClaw gateway
#   7. Verifies plugin discovery (live tool use is validated separately)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ── Source NemoClaw-Thor libs ──────────────────────────────────────────────────

NEMOCLAW_THOR="${NEMOCLAW_THOR:-$HOME/workspaces/dev_ws/src/NemoClaw-Thor}"

if [[ ! -f "${NEMOCLAW_THOR}/setup/checks.sh" ]]; then
    echo "ERROR: NemoClaw-Thor not found at ${NEMOCLAW_THOR}" >&2
    echo "Set NEMOCLAW_THOR to the NemoClaw-Thor directory." >&2
    exit 1
fi

source "${NEMOCLAW_THOR}/setup/checks.sh"
source "${NEMOCLAW_THOR}/serving/config.sh"
source "${NEMOCLAW_THOR}/setup/sandbox-runtime.sh"
source "${SCRIPT_DIR}/manyforge-direct-route-common.sh"

push_tarball_to_sandbox() {
    local sandbox_name="$1"
    local tar_root="$2"
    local dest_cmd="$3"
    shift 3

    local tmp_tar
    tmp_tar="$(mktemp)"
    tar -C "${tar_root}" -cf "${tmp_tar}" "$@"
    openshell sandbox exec -n "${sandbox_name}" -- bash -c "${dest_cmd}" < "${tmp_tar}"
    rm -f "${tmp_tar}"
}

resolve_plugin_metadata_path() {
    local archive_path="$1"
    if [[ "${archive_path}" == *"/manyforge-mcp-latest.tar.gz" ]]; then
        printf '%s\n' "${archive_path%.tar.gz}.json"
        return 0
    fi
    printf '%s\n' "${archive_path%.tar.gz}.json"
}

generate_plugin_metadata_json_from_archive() {
    local archive_path="$1"
    python3 - "${archive_path}" <<'PY'
import hashlib
import json
import pathlib
import sys
import tarfile

archive = pathlib.Path(sys.argv[1]).resolve()
with tarfile.open(archive, "r:gz") as tar:
    plugin_manifest = json.loads(tar.extractfile("manyforge-mcp/openclaw.plugin.json").read().decode("utf-8"))
    package_manifest = json.loads(tar.extractfile("manyforge-mcp/package.json").read().decode("utf-8"))
    readme = tar.extractfile("manyforge-mcp/README.md").read().decode("utf-8")
tool_count = 0
for line in readme.splitlines():
    if line.startswith("- Tool count:"):
        tool_count = int(line.split("`")[1])
        break
artifact_sha = hashlib.sha256(archive.read_bytes()).hexdigest()
print(json.dumps({
    "plugin_id": plugin_manifest["id"],
    "plugin_version": plugin_manifest["version"],
    "tool_count": tool_count,
    "surface_sha256": "",
    "artifact_sha256": artifact_sha,
    "archive_name": archive.name,
    "archive_path": str(archive),
    "mcp_endpoint": "",
}))
PY
}

extract_plugin_stage_dir() {
    local archive_path="$1"
    local extract_root
    extract_root="$(mktemp -d)"
    tar -xzf "${archive_path}" -C "${extract_root}"
    printf '%s\n' "${extract_root}/manyforge-mcp"
}

# ── Args ──────────────────────────────────────────────────────────────────────

CONFIGURE_BUILD_PLUGIN="true"
CONFIGURE_PLUGIN_ARCHIVE_PATH=""
CONFIGURE_USE_CURRENT_ARTIFACT="false"
PLUGIN_STAGE_ROOT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --artifact)
            CONFIGURE_BUILD_PLUGIN="false"
            CONFIGURE_PLUGIN_ARCHIVE_PATH="${2:-}"
            shift 2
            ;;
        --use-current-artifact|--no-build)
            CONFIGURE_BUILD_PLUGIN="false"
            CONFIGURE_USE_CURRENT_ARTIFACT="true"
            shift
            ;;
        *)
            echo "Usage: $0 [--artifact /path/to/plugin.tar.gz] [--use-current-artifact]" >&2
            exit 1
            ;;
    esac
done

cleanup_temp_dirs() {
    if [[ -n "${PLUGIN_STAGE_ROOT}" && -d "${PLUGIN_STAGE_ROOT}" ]]; then
        rm -rf "${PLUGIN_STAGE_ROOT}"
    fi
}
trap cleanup_temp_dirs EXIT

# ── Load config ───────────────────────────────────────────────────────────────

load_thor_runtime_config ""
SANDBOX_NAME="${THOR_MANAGED_SANDBOX_NAME:-thor-v5}"
MANYFORGE_PORT="${MANYFORGE_PORT:-9000}"
MUX_PROXY_PORT="${MUX_PROXY_PORT:-8888}"
DIRECT_ROUTE_PORT="${MANYFORGE_DIRECT_ROUTE_PORT:-$(manyforge_direct_route_port)}"
MANYFORGE_HMI_PORT="${MANYFORGE_HMI_PORT:-8081}"
THOR_INFERENCE_ROUTE_TIMEOUT_SECONDS="${THOR_INFERENCE_ROUTE_TIMEOUT_SECONDS:-180}"
MANYFORGE_ENABLE_DIRECT_ROUTE_EXPERIMENT="${MANYFORGE_ENABLE_DIRECT_ROUTE_EXPERIMENT:-false}"
MANYFORGE_PLUGIN_ARTIFACT_DIR="${MANYFORGE_PLUGIN_ARTIFACT_DIR:-$HOME/.manyforge/plugin-artifacts}"
MANYFORGE_PLUGIN_STATE_DIR="${MANYFORGE_PLUGIN_STATE_DIR:-$HOME/.manyforge/plugin-state}"
mkdir -p "${MANYFORGE_PLUGIN_STATE_DIR}"

header "Configuring ManyForge provider for sandbox ${SANDBOX_NAME}"

# ── Step 1: Verify ManyForge Composer ──────────────────────────────────────────

info "Checking ManyForge Composer at http://localhost:${MANYFORGE_PORT}..."
mf_health=$(curl -sf --max-time 5 "http://localhost:${MANYFORGE_PORT}/api/health" 2>/dev/null || echo "")
if [[ -z "$mf_health" ]]; then
    fail "ManyForge Composer not reachable at port ${MANYFORGE_PORT}"
    fix "Start ManyForge: ./scripts/manyforge-stack.sh up --with-nemoclaw"
    exit 1
fi
pass "ManyForge Composer healthy"

# Verify MCP HTTP is enabled
mcp_check=$(curl -sf --max-time 5 \
    -X POST "http://localhost:${MANYFORGE_PORT}/api/mcp" \
    -H "Content-Type: application/json" \
    -H "X-Principal-ID: system:configure" \
    -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' 2>/dev/null || echo "")
if [[ -z "$mcp_check" ]]; then
    fail "MCP HTTP transport not enabled on ManyForge"
    fix "Restart ManyForge with --mcp-http flag"
    exit 1
fi
host_tool_count=$(echo "${mcp_check}" | python3 -c "
import json, sys
try:
    d = json.load(sys.stdin)
    print(len(d.get('result', {}).get('tools', [])))
except: print('0')
" 2>/dev/null)
pass "MCP HTTP enabled (${host_tool_count} tools)"

# ── Step 2: Verify sandbox ───────────────────────────────────────────────────

info "Checking sandbox ${SANDBOX_NAME}..."
sandbox_phase=$(openshell sandbox list 2>/dev/null | grep "${SANDBOX_NAME}" | awk '{print $NF}')
sandbox_phase=$(echo "${sandbox_phase}" | sed 's/\x1b\[[0-9;]*m//g')
if [[ "${sandbox_phase}" != "Ready" ]]; then
    fail "Sandbox ${SANDBOX_NAME} not Ready (phase: ${sandbox_phase})"
    exit 1
fi
pass "Sandbox ${SANDBOX_NAME} is Ready"

# ── Step 3: Apply network policy (best effort, future direct path) ──────────

info "Merging ManyForge network policy..."
current_policy=$(openshell policy get "${SANDBOX_NAME}" --full 2>/dev/null || echo "")
# Extract current policy YAML (after the --- separator)
current_yaml=$(echo "${current_policy}" | sed '1,/^---$/d')

python3 -c "
import yaml, sys, pathlib

current_text = '''${current_yaml}'''
current = yaml.safe_load(current_text) if current_text.strip() else {}
fragment = yaml.safe_load(pathlib.Path('${SCRIPT_DIR}/templates/manyforge-policy-fragment.yaml').read_text())

if 'network_policies' not in current:
    current['network_policies'] = {}
current['network_policies'].update(fragment.get('network_policies', {}))

pathlib.Path('/tmp/manyforge-merged-policy.yaml').write_text(
    yaml.dump(current, default_flow_style=False))
" 2>/dev/null

openshell policy set "${SANDBOX_NAME}" --policy /tmp/manyforge-merged-policy.yaml --wait --timeout 30 2>/dev/null || true
rm -f /tmp/manyforge-merged-policy.yaml

updated_policy=$(openshell policy get "${SANDBOX_NAME}" --full 2>/dev/null || echo "")
if echo "${updated_policy}" | grep -q "port: 443"; then
    pass "ManyForge policy applied (includes direct TLS route)"
else
    warn "ManyForge policy update did not expose port 443; sandbox may remain on mux fallback"
fi

# ── Step 4: Prefer dedicated route, fall back to mux ────────────────────────

PLUGIN_ENDPOINT=""
PLUGIN_BEARER_TOKEN=""
PLUGIN_CA_CERT_PATH=""
TRANSPORT_MODE="mux"

info "Ensuring dedicated ManyForge TLS route is available..."
openshell sandbox exec -n "${SANDBOX_NAME}" -- mkdir -p /sandbox/.manyforge/certs 2>/dev/null || true

# Keep the bridge script available for manual diagnostics during the transition,
# but do not register ACPX mcpServers for the embedded main agent.
push_tarball_to_sandbox "${SANDBOX_NAME}" "${SCRIPT_DIR}" \
    'cd /sandbox/.manyforge && tar xf -' manyforge-mcp-bridge.py 2>/dev/null
pass "Diagnostic MCP bridge script deployed"

if [[ "${MANYFORGE_ENABLE_DIRECT_ROUTE_EXPERIMENT,,}" == "true" ]]; then
    if start_manyforge_direct_route_proxy "${SCRIPT_DIR}"; then
        pass "Dedicated route proxy running on port ${DIRECT_ROUTE_PORT}"
    else
        warn "Dedicated route proxy failed to start; will fall back to mux transport"
    fi

    if manyforge_direct_route_proxy_is_running; then
        direct_route_host_health=$(curl -sf --max-time 5 \
            --cacert "$(manyforge_direct_route_ca_cert_path)" \
            -H "Authorization: Bearer ${MANYFORGE_DIRECT_ROUTE_TOKEN}" \
            "https://localhost:${DIRECT_ROUTE_PORT}/api/health" 2>/dev/null || echo "")
        if [[ -n "${direct_route_host_health}" ]]; then
            pass "Dedicated route healthy on host"

            push_tarball_to_sandbox "${SANDBOX_NAME}" "$(manyforge_direct_route_dir)" \
                'mkdir -p /sandbox/.manyforge/certs && cd /sandbox/.manyforge/certs && tar xf -' \
                ca-cert.pem 2>/dev/null
            pass "Dedicated route CA bundle uploaded to sandbox"

            info "Probing dedicated route from the live sandbox..."
            direct_route_sandbox_health=$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
                bash -lc "curl -sf --max-time 10 --cacert /sandbox/.manyforge/certs/ca-cert.pem -H 'Authorization: Bearer ${MANYFORGE_DIRECT_ROUTE_TOKEN}' https://host.openshell.internal/api/health" \
                2>/dev/null || echo "")
            if [[ -n "${direct_route_sandbox_health}" ]]; then
                pass "Dedicated route reachable from sandbox"
                TRANSPORT_MODE="direct-route"
                PLUGIN_ENDPOINT="https://host.openshell.internal/api/mcp"
                PLUGIN_BEARER_TOKEN="${MANYFORGE_DIRECT_ROUTE_TOKEN}"
                PLUGIN_CA_CERT_PATH="/sandbox/.manyforge/certs/ca-cert.pem"
                THOR_MANYFORGE_MUX_ENABLED="false"
                THOR_LOCAL_VLLM_BASE_URL="http://host.openshell.internal:8000/v1"
                openshell provider update "${THOR_LOCAL_PROVIDER_NAME}" \
                    --config OPENAI_BASE_URL="${THOR_LOCAL_VLLM_BASE_URL}" 2>/dev/null
                pass "Provider ${THOR_LOCAL_PROVIDER_NAME} restored to direct vLLM (port 8000)"
                sync_sandbox_runtime_config "${SANDBOX_NAME}" 2>/dev/null
                pass "Sandbox OpenClaw config synced for dedicated-route mode"
            else
                warn "Dedicated route not reachable from sandbox; falling back to mux transport"
            fi
        else
            warn "Dedicated route host health probe failed; falling back to mux transport"
        fi
    fi
else
    info "Skipping experimental dedicated route probe; using mux baseline"
fi

if [[ "${TRANSPORT_MODE}" == "mux" ]]; then
    info "Ensuring muxed ManyForge transport is available..."
    if ! pgrep -f "manyforge-mux-proxy.py" >/dev/null 2>&1; then
        info "Starting mux proxy on port ${MUX_PROXY_PORT}..."
        python3 "${SCRIPT_DIR}/manyforge-mux-proxy.py" --port "${MUX_PROXY_PORT}" &
        sleep 1
        if pgrep -f "manyforge-mux-proxy.py" >/dev/null 2>&1; then
            pass "Mux proxy started"
        else
            fail "Mux proxy failed to start"
            exit 1
        fi
    else
        pass "Mux proxy already running"
    fi

    info "Repointing inference.local through the mux proxy..."
    THOR_MANYFORGE_MUX_ENABLED="true"
    THOR_LOCAL_VLLM_BASE_URL="http://host.openshell.internal:${MUX_PROXY_PORT}/v1"
    PLUGIN_ENDPOINT="https://inference.local/v1/completions"
    PLUGIN_BEARER_TOKEN=""
    PLUGIN_CA_CERT_PATH=""

    openshell provider update "${THOR_LOCAL_PROVIDER_NAME}" \
        --config OPENAI_BASE_URL="${THOR_LOCAL_VLLM_BASE_URL}" 2>/dev/null
    pass "Provider ${THOR_LOCAL_PROVIDER_NAME} repointed to mux proxy (port ${MUX_PROXY_PORT})"

    info "Syncing sandbox runtime config with muxed URL..."
    sync_sandbox_runtime_config "${SANDBOX_NAME}" 2>/dev/null
    pass "Sandbox OpenClaw config synced (baseUrl remains inference.local)"
fi

info "Refreshing inference route timeout budget..."
openshell inference set \
    --provider "${THOR_LOCAL_PROVIDER_NAME}" \
    --model "${THOR_MODEL_ID}" \
    --timeout "${THOR_INFERENCE_ROUTE_TIMEOUT_SECONDS}" \
    --no-verify >/dev/null
pass "Inference route timeout set to ${THOR_INFERENCE_ROUTE_TIMEOUT_SECONDS}s"

# Persist the converged transport mode as soon as provider and sandbox config
# are aligned. Later plugin/upload failures must not leave Thor status stale.
save_thor_runtime_config
pass "Transport config persisted to ${THOR_CONFIG_FILE}"

# ── Step 5: Build and install the supported OpenClaw plugin artifact ────────

plugin_metadata_json=""
plugin_archive_path=""
plugin_metadata_path=""
plugin_stage_dir=""

if [[ "${CONFIGURE_BUILD_PLUGIN}" == "true" ]]; then
    info "Building versioned OpenClaw plugin artifact from live ManyForge MCP tools..."
    plugin_metadata_json="$(python3 "${SCRIPT_DIR}/build-manyforge-openclaw-plugin.py" \
        --mcp-endpoint "http://localhost:${MANYFORGE_PORT}/api/mcp" \
        --output-dir "${MANYFORGE_PLUGIN_ARTIFACT_DIR}")"
    plugin_archive_path="$(printf '%s' "${plugin_metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["archive_path"])')"
    plugin_metadata_path="$(resolve_plugin_metadata_path "${plugin_archive_path}")"
    plugin_stage_dir="$(printf '%s' "${plugin_metadata_json}" | python3 -c 'import json,sys,os; data=json.load(sys.stdin); print(os.path.join(os.path.dirname(data["archive_path"]), data["plugin_id"]))')"
else
    if [[ -z "${CONFIGURE_PLUGIN_ARCHIVE_PATH}" ]]; then
        if [[ "${CONFIGURE_USE_CURRENT_ARTIFACT}" == "true" ]]; then
            plugin_archive_path="${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.tar.gz"
        else
            plugin_archive_path="${MANYFORGE_PLUGIN_ARTIFACT_DIR}/manyforge-mcp-latest.tar.gz"
        fi
    else
        plugin_archive_path="${CONFIGURE_PLUGIN_ARCHIVE_PATH}"
    fi
    if [[ ! -f "${plugin_archive_path}" ]]; then
        fail "Selected plugin archive not found: ${plugin_archive_path}"
        exit 1
    fi
    plugin_metadata_path="$(resolve_plugin_metadata_path "${plugin_archive_path}")"
    if [[ -f "${plugin_metadata_path}" ]]; then
        plugin_metadata_json="$(cat "${plugin_metadata_path}")"
    else
        warn "Selected plugin metadata not found; reconstructing from archive"
        plugin_metadata_json="$(generate_plugin_metadata_json_from_archive "${plugin_archive_path}")"
        printf '%s\n' "${plugin_metadata_json}" > "${plugin_metadata_path}"
    fi
    plugin_stage_dir="$(extract_plugin_stage_dir "${plugin_archive_path}")"
    PLUGIN_STAGE_ROOT="$(dirname "${plugin_stage_dir}")"
fi

plugin_version="$(printf '%s' "${plugin_metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["plugin_version"])')"
plugin_artifact_sha="$(printf '%s' "${plugin_metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["artifact_sha256"])')"
plugin_archive_name="$(printf '%s' "${plugin_metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["archive_name"])')"
pass "Plugin artifact selected (${plugin_version}, sha256 ${plugin_artifact_sha:0:12})"

info "Uploading and staging OpenClaw plugin artifact..."
openshell sandbox exec -n "${SANDBOX_NAME}" -- \
    mkdir -p /sandbox/.manyforge/plugins 2>/dev/null || true
push_tarball_to_sandbox "${SANDBOX_NAME}" "${PLUGIN_STAGE_ROOT:-${MANYFORGE_PLUGIN_ARTIFACT_DIR}}" \
    'cd /sandbox/.manyforge/plugins && tar xf -' \
    "$(basename "${plugin_stage_dir}")" 2>/dev/null
if [[ "$(realpath "${plugin_archive_path}")" != "$(realpath -m "${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.tar.gz")" ]]; then
    cp "${plugin_archive_path}" "${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.tar.gz"
fi
if [[ "${plugin_metadata_path}" != "${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.json" ]]; then
    printf '%s\n' "${plugin_metadata_json}" > "${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.json"
fi
printf '%s  %s\n' "${plugin_artifact_sha}" "manyforge-mcp-current.tar.gz" > "${MANYFORGE_PLUGIN_STATE_DIR}/manyforge-mcp-current.sha256"
push_tarball_to_sandbox "${SANDBOX_NAME}" "${MANYFORGE_PLUGIN_STATE_DIR}" \
    'cd /sandbox/.manyforge/plugins && tar xf -' \
    manyforge-mcp-current.tar.gz manyforge-mcp-current.json manyforge-mcp-current.sha256 2>/dev/null

plugin_runtime_origin="workspace"
plugin_install_record_json=""

info "Installing pinned plugin artifact into OpenClaw global extensions..."
plugin_install_output="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
    bash -lc "rm -rf /sandbox/.openclaw-data/extensions/manyforge-mcp && \
        HOME=/sandbox \
        OPENCLAW_STATE_DIR=/sandbox/.openclaw-data \
        OPENCLAW_CONFIG_PATH=/sandbox/.openclaw-data/openclaw.json \
        openclaw plugins install /sandbox/.manyforge/plugins/manyforge-mcp-current.tar.gz" \
    2>&1 || true)"
if printf '%s\n' "${plugin_install_output}" | grep -q "Installed plugin: manyforge-mcp"; then
    plugin_runtime_origin="global"
    plugin_install_record_json="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc "python3 -c 'import json,pathlib; cfg=json.loads(pathlib.Path(\"/sandbox/.openclaw-data/openclaw.json\").read_text()); print(json.dumps((cfg.get(\"plugins\") or {}).get(\"installs\", {}).get(\"manyforge-mcp\", {})))'" \
        2>/dev/null || true)"
    pass "Pinned plugin artifact installed into OpenClaw global extensions"
else
    warn "Pinned plugin install did not converge cleanly; falling back to workspace plugin"
    if [[ -n "${plugin_install_output}" ]]; then
        info "${plugin_install_output}"
    fi
fi

cluster_container=$(thor_openshell_cluster_container_name 2>/dev/null || true)
if [[ -z "${cluster_container}" ]]; then
    fail "OpenShell cluster container not found; cannot patch root-owned openclaw.json"
    exit 1
fi

docker exec -i \
    -e THOR_SANDBOX_NAME="${SANDBOX_NAME}" \
    -e MANYFORGE_PLUGIN_ENDPOINT="${PLUGIN_ENDPOINT}" \
    -e MANYFORGE_PLUGIN_BEARER_TOKEN="${PLUGIN_BEARER_TOKEN}" \
    -e MANYFORGE_PLUGIN_CA_CERT_PATH="${PLUGIN_CA_CERT_PATH}" \
    -e MANYFORGE_PLUGIN_RUNTIME_ORIGIN="${plugin_runtime_origin}" \
    -e MANYFORGE_PLUGIN_INSTALL_RECORD_JSON="${plugin_install_record_json}" \
    "${cluster_container}" sh <<'SH' >/dev/null
set -euo pipefail

kubectl -n openshell exec -i "${THOR_SANDBOX_NAME}" -- \
  env \
    MANYFORGE_PLUGIN_ENDPOINT="${MANYFORGE_PLUGIN_ENDPOINT}" \
    MANYFORGE_PLUGIN_BEARER_TOKEN="${MANYFORGE_PLUGIN_BEARER_TOKEN}" \
    MANYFORGE_PLUGIN_CA_CERT_PATH="${MANYFORGE_PLUGIN_CA_CERT_PATH}" \
    MANYFORGE_PLUGIN_RUNTIME_ORIGIN="${MANYFORGE_PLUGIN_RUNTIME_ORIGIN}" \
    MANYFORGE_PLUGIN_INSTALL_RECORD_JSON="${MANYFORGE_PLUGIN_INSTALL_RECORD_JSON}" \
    python3 - <<'PYEOF'
import json
import os
import pathlib
import shutil

config_path = pathlib.Path("/sandbox/.openclaw/openclaw.json")
config = json.loads(config_path.read_text()) if config_path.exists() else {}

workspace_extension_path = pathlib.Path("/sandbox/.openclaw/workspace/.openclaw/extensions/manyforge-mcp")
runtime_origin = os.environ.get("MANYFORGE_PLUGIN_RUNTIME_ORIGIN", "workspace")
if runtime_origin == "global":
    if workspace_extension_path.exists():
        shutil.rmtree(workspace_extension_path)
else:
    if workspace_extension_path.exists():
        shutil.rmtree(workspace_extension_path)
    workspace_extension_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree("/sandbox/.manyforge/plugins/manyforge-mcp", workspace_extension_path)

plugins = config.setdefault("plugins", {}).setdefault("entries", {})
plugin_entry = plugins.setdefault("manyforge-mcp", {})
plugin_entry["enabled"] = True
plugin_entry["config"] = {
    "endpoint": os.environ.get("MANYFORGE_PLUGIN_ENDPOINT", ""),
    "principalId": "llm:openclaw-agent",
    "timeoutMs": 120000,
    "terminalAnswerDir": "/sandbox/.manyforge/terminal-answers",
}
bearer_token = os.environ.get("MANYFORGE_PLUGIN_BEARER_TOKEN", "")
ca_cert_path = os.environ.get("MANYFORGE_PLUGIN_CA_CERT_PATH", "")
if bearer_token:
    plugin_entry["config"]["bearerToken"] = bearer_token
else:
    plugin_entry["config"].pop("bearerToken", None)
if ca_cert_path:
    plugin_entry["config"]["caCertPath"] = ca_cert_path
else:
    plugin_entry["config"].pop("caCertPath", None)

install_record_raw = os.environ.get("MANYFORGE_PLUGIN_INSTALL_RECORD_JSON", "").strip()
if install_record_raw:
    try:
        install_record = json.loads(install_record_raw)
    except json.JSONDecodeError:
        install_record = {}
    if isinstance(install_record, dict) and install_record:
        installs = config.setdefault("plugins", {}).setdefault("installs", {})
        installs["manyforge-mcp"] = install_record
elif "installs" in config.get("plugins", {}):
    installs = config["plugins"].get("installs")
    if isinstance(installs, dict):
        installs.pop("manyforge-mcp", None)
        if not installs:
            config["plugins"].pop("installs", None)

plugins_allow = config.setdefault("plugins", {}).setdefault("allow", [])
if isinstance(plugins_allow, list) and "manyforge-mcp" not in plugins_allow:
    plugins_allow.append("manyforge-mcp")

mcp_cfg = config.get("mcp")
if isinstance(mcp_cfg, dict):
    servers = mcp_cfg.get("servers")
    if isinstance(servers, dict):
        servers.pop("manyforge", None)
        if not servers:
            mcp_cfg.pop("servers", None)
    if not mcp_cfg:
        config.pop("mcp", None)

acpx_cfg = plugins.get("acpx", {}).get("config")
if isinstance(acpx_cfg, dict):
    mcp_servers = acpx_cfg.get("mcpServers")
    if isinstance(mcp_servers, dict):
        mcp_servers.pop("manyforge", None)
        if not mcp_servers:
            acpx_cfg.pop("mcpServers", None)
    if str(acpx_cfg.get("command", "")).startswith("/sandbox/.manyforge/acpx/"):
        plugins.pop("acpx", None)

tools_cfg = config.setdefault("tools", {})
also_allow = tools_cfg.setdefault("alsoAllow", [])
if isinstance(also_allow, list) and "manyforge-mcp" not in also_allow:
    also_allow.append("manyforge-mcp")

config_path.parent.mkdir(parents=True, exist_ok=True)
config_path.write_text(json.dumps(config, indent=2))
print(f"ManyForge plugin configured in openclaw.json (origin={runtime_origin})")
PYEOF
SH
if [[ "${plugin_runtime_origin}" == "global" ]]; then
    pass "Installed plugin selected as runtime origin"
else
    pass "Workspace plugin refreshed from versioned artifact"
fi

# ── Step 6: Install skills ───────────────────────────────────────────────────

info "Installing OpenClaw skills..."
for skill_dir in "${SCRIPT_DIR}/templates/skills/"*/; do
    [[ -d "${skill_dir}" ]] || continue
    skill_name=$(basename "${skill_dir}")
    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        mkdir -p "/sandbox/.agents/skills/${skill_name}" 2>/dev/null || true
    push_tarball_to_sandbox "${SANDBOX_NAME}" "${skill_dir}" \
        "cd /sandbox/.agents/skills/${skill_name} && tar xf -" SKILL.md 2>/dev/null
done
skill_count=$(ls -d "${SCRIPT_DIR}/templates/skills/"*/ 2>/dev/null | wc -l)
pass "${skill_count} skills installed"

# ── Step 7: Upload workspace files ───────────────────────────────────────────

info "Uploading workspace files..."
push_tarball_to_sandbox "${SANDBOX_NAME}" "${SCRIPT_DIR}/templates" \
    'cd /sandbox && tar xf -' AGENTS.md SOUL.md 2>/dev/null
pass "Workspace files uploaded (AGENTS.md, SOUL.md)"

# ── Step 8: Restart OpenClaw gateway ─────────────────────────────────────────

info "Restarting OpenClaw gateway..."
gateway_restarted_via_helper="false"

# Try NemoClaw-Thor's native gateway management first
if type ensure_sandbox_gateway_running &>/dev/null; then
    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -c 'pkill -f "openclaw gateway" 2>/dev/null || true' 2>/dev/null || true
    sleep 2
    ensure_sandbox_gateway_running "${SANDBOX_NAME}" 2>/dev/null && {
        gateway_restarted_via_helper="true"
        pass "Gateway restarted via NemoClaw-Thor"
    } || {
        # Fallback: manual gateway start
        warn "NemoClaw gateway helper failed, starting manually"
        openshell sandbox exec -n "${SANDBOX_NAME}" -- \
            bash -c 'HOME=/sandbox nohup openclaw gateway run --auth none --port 18789 > /tmp/gateway.log 2>&1 &' \
            2>/dev/null || true
        sleep 3
    }
else
    # No NemoClaw helper available — manual start
    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -c 'pkill -f "openclaw gateway" 2>/dev/null || true' 2>/dev/null || true
    sleep 2
    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -c 'HOME=/sandbox nohup openclaw gateway run --auth none --port 18789 > /tmp/gateway.log 2>&1 &' \
        2>/dev/null || true
        sleep 3
fi

if [[ "${gateway_restarted_via_helper}" == "true" ]]; then
    pass "Gateway running (verified by NemoClaw-Thor helper)"
else
    gw_status=$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'HOME=/sandbox openclaw gateway status 2>/dev/null || true' 2>/dev/null || echo "")
    if echo "${gw_status}" | grep -q "RPC probe: ok"; then
        pass "Gateway running"
    else
        warn "Gateway may not have started — check /tmp/gateway.log in sandbox"
    fi
fi

# ── Step 9: Verify plugin discovery ──────────────────────────────────────────

info "Verifying ManyForge plugin discovery..."
plugins_output=$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
    bash -lc 'HOME=/sandbox openclaw plugins list 2>/dev/null || true' 2>/dev/null || true)
if ! echo "${plugins_output}" | grep -q "manyforge-mcp"; then
    fail "OpenClaw plugin list does not show manyforge-mcp"
    fix "Check sandbox plugin archive: /sandbox/.manyforge/plugins/manyforge-mcp-latest.tar.gz"
    fix "Check gateway/plugin logs in the sandbox."
    exit 1
fi
pass "OpenClaw loads the manyforge-mcp plugin"

plugin_info_output=$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
    bash -lc 'HOME=/sandbox openclaw plugins info manyforge-mcp 2>/dev/null || true' 2>/dev/null || true)
plugin_info_origin=$(printf '%s\n' "${plugin_info_output}" | awk -F': ' '/Origin:/ {print $2; exit}')
if [[ "${plugin_runtime_origin}" == "global" && "${plugin_info_origin}" != "global" ]]; then
    warn "Pinned plugin installed, but runtime still reports origin=${plugin_info_origin:-unknown}"
elif [[ "${plugin_runtime_origin}" == "global" ]]; then
    pass "Runtime plugin origin is global"
fi

# ── Step 10: Save mux state to Thor config ────────────────────────────────────

save_thor_runtime_config
pass "Mux config persisted to ${THOR_CONFIG_FILE}"

# ── Summary ───────────────────────────────────────────────────────────────────

header "ManyForge provider configured successfully"
echo ""
echo "  Mode:     ${TRANSPORT_MODE}"
echo "  Sandbox:  ${THOR_OPENCLAW_BASE_URL}"
echo "  Provider: ${THOR_LOCAL_PROVIDER_NAME} → ${THOR_LOCAL_VLLM_BASE_URL}"
echo "  Plugin:   ${PLUGIN_ENDPOINT}"
echo "  Artifact: ${plugin_archive_name} (${plugin_artifact_sha:0:12})"
echo "  Origin:   ${plugin_runtime_origin}"
echo "  Tools:    ${host_tool_count}"
echo "  Skills:   ${skill_count}"
echo ""
echo "  To restore direct vLLM mode only:"
echo "    cd ${NEMOCLAW_THOR} && ./setup/configure-local-provider.sh --without-manyforge-mux"
echo ""
echo "  To redeploy a pinned artifact without rebuilding:"
echo "    ./scripts/configure-manyforge-provider.sh --artifact ${plugin_archive_path}"
echo ""
echo "  Next validation with a live model:"
echo "    openshell sandbox connect ${SANDBOX_NAME}"
echo "    openclaw agent --local --json -m \"Reply with only the names of any tools available to you whose names start with manyforge_. If none are available, reply none.\""
