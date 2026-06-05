#!/usr/bin/env bash
# manyforge-openclaw-smoke.sh — End-to-end validation for the current
# OpenClaw/NemoClaw ↔ ManyForge baseline.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NEMOCLAW_THOR="${NEMOCLAW_THOR:-$HOME/workspaces/dev_ws/src/NemoClaw-Thor}"

source "${NEMOCLAW_THOR}/setup/checks.sh"
source "${NEMOCLAW_THOR}/serving/config.sh"
source "${NEMOCLAW_THOR}/setup/sandbox-runtime.sh"

MODE="${1:-all}"
KEEP_FINAL_MODE="${KEEP_FINAL_MODE:-false}"
THOR_AGENT_TIMEOUT_SECONDS="${THOR_AGENT_TIMEOUT_SECONDS:-120}"
SMOKE_TERMINAL_ANSWER_DIR="${SMOKE_TERMINAL_ANSWER_DIR:-/sandbox/.manyforge/terminal-answers}"
SMOKE_STATE_DIR="${SMOKE_STATE_DIR:-$HOME/.manyforge/smoke}"
SMOKE_PLUGIN_DEPLOY_MODE="${SMOKE_PLUGIN_DEPLOY_MODE:-current}"
CURRENT_PHASE="initializing"

case "${MODE}" in
    direct|mux|all) ;;
    *)
        echo "Usage: $0 [direct|mux|all]" >&2
        exit 1
        ;;
esac

load_thor_runtime_config ""
SANDBOX_NAME="${THOR_MANAGED_SANDBOX_NAME:-thor-v5}"
INITIAL_MUX_ENABLED="${THOR_MANYFORGE_MUX_ENABLED:-false}"
RESTORE_MODE="false"

cleanup() {
    if [[ "${KEEP_FINAL_MODE}" == "true" || "${RESTORE_MODE}" != "true" ]]; then
        return
    fi

    if [[ "${INITIAL_MUX_ENABLED}" == "true" ]]; then
        "${SCRIPT_DIR}/configure-manyforge-provider.sh" >/dev/null
    else
        (cd "${NEMOCLAW_THOR}" && ./setup/configure-local-provider.sh --without-manyforge-mux >/dev/null)
    fi
}
trap cleanup EXIT

