#!/usr/bin/env bash
# setup-hermes.sh — Phase 4 BUILD-AROUND bring-up for the Hermes lane.
#
# Provisions a Hermes sandbox that is born MCP-capable WITHOUT editing the
# third-party NemoClaw repo at all. We mirror the OpenClaw lane's philosophy
# (use the agent's native surfaces + a staged overlay + official routes; never
# patch/fork the third-party agent — THREE-LANE-MIGRATION-PLAN.md principle #1).
#
# Everything the gateway needs at its first (clean) MCP discovery is in place
# before it starts — so there is NO post-start config patch, NO policy-add after
# onboard, NO gateway restart (a restart would trip the #2478 relaunch guard):
#
#   * mcp_servers config — injected via NemoClaw's SUPPORTED managed-tool-gateway
#       matrix door (agents/hermes/config/managed-tool-gateway.ts): we stage our
#       scripts/hermes/managed-tool-gateway-matrix.json as the agent's host matrix
#       and set NEMOCLAW_HERMES_TOOL_GATEWAY_BROKER=1 + PRESETS_B64=["manyforge"].
#       config-gen then merges mcp_servers.manyforge into config.yaml at build.
#   * mcp client SDK    — installed into the hermes venv by the derived Dockerfile
#       (the base image's HERMES_UV_EXTRAS omits "mcp").
#   * bridge script     — the stdlib-only stdio bridge, COPYed into the image.
#   * composer + vLLM egress — supplied at create time via `openshell sandbox
#       create --policy <complete-policy>` (base + manyforge_composer + local
#       inference), so egress is ACTIVE at the gateway's first MCP discovery.
#   * gateway reachability — `--forward <port>` publishes the in-sandbox gateway
#       to the host so the :8300 lane driver (HERMES_BASE_URL) can drive /v1/runs.
#
# The third-party tree (~/NemoClaw) is READ ONLY here — we copy from it into a
# temp staging context and derive a Dockerfile; we never modify it.
#
# Proven on the `hermes-ba2` sandbox: first-boot discovery fetched the Composer
# catalog (HTTP 200) and registered 25 mcp_manyforge_* tools with no restart and
# no #2478; a /v1/runs turn then called mcp_manyforge_program_read and Composer
# returned 200. See PHASE-4-HERMES-LONGITUDINAL.md.
#
# Live operations (openshell/docker) — gated behind HERMES_LANE_PHASE4_ENABLED.
set -euo pipefail

NAME="${1:-hermes-assistant}"
NEMOCLAW="${NEMOCLAW_HOME:-${HOME}/NemoClaw}"                       # third-party, READ-ONLY
NEMOCLAW_THOR="${NEMOCLAW_THOR:-${HOME}/workspaces/dev_ws/src/NemoClaw-Thor}"
MF_THOR="${NEMOCLAW_THOR}/manyforge"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"           # manyforge/scripts
MF="$(cd "${SELF_DIR}/.." && pwd)"                                 # standalone manyforge repo

VLLM_ENDPOINT="${NEMOCLAW_VLLM_ENDPOINT:-http://host.openshell.internal:8000/v1}"
VLLM_MODEL="${NEMOCLAW_VLLM_MODEL:-${MODEL_PROFILE:-gemma4-12b-it-gguf}}"
GATEWAY_PORT="${HERMES_GATEWAY_PORT:-8642}"          # host port --forward -> sandbox gateway
DASH_INTERNAL="${HERMES_DASHBOARD_INTERNAL_PORT:-18642}"
ASSISTANT_CASE_TIMEOUT_S="${ASSISTANT_TIMEOUT_S:-300}"
if [[ "${ASSISTANT_CASE_TIMEOUT_S}" =~ ^[0-9]+$ ]]; then
  HERMES_STREAM_TIMEOUT_DEFAULT_S="$((ASSISTANT_CASE_TIMEOUT_S + 60))"
else
  HERMES_STREAM_TIMEOUT_DEFAULT_S="360"
fi

MATRIX="${SELF_DIR}/hermes/managed-tool-gateway-matrix.json"
DERIVE="${SELF_DIR}/hermes/derive-hermes-dockerfile.mjs"
BRIDGE="${MF}/scripts/manyforge-mcp-bridge.py"
EGRESS_PRESET="${MF_THOR}/policies/manyforge-composer-hermes.merged.yaml"
BASE_POLICY="${NEMOCLAW}/nemoclaw-blueprint/policies/openclaw-sandbox.yaml"
LI_PRESET="${NEMOCLAW}/nemoclaw-blueprint/policies/presets/local-inference.yaml"
UPSTREAM_DOCKERFILE="${NEMOCLAW}/agents/hermes/Dockerfile"

green() { echo -e "\033[32m✓\033[0m $1"; }
yellow() { echo -e "\033[33m!\033[0m $1"; }
red() { echo -e "\033[31m✗\033[0m $1" >&2; }
step() { echo -e "\n\033[1m▶ $1\033[0m"; }

if [[ "${HERMES_LANE_PHASE4_ENABLED:-false}" != "true" ]]; then
  yellow "HERMES_LANE_PHASE4_ENABLED is not true — refusing to run live bring-up."
  yellow "To bring up the Hermes lane:  HERMES_LANE_PHASE4_ENABLED=true $0 ${NAME}"
  exit 0
fi

# --- preflight ----------------------------------------------------------------
step "Preflight"
for bin in openshell nemoclaw docker node python3 awk jq; do
  command -v "$bin" >/dev/null || { red "$bin not on PATH"; exit 1; }
done
for f in "$MATRIX" "$DERIVE" "$BRIDGE" "$EGRESS_PRESET" "$BASE_POLICY" "$LI_PRESET" "$UPSTREAM_DOCKERFILE"; do
  [[ -e "$f" ]] || { red "required input missing: $f"; exit 1; }
done
if nemoclaw list 2>/dev/null | grep -q "\b${NAME}\b"; then
  red "Sandbox '${NAME}' already exists. Destroy it first for a clean born-with build:"
  red "  nemoclaw ${NAME} destroy --yes"
  exit 1
fi
green "preflight ok (model=${VLLM_MODEL}, endpoint=${VLLM_ENDPOINT}, gateway port=${GATEWAY_PORT})"
green "third-party ~/NemoClaw is read-only here (copied into a temp staging context)"

# --- 1. stage build context (only what the Dockerfile COPYs + our overlay) ----
step "1/4 Stage build context (upstream COPY sources + our matrix + bridge)"
WORK="$(mktemp -d /tmp/hermes-ba.XXXXXX)"; CTX="${WORK}/ctx"
trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${CTX}/agents/hermes/host" "${CTX}/scripts/lib"
cp -r "${NEMOCLAW}/agents/hermes/plugin"            "${CTX}/agents/hermes/"
cp    "${NEMOCLAW}/agents/hermes/generate-config.ts" "${CTX}/agents/hermes/"
cp -r "${NEMOCLAW}/agents/hermes/config"            "${CTX}/agents/hermes/"
cp    "${NEMOCLAW}/agents/hermes/start.sh"          "${CTX}/agents/hermes/"
cp -r "${NEMOCLAW}/nemoclaw-blueprint"              "${CTX}/"
cp    "${NEMOCLAW}/scripts/lib/sandbox-init.sh"     "${CTX}/scripts/lib/"
cp    "${MATRIX}"  "${CTX}/agents/hermes/host/managed-tool-gateway-matrix.json"   # OUR matrix
cp    "${BRIDGE}"  "${CTX}/manyforge-mcp-bridge.py"
green "staged context at ${CTX}"

# --- 2. derive the Dockerfile from pristine upstream (anchored transforms) -----
step "2/4 Derive Dockerfile from upstream agents/hermes/Dockerfile"
HERMES_UPSTREAM_DOCKERFILE="${UPSTREAM_DOCKERFILE}" \
HERMES_DERIVED_DOCKERFILE="${CTX}/Dockerfile" \
NEMOCLAW_MODEL="${VLLM_MODEL}" \
NEMOCLAW_INFERENCE_BASE_URL="${VLLM_ENDPOINT}" \
  node "${DERIVE}"