record_smoke_result() {
    local status="$1"
    local detail="$2"
    local provider_url sandbox_base_url plugin_origin plugin_version artifact_sha
    mkdir -p "${SMOKE_STATE_DIR}"
    provider_url="$(provider_base_url || true)"
    sandbox_base_url="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'python3 -c "import json,pathlib; config=json.loads(pathlib.Path(\"/sandbox/.openclaw/openclaw.json\").read_text()); print(config.get(\"models\", {}).get(\"providers\", {}).get(\"inference\", {}).get(\"baseUrl\", \"\"))"' \
        2>/dev/null || true)"
    plugin_origin="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'HOME=/sandbox openclaw plugins info manyforge-mcp 2>/dev/null | awk -F\": \" \"/Origin:/ {print \\$2; exit}\"' \
        2>/dev/null || true)"
    plugin_version="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'HOME=/sandbox openclaw plugins info manyforge-mcp 2>/dev/null | awk -F\": \" \"/Version:/ {print \\$2; exit}\"' \
        2>/dev/null || true)"
    artifact_sha="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'python3 -c "import json,pathlib; p=pathlib.Path(\"/sandbox/.manyforge/plugins/manyforge-mcp-current.json\"); print(json.loads(p.read_text()).get(\"artifact_sha256\", \"\") if p.exists() else \"\")"' \
        2>/dev/null || true)"
    python3 - "${SMOKE_STATE_DIR}" "${MODE}" "${status}" "${detail}" "${provider_url}" "${sandbox_base_url}" "${plugin_origin}" "${plugin_version}" "${artifact_sha}" <<'PY'
import json
import pathlib
import sys
from datetime import datetime, timezone

state_dir = pathlib.Path(sys.argv[1])
payload = {
    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
    "mode": sys.argv[2],
    "status": sys.argv[3],
    "detail": sys.argv[4],
    "provider_url": sys.argv[5],
    "sandbox_base_url": sys.argv[6],
    "plugin_origin": sys.argv[7],
    "plugin_version": sys.argv[8],
    "artifact_sha256": sys.argv[9],
}
state_dir.mkdir(parents=True, exist_ok=True)
(state_dir / "latest.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
with (state_dir / "history.jsonl").open("a", encoding="utf-8") as handle:
    handle.write(json.dumps(payload) + "\n")
PY

    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc "mkdir -p /sandbox/.manyforge/smoke && cat > /sandbox/.manyforge/smoke/latest.json" \
        < "${SMOKE_STATE_DIR}/latest.json" >/dev/null 2>&1 || true
}

extract_json() {
    local raw_path="$1"
    python3 - "${raw_path}" <<'PY'
import json
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
decoder = json.JSONDecoder()
best = None
best_start = -1
fallback = None
fallback_start = -1
for idx, ch in enumerate(text):
    if ch != "{":
        continue
    try:
        obj, end = decoder.raw_decode(text[idx:])
    except Exception:
        continue
    if isinstance(obj, dict) and ("payloads" in obj or "meta" in obj):
        if idx > best_start:
            best = obj
            best_start = idx
    elif idx > fallback_start:
        fallback = obj
        fallback_start = idx
if best is None:
    best = fallback
if best is None:
    raise SystemExit("no JSON object found in OpenClaw output")
print(json.dumps(best))
PY
}

provider_base_url() {
    env -u THOR_LOCAL_VLLM_BASE_URL -u THOR_MANYFORGE_MUX_ENABLED bash -lc \
        "source '${NEMOCLAW_THOR}/serving/config.sh' && load_thor_runtime_config '' >/dev/null 2>&1 && printf '%s\n' \"\${THOR_LOCAL_VLLM_BASE_URL:-}\""
}

assert_sandbox_base_url() {
    local expected="$1"
    local actual
    actual="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'python3 -c "import json,pathlib; config=json.loads(pathlib.Path(\"/sandbox/.openclaw/openclaw.json\").read_text()); print(config.get(\"models\", {}).get(\"providers\", {}).get(\"inference\", {}).get(\"baseUrl\", \"\"))"' \
        2>/dev/null || true)"
    if [[ -z "${actual}" ]]; then
        fail "Could not inspect sandbox base URL"
        return 1
    fi
    if [[ "${actual}" != "${expected}" ]]; then
        fail "Sandbox base URL mismatch: ${actual} (expected ${expected})"
        return 1
    fi
    pass "Sandbox base URL: ${actual}"
}

run_agent_json() {
    local session_id="$1"
    local message="$2"
    local output_path="$3"
    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc "HOME=/sandbox openclaw agent --local --json --session-id '${session_id}' --message $(printf '%q' "${message}") --thinking off --timeout ${THOR_AGENT_TIMEOUT_SECONDS}" \
        >"${output_path}" 2>&1
}

direct_smoke() {
    CURRENT_PHASE="direct"
    header "Direct Mode Smoke"
    (cd "${NEMOCLAW_THOR}" && ./setup/configure-local-provider.sh --without-manyforge-mux >/dev/null)
    RESTORE_MODE="true"
    load_thor_runtime_config ""

    local provider_url
    provider_url="$(provider_base_url)"
    if [[ "${provider_url}" != "http://host.openshell.internal:8000/v1" ]]; then
        fail "Direct mode provider target mismatch: ${provider_url}"
        return 1
    fi
    pass "Provider target: ${provider_url}"
    assert_sandbox_base_url "https://inference.local/v1"

    local raw_output json_output
    raw_output="$(mktemp)"
    json_output="$(mktemp)"
    run_agent_json "manyforge-smoke-direct" "Reply with exactly one word: ready" "${raw_output}"
    extract_json "${raw_output}" > "${json_output}"
    python3 - "${json_output}" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
payloads = data.get("payloads") or []
text = (payloads[0].get("text") if payloads else "") or ""
if text.strip() != "ready":
    raise SystemExit(f"unexpected direct smoke reply: {text!r}")
PY
    pass "Direct agent reply: ready"
    rm -f "${raw_output}" "${json_output}"
}

verify_productized_plugin() {
    local plugin_info
    plugin_info="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'HOME=/sandbox openclaw plugins info manyforge-mcp 2>/dev/null || true' 2>/dev/null || true)"
    if [[ -z "${plugin_info}" ]]; then
        fail "Could not inspect manyforge-mcp plugin info"
        return 1
    fi
    local version origin install_source install_path
    version="$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Version:/ {print $2; exit}')"
    origin="$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Origin:/ {print $2; exit}')"
    install_source="$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Install:/ {print $2; exit}')"
    install_path="$(printf '%s\n' "${plugin_info}" | awk -F': ' '/Install path:/ {print $2; exit}')"
    if [[ -z "${version}" ]]; then
        fail "manyforge-mcp plugin version is missing"
        return 1
    fi
    if [[ "${origin}" != "global" ]]; then
        fail "manyforge-mcp runtime origin is ${origin:-unknown}, expected global"
        return 1
    fi
    if [[ "${install_source}" != "archive" ]]; then
        fail "manyforge-mcp install provenance is ${install_source:-unknown}, expected archive"
        return 1
    fi
    if [[ "${install_path}" != "~/.openclaw-data/extensions/manyforge-mcp" ]]; then
        fail "manyforge-mcp install path is ${install_path:-unknown}, expected ~/.openclaw-data/extensions/manyforge-mcp"
        return 1
    fi

    local metadata_json
    metadata_json="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'cat /sandbox/.manyforge/plugins/manyforge-mcp-current.json 2>/dev/null || true' \
        2>/dev/null || true)"
    if [[ -z "${metadata_json}" ]]; then
        fail "Pinned plugin artifact metadata is missing from sandbox"
        return 1
    fi
    local artifact_version artifact_sha
    artifact_version="$(printf '%s' "${metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["plugin_version"])')"
    artifact_sha="$(printf '%s' "${metadata_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["artifact_sha256"])')"
    if [[ "${version}" != "${artifact_version}" ]]; then
        fail "Plugin version ${version} does not match pinned artifact ${artifact_version}"
        return 1
    fi
    pass "Plugin origin=global and pinned to artifact ${artifact_version} (${artifact_sha:0:12})"
}

mux_smoke() {
    CURRENT_PHASE="mux"
    header "ManyForge Mux Smoke"
    if [[ "${SMOKE_PLUGIN_DEPLOY_MODE}" == "latest" ]]; then
        "${SCRIPT_DIR}/configure-manyforge-provider.sh" >/dev/null
    else
        "${SCRIPT_DIR}/configure-manyforge-provider.sh" --use-current-artifact >/dev/null
    fi
    RESTORE_MODE="true"
    load_thor_runtime_config ""

    local provider_url
    provider_url="$(provider_base_url)"
    if [[ "${provider_url}" != "http://host.openshell.internal:8888/v1" ]]; then
        fail "Mux mode provider target mismatch: ${provider_url}"
        return 1
    fi
    pass "Provider target: ${provider_url}"
    assert_sandbox_base_url "https://inference.local/v1"
    verify_productized_plugin

    local mux_health
    mux_health="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc 'curl -sf --max-time 10 https://inference.local/v1/models/manyforge/api/health' 2>/dev/null || true)"
    if [[ -z "${mux_health}" ]]; then
        fail "ManyForge health is not reachable through inference.local"
        return 1
    fi
    pass "ManyForge health reachable through inference.local"

    openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc "rm -rf '${SMOKE_TERMINAL_ANSWER_DIR}' && mkdir -p '${SMOKE_TERMINAL_ANSWER_DIR}'" >/dev/null

    local raw_output json_output
    raw_output="$(mktemp)"
    json_output="$(mktemp)"
    run_agent_json \
        "manyforge-smoke-mux" \
        "Call manyforge_execution_status with include set to digest. Then call report_lifecycle_state with lifecycle_state equal to digest.state, current_node equal to digest.current_node if present, and summary_code equal to smoke_ok. Do not provide any free text after the terminal tool call." \
        "${raw_output}"
    extract_json "${raw_output}" > "${json_output}"

    local answer_json
    answer_json="$(openshell sandbox exec -n "${SANDBOX_NAME}" -- \
        bash -lc "cat '${SMOKE_TERMINAL_ANSWER_DIR}/latest-report_lifecycle_state.json' 2>/dev/null || true" 2>/dev/null || true)"
    if [[ -z "${answer_json}" ]]; then
        fail "Terminal lifecycle answer was not recorded"
        return 1
    fi

    local reported_state expected_state
    reported_state="$(printf '%s' "${answer_json}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["lifecycle_state"])')"
    expected_state="$(curl -sf --max-time 5 http://localhost:9000/api/execution/digest | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')"
    if [[ "${reported_state}" != "${expected_state}" ]]; then
        fail "Reported lifecycle state ${reported_state} does not match ManyForge digest ${expected_state}"
        return 1
    fi
    pass "Terminal lifecycle answer: ${reported_state}"

    python3 - "${json_output}" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
payloads = data.get("payloads") or []
text = "\n".join((item.get("text") or "") for item in payloads if isinstance(item, dict))
if "self-signed certificate in certificate chain" in text:
    raise SystemExit("mux smoke hit CA misconfiguration")
PY
    pass "Mux agent turn completed without CA bridge errors"
    rm -f "${raw_output}" "${json_output}"
}

run_requested_smoke() {
    case "${MODE}" in
        direct)
            direct_smoke
            ;;
        mux)
            mux_smoke
            ;;
        all)
            direct_smoke
            mux_smoke
            ;;
    esac
}

if run_requested_smoke; then
    record_smoke_result "success" "${CURRENT_PHASE}"
else
    record_smoke_result "failure" "${CURRENT_PHASE}"
    exit 1
fi

header "Smoke Complete"
echo "  Mode: ${MODE}"