# --- 3. build the complete create-time egress policy --------------------------
step "3/4 Build complete egress policy (base + manyforge_composer + local_inference)"
POLICY="${WORK}/egress.yaml"
{
  cat "${BASE_POLICY}"
  echo ""; echo "  # ── [build-around] ManyForge composer egress (Hermes lane) ──"
  awk '/^  manyforge_composer:/{f=1} f' "${EGRESS_PRESET}"
  echo ""; echo "  # ── [build-around] local inference (vLLM) egress ──"
  awk '/^  local_inference:/{f=1} f' "${LI_PRESET}"
} > "${POLICY}"
python3 - "${POLICY}" <<'PY'
import sys, yaml
d = yaml.safe_load(open(sys.argv[1]))
np = d.get("network_policies", {})
assert "manyforge_composer" in np and "local_inference" in np, f"policy merge failed: {sorted(np)}"
assert d.get("filesystem_policy"), "policy missing filesystem_policy"
ports = [e["port"] for e in np["manyforge_composer"]["endpoints"]]
assert 9000 in ports, f"composer :9000 missing ({ports})"
print(f"  egress policy ok: network_policies={sorted(np)} manyforge_composer_ports={ports}")
PY
green "egress policy built (composer :9000 + vLLM egress active at first boot)"

# --- 4. create the sandbox: egress at first boot + port-forward ---------------
step "4/4 openshell sandbox create (born MCP-capable + egress + --forward)"
openshell sandbox create \
  --from "${CTX}/Dockerfile" \
  --name "${NAME}" \
  --policy "${POLICY}" \
  --forward "${GATEWAY_PORT}" \
  -- env \
     "CHAT_UI_URL=http://127.0.0.1:${GATEWAY_PORT}" \
     "NEMOCLAW_DASHBOARD_PORT=${DASH_INTERNAL}" \
     "HERMES_STREAM_STALE_TIMEOUT=${HERMES_STREAM_STALE_TIMEOUT:-${HERMES_STREAM_TIMEOUT_DEFAULT_S}}" \
     "HERMES_STREAM_READ_TIMEOUT=${HERMES_STREAM_READ_TIMEOUT:-${HERMES_STREAM_TIMEOUT_DEFAULT_S}}" \
     "HERMES_API_TIMEOUT=${HERMES_API_TIMEOUT:-${HERMES_STREAM_TIMEOUT_DEFAULT_S}}" \
     nemoclaw-start
# NOTE: Hermes streaming timeouts, pinned above the Composer case budget.
# run_agent.py
# guards the model stream with HERMES_STREAM_STALE_TIMEOUT (inter-chunk "no chunk
# received" detector; default 180s) and HERMES_STREAM_READ_TIMEOUT (httpx read;
# default 120s). It auto-relaxes both (stale->inf, read->HERMES_API_TIMEOUT) ONLY when
# is_local_endpoint(base_url) is true — but that helper (agent/model_metadata.py) does
# NOT recognize the ".openshell.internal" host suffix (only .docker/.containers/
# .lima.internal), so our Thor endpoint is classified as a CLOUD provider and the
# strict 120s/180s defaults apply. Compounding this, vllm-proxy.py BUFFERS the full
# upstream response (no incremental SSE pass-through), so Hermes sees its first chunk
# only at completion — the timeout then guards *total* response latency, not the
# inter-chunk gap. Any case whose buffered response exceeds ~120s is then killed
# ("Streaming failed before delivery: Request timed out") -> 3 retries -> dead runs
# pile onto the single llama.cpp slot (cascade -> chat HTTP -1). Fix: pin all three
# above the case budget (default ASSISTANT_TIMEOUT_S + 60s). The Composer bridge's
# case timeout and cancel/stop path then own the deadline, while Hermes avoids a
# same-deadline stale retry storm. The proxy's per-call slot-free kill still fires
# first at ASSISTANT_TIMEOUT_S-15 (set by lib/assistant.sh). Override the HERMES_*
# envs if the case budget changes.
CONTAINER="$(docker ps --format '{{.Names}}' | grep -E "openshell-${NAME}(-|$)" | head -1 || true)"
[[ -n "${CONTAINER}" ]] || { red "could not resolve sandbox container for '${NAME}'"; exit 1; }
green "sandbox container: ${CONTAINER}"

# --- first-boot verification gate ---------------------------------------------
# Success bar: the gateway came up ONCE already exposing manyforge MCP — no
# restart, no patch, no #2478. Reads the bridge's own trace (a file, so docker
# exec is sufficient — the gateway API itself lives in an isolated netns).
step "First-boot verification gate"
FAIL=0
TRACE="/sandbox/.hermes/logs/manyforge-mcp-trace.jsonl"

docker exec "${CONTAINER}" /opt/hermes/.venv/bin/python -c "import mcp, mcp.client.stdio" 2>/dev/null \
  && green "(a) mcp client SDK importable in hermes venv" \
  || { red "(a) mcp client SDK NOT importable"; FAIL=1; }

docker exec "${CONTAINER}" bash -lc 'grep -q "^mcp_servers:" /sandbox/.hermes/config.yaml' 2>/dev/null \
  && green "(b) config.yaml carries mcp_servers (baked via the matrix)" \
  || { red "(b) config.yaml has no mcp_servers"; FAIL=1; }

docker exec "${CONTAINER}" test -f /sandbox/manyforge/scripts/manyforge-mcp-bridge.py 2>/dev/null \
  && green "(c) bridge script present at the stable path" \
  || { red "(c) bridge script missing"; FAIL=1; }

# (d/e) discovery fetched the catalog (HTTP 200) and delivered tools to Hermes.
# Retry briefly in case the gateway's first discovery is still in flight.
TOOLS=0; EMIT=0
for _ in $(seq 1 20); do
  TOOLS="$(docker exec "${CONTAINER}" bash -lc "grep -oE '\"tools_count\": [0-9]+' '${TRACE}' 2>/dev/null | grep -oE '[0-9]+' | tail -1" 2>/dev/null || echo 0)"
  EMIT="$(docker exec "${CONTAINER}" bash -lc "grep -oE '\"emitted\": [0-9]+' '${TRACE}' 2>/dev/null | grep -oE '[0-9]+' | tail -1" 2>/dev/null || echo 0)"
  [[ "${TOOLS:-0}" -gt 0 && "${EMIT:-0}" -gt 0 ]] && break
  sleep 3
done
[[ "${TOOLS:-0}" -gt 0 ]] && green "(d) bridge fetched the Composer catalog at first boot: ${TOOLS} tools (HTTP 200)" \
  || { red "(d) bridge did not fetch the catalog — inspect ${TRACE} (http_* events / egress 403?)"; FAIL=1; }
[[ "${EMIT:-0}" -gt 0 ]] && green "(e) ${EMIT} mcp_manyforge_* tools delivered to Hermes via tools/list ON FIRST BOOT" \
  || { red "(e) bridge returned 0 tools to Hermes — inspect ${TRACE} (tools_list_* events)"; FAIL=1; }

# (f) gateway API reachable on the forwarded host port.
curl -fsS -m 5 "http://127.0.0.1:${GATEWAY_PORT}/v1/models" >/dev/null 2>&1 \
  && green "(f) gateway /v1 reachable on host :${GATEWAY_PORT} (--forward)" \
  || yellow "(f) gateway not reachable on host :${GATEWAY_PORT} yet (may still be starting)"

echo ""
if [[ "${FAIL}" -eq 0 ]]; then
  green "BORN-WITH-BUILD SUCCESS — Hermes is MCP-capable on first boot (build-around, no ~/NemoClaw edits)."
  cat <<EOF

Next (host side):
  # start the lane driver (drives /v1/runs at the forwarded gateway):
  ASSISTANT_PROVIDER=hermes HERMES_BASE_URL=http://127.0.0.1:${GATEWAY_PORT} \\
    OPENCLAW_ASSISTANT_SANDBOX=${NAME} <start scripts/lib/assistant.sh::start_bridge_hermes>
  # then verify + benchmark:
  python3 ${MF_THOR}/scripts/debug/longitudinal_hermes.py --sandbox ${NAME} --composer http://127.0.0.1:9000
EOF
else
  red "GATE FAILED (${FAIL} check(s)). Do NOT restart the gateway to fix it (that trips #2478);"
  red "diagnose via ${TRACE}, fix the matrix/policy/Dockerfile, then destroy + re-run for a clean born-with build."
  exit 1
fi
