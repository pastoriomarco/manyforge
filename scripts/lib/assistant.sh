# shellcheck shell=bash
# scripts/lib/assistant.sh — AI assistant stack lifecycle.
#
# Functions (extracted from scripts/demo-assistant-known-good.sh):
#
# Lifecycle (called from `start`):
#   start_model              — vLLM container via NemoClaw-Thor/serving/start-model.sh (liveness only)
#   start_vllm_proxy         — iter-32 mutator proxy on :8000 (optional, liveness only)
#   configure_nemoclaw       — NemoClaw-Thor/setup/configure-local-provider.sh
#   provision_openclaw_sandbox — NemoClaw-Thor/manyforge/setup-manyforge-assistant.sh
#   start_bridge             — dispatches by ASSISTANT_PROVIDER (openclaw|nemoclaw) (liveness only)
#   start_assistant_supervisor — runs manyforge_assistant_bridge.supervisor (full grace probe)
#   stop_assistant_stack     — tears everything started here down
#
# Strict readiness (called from `smoke`):
#   wait_for_vllm_model      — /v1/models returns AND served id matches MODEL_PROFILE
#   wait_for_vllm_proxy      — proxy /v1/models proxies through (requires vLLM ready)
#   wait_for_bridge          — bridge /healthz returns 200

if [[ -n "${_MANYFORGE_LIB_ASSISTANT:-}" ]]; then return 0; fi
_MANYFORGE_LIB_ASSISTANT=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"
source "$(dirname "${BASH_SOURCE[0]}")/preflight.sh"
source "$(dirname "${BASH_SOURCE[0]}")/composer.sh"
source "$(dirname "${BASH_SOURCE[0]}")/wait.sh"

# Per-service readiness timeouts (seconds). Overridable from env.
# `start` blocks on these for liveness only (process up, port bound);
# full readiness is reported by the assistant_supervisor into the
# Composer status popup. `smoke` enforces strict readiness using the
# same timeouts.
VLLM_MODEL_READY_TIMEOUT_S="${VLLM_MODEL_READY_TIMEOUT_S:-900}"
VLLM_PROXY_READY_TIMEOUT_S="${VLLM_PROXY_READY_TIMEOUT_S:-30}"
ASSISTANT_BRIDGE_READY_TIMEOUT_S="${ASSISTANT_BRIDGE_READY_TIMEOUT_S:-60}"
ASSISTANT_SUPERVISOR_GRACE_S="${ASSISTANT_SUPERVISOR_GRACE_S:-3}"
VLLM_LIVENESS_GRACE_S="${VLLM_LIVENESS_GRACE_S:-5}"
BRIDGE_LIVENESS_GRACE_S="${BRIDGE_LIVENESS_GRACE_S:-5}"
PROXY_LIVENESS_GRACE_S="${PROXY_LIVENESS_GRACE_S:-2}"

# ── Configuration (env vars carried over from demo-assistant-known-good.sh) ──
NEMOCLAW_THOR="${NEMOCLAW_THOR:-${HOME}/workspaces/dev_ws/src/NemoClaw-Thor}"
# Default Thor model: gemma4-12b-it-gguf (QAT). Top effective rate on the
# 2026-06-07 7-model sweep (78.8%) and the strongest model on both Thor and
# Orin; lightweight GGUF, no vLLM, ~2x faster than plain gemma. Orin's
# equivalent default is gemma4-12b-it-gguf-orin (scripts/launch-orin-gemma.sh).
MODEL_PROFILE="${MODEL_PROFILE:-gemma4-12b-it-gguf}"
ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER:-openclaw}"
START_VLLM_PROXY="${START_VLLM_PROXY:-true}"
OPENCLAW_PROXY_OVERRIDE_MAX_TOKENS="${OPENCLAW_PROXY_OVERRIDE_MAX_TOKENS:-2048}"
OPENCLAW_PROXY_THINKING_TOKEN_BUDGET="${OPENCLAW_PROXY_THINKING_TOKEN_BUDGET:-512}"
OPENCLAW_ASSISTANT_COMPACT_EVERY_N="${OPENCLAW_ASSISTANT_COMPACT_EVERY_N:-2}"
OPENCLAW_ASSISTANT_COMPACT_TIMEOUT_S="${OPENCLAW_ASSISTANT_COMPACT_TIMEOUT_S:-120}"
OPENCLAW_ASSISTANT_METRICS_ENABLED="${OPENCLAW_ASSISTANT_METRICS_ENABLED:-false}"
# FIX 5 — bridge-side cross-turn loop break thresholds.
# Same-tool-name detector (existing): fires when N consecutive turns call
# the same tool name even if args differ. Default 5.
# Same-tool-same-args detector (new in 2026-06-03 lane parity work):
# fires when the EXACT same tool+args fingerprint appears N+1 times,
# meaning the model isn't reading the error envelope's diff/hint. Default
# 2 (3 identical calls trigger the break). Setting either to 0 disables
# that detector.
OPENCLAW_ASSISTANT_LOOP_TOOL_THRESHOLD="${OPENCLAW_ASSISTANT_LOOP_TOOL_THRESHOLD:-5}"
OPENCLAW_ASSISTANT_LOOP_ARGS_THRESHOLD="${OPENCLAW_ASSISTANT_LOOP_ARGS_THRESHOLD:-2}"
# OPENCLAW_ASSISTANT_TOOL_SURFACE — selects which dispatch-surface
# primer the bridge prompt teaches the model. Must match what the
# sandbox-side OpenClaw config actually exposes in tools[]:
#   "tools" → tools[] is {tool_search, tool_describe, tool_call}.
#             PRODUCTION DEFAULT since Phase 3 (2026-06-03). Measured
#             58.3% effective rate on cosmos-reason2-8b vs 29.0% for
#             code mode (see docs/LANE-COMPARISON.md).
#             Requires sandbox openclaw.json to set
#             tools.toolSearch = {enabled: true, mode: "tools"}.
#   "code"  → tools[] is {tool_search_code}. OpenClaw 2026.5.6+
#             baseline. Use only for the multi-model bake-off or
#             to test the alternative surface; not the production
#             default any more.
# Forwarded to BOTH the bridge launcher AND the vllm-proxy launcher
# (the proxy uses it to emit ``tool_surface_mismatch`` warnings if
# the observed tools[] does not match the operator's declaration —
# the load-bearing operator-error detector for this knob). Setting
# the same value at both layers gives the drift check something to
# compare against.
OPENCLAW_ASSISTANT_TOOL_SURFACE="${OPENCLAW_ASSISTANT_TOOL_SURFACE:-tools}"
ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S:-300}"
ASSISTANT_MAX_TURNS="${ASSISTANT_MAX_TURNS:-16}"
# vllm-proxy upstream (per model-call) socket timeout. The proxy guards the
# single llama.cpp slot, so by default we set it ~15s BELOW the per-case
# assistant budget (ASSISTANT_TIMEOUT_S): the proxy then fails FIRST and closes
# the upstream socket to release the slot before the case-level timeout fires,
# instead of racing it. (The old hardcoded 200s sat far below the budget and
# cut off legitimate prefill-heavy turns — see vllm-proxy.py _UPSTREAM_TIMEOUT_S.)
# Canonical MANYFORGE_PROXY_ name; legacy OPENCLAW_PROXY_ accepted. Falls back to
# the budget value when it isn't a plain integer.
if [[ "${ASSISTANT_TIMEOUT_S}" =~ ^[0-9]+$ && "${ASSISTANT_TIMEOUT_S}" -gt 30 ]]; then
    _proxy_upstream_default=$(( ASSISTANT_TIMEOUT_S - 15 ))
else
    _proxy_upstream_default="${ASSISTANT_TIMEOUT_S}"
fi
MANYFORGE_PROXY_UPSTREAM_TIMEOUT_S="${MANYFORGE_PROXY_UPSTREAM_TIMEOUT_S:-${OPENCLAW_PROXY_UPSTREAM_TIMEOUT_S:-${_proxy_upstream_default}}}"
DROP_CACHES="${DROP_CACHES:-true}"
PROVISION_OPENCLAW_SANDBOX="${PROVISION_OPENCLAW_SANDBOX:-true}"

VLLM_CONTAINER="${VLLM_CONTAINER:-manyforge-e2e-vllm}"
LOG_DIR="${LOG_DIR:-/tmp/manyforge-assistant-e2e}"
BRIDGE_PID_FILE="${BRIDGE_PID_FILE:-${LOG_DIR}/known-good-bridge.pid}"
BRIDGE_LOG="${BRIDGE_LOG:-${LOG_DIR}/known-good-bridge.log}"
BRIDGE_AUDIT_LOG="${BRIDGE_AUDIT_LOG:-${LOG_DIR}/known-good-bridge-audit.jsonl}"
SUPERVISOR_PID_FILE="${SUPERVISOR_PID_FILE:-${LOG_DIR}/assistant-supervisor.pid}"
SUPERVISOR_LOG="${SUPERVISOR_LOG:-${LOG_DIR}/assistant-supervisor.log}"

# Port layout: vLLM moves off :8000 when the proxy is active.
if [[ -z "${VLLM_PORT:-}" ]]; then
    if [[ "${START_VLLM_PROXY}" == "true" ]]; then
        VLLM_PORT="8050"
    else
        VLLM_PORT="8000"
    fi
fi
UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL:-http://127.0.0.1:8000/v1}"
VLLM_DIRECT_URL="http://127.0.0.1:${VLLM_PORT}/v1"

# LANE_REGISTRY — per THREE-LANE-MIGRATION-PLAN.md §8 Phase 1.
# Mirrors the Composer-side registry in
# manyforge_composer/backend/assistant_provider.py (LANE_REGISTRY).
# Eliminates the pre-Phase-1 "unknown provider silently falls back to
# Direct" foot-gun.
#
# Format (case statement below):
#   <provider id>) BRIDGE_URL=<default-port>;;
#   The * branch errors out with a clear message including the accepted ids.
# Capture an operator-pinned BRIDGE_URL (rare) so re-resolution still honors it.
_ASSISTANT_BRIDGE_URL_REQUESTED="${BRIDGE_URL:-}"

# resolve_assistant_bridge_url — set BRIDGE_URL from the CURRENT
# ASSISTANT_PROVIDER (honoring an operator-pinned BRIDGE_URL). Safe to call
# repeatedly. The launcher re-calls this after its lane wizard changes
# ASSISTANT_PROVIDER, so BRIDGE_URL — and everything derived from it (the
# Composer assistant endpoint, the supervisor /healthz probe, the plan/status
# display) — tracks the chosen lane instead of the source-time default.
resolve_assistant_bridge_url() {
    case "${ASSISTANT_PROVIDER}" in
        direct|nemoclaw)
            # "direct" is the canonical id; "nemoclaw" is a legacy alias
            # (see LANE_ID_ALIASES in manyforge_composer/backend/assistant_provider.py).
            BRIDGE_URL="${_ASSISTANT_BRIDGE_URL_REQUESTED:-http://127.0.0.1:8100}"
            ;;
        openclaw)
            BRIDGE_URL="${_ASSISTANT_BRIDGE_URL_REQUESTED:-http://127.0.0.1:8200}"
            ;;
        hermes)
            # Phase 4 landed: the bridge (start_bridge_hermes) is implemented.
            # Still gated behind HERMES_LANE_PHASE4_ENABLED so the lane is opt-in
            # until the live bake-off has validated it as a production default.
            if [[ "${HERMES_LANE_PHASE4_ENABLED:-false}" != "true" ]]; then
                echo "ERROR: ASSISTANT_PROVIDER=hermes is opt-in until its bake-off lands." >&2
                echo "       Set HERMES_LANE_PHASE4_ENABLED=true to enable the Hermes lane." >&2
                return 1 2>/dev/null || exit 1
            fi
            BRIDGE_URL="${_ASSISTANT_BRIDGE_URL_REQUESTED:-http://127.0.0.1:8300}"
            ;;
        "")
            # Empty provider → mock mode; bridge URL doesn't matter, but
            # don't fall through into the error branch.
            BRIDGE_URL="${_ASSISTANT_BRIDGE_URL_REQUESTED:-http://127.0.0.1:8100}"
            ;;
        *)
            echo "ERROR: Unknown ASSISTANT_PROVIDER='${ASSISTANT_PROVIDER}'." >&2
            echo "       Accepted values: direct | nemoclaw (alias of direct) | openclaw | hermes (Phase 4) | '' (mock)." >&2
            echo "       See LANE_REGISTRY in manyforge_composer/backend/assistant_provider.py." >&2
            return 1 2>/dev/null || exit 1
            ;;
    esac
}
resolve_assistant_bridge_url

# ── Internal helpers ──────────────────────────────────────────────────────────

bridge_pid_alive() {
    [[ -f "${BRIDGE_PID_FILE}" ]] || return 1
    local pid
    pid="$(cat "${BRIDGE_PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
        return 0
    fi
    rm -f "${BRIDGE_PID_FILE}"
    return 1
}

supervisor_pid_alive() {
    [[ -f "${SUPERVISOR_PID_FILE}" ]] || return 1
    local pid
    pid="$(cat "${SUPERVISOR_PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1; then
        return 0
    fi
    rm -f "${SUPERVISOR_PID_FILE}"
    return 1
}

ensure_log_dir() {
    mkdir -p "${LOG_DIR}"
}

drop_caches() {
    # Free Linux page caches / dentries / inodes. On Thor (and other
    # unified-memory hosts) this is load-bearing after a vLLM stop or
    # crash because vLLM's GPU allocations are backed by the same
    # physical memory as host page cache; without this, RSS reported
    # as "buff/cache" stays high until the next allocation pressure
    # event evicts it. The launcher calls drop_caches at the end of
    # both start_model (before launching a new model) AND
    # stop_assistant_stack (after the vLLM container is gone).
    # Default DROP_CACHES=true; set DROP_CACHES=false to skip.
    if [[ "${DROP_CACHES}" != "true" ]]; then
        return
    fi
    sync
    # Try passwordless sudo first; if that needs a password, fall back
    # to a non-fatal warn so the launcher doesn't block waiting for
    # input. Operators who want this guaranteed should add a sudoers
    # rule for `sh -c 'echo 3 > /proc/sys/vm/drop_caches'` (NOPASSWD).
    if sudo -n true 2>/dev/null; then
        info "Dropping page caches (sync + drop_caches=3)"
        sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' >/dev/null 2>&1 || true
    else
        warn "drop_caches needs sudo but sudo would prompt for a password. Run \`sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'\` manually to free unified memory, or add a NOPASSWD sudoers rule."
    fi
}

served_model_id() {
    curl -fsS "${VLLM_DIRECT_URL}/models" 2>/dev/null | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
    models = data.get("data") or []
    if models:
        print(models[0].get("id", ""))
except Exception:
    pass
' || true
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
    info "Provisioning manyforge_assistant_bridge venv"
    (cd "${MANYFORGE_ROOT}" && python3 -m venv manyforge_assistant_bridge/.venv)
    "${venv_python}" -m pip install --quiet -r "${MANYFORGE_ROOT}/manyforge_assistant_bridge/requirements.txt"
}

# ── Public lifecycle functions ────────────────────────────────────────────────

vllm_container_alive() {
    [[ -n "$(docker ps -q --filter "name=^/${VLLM_CONTAINER}$" 2>/dev/null)" ]]
}

vllm_container_logs() {
    docker logs --tail 20 "${VLLM_CONTAINER}" 2>&1 || true
}

vllm_models_ready() {
    curl -fsS "${VLLM_DIRECT_URL}/models" >/dev/null 2>&1
}

start_model() {
    step "Preparing vLLM model: ${MODEL_PROFILE}"
    if container_running "${VLLM_CONTAINER}"; then
        local current_model
        current_model="$(served_model_id || true)"
        if [[ -n "${current_model}" && "${current_model}" != "${MODEL_PROFILE}" ]]; then
            info "Existing ${VLLM_CONTAINER} serves ${current_model}; restarting for ${MODEL_PROFILE}."
            docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true
        elif [[ "${current_model}" == "${MODEL_PROFILE}" ]]; then
            pass "Reusing ${VLLM_CONTAINER}; already serves ${MODEL_PROFILE}"
            return 0
        else
            # Container is up but not yet serving. Liveness-only at start;
            # supervisor will report readiness to the popup.
            pass "Reusing ${VLLM_CONTAINER} (model still loading; supervisor reports readiness)"
            return 0
        fi
    fi
    info "Starting ${VLLM_CONTAINER}; first launch can take 60+ min while vLLM loads/compiles. Supervisor will report readiness to the Composer status popup."
    docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true
    drop_caches
    # THOR_RESTART_PROXY=0 prevents start-model.sh from spawning its own
    # vllm-proxy. The launcher's start_vllm_proxy step (called next) is
    # the sole owner of the proxy lifecycle — having two competing
    # owners produced the "Found a vllm-proxy.py process that is NOT
    # launcher-managed" error documented in
    # docs/PHASE-3-OPENCLAW-NATIVE-RESULT.md. By forcing 0 here we
    # eliminate the race at the source rather than relying on operators
    # to remember the env var. 2026-06-03.
    (
        cd "${NEMOCLAW_THOR}"
        THOR_DETACH=1 \
        THOR_RESTART_PROXY=0 \
        THOR_CONTAINER_NAME="${VLLM_CONTAINER}" \
        THOR_VLLM_PORT="${VLLM_PORT}" \
        ./serving/start-model.sh "${MODEL_PROFILE}"
    )
    # Liveness check: did the container actually launch and stay up past
    # the grace period? Anything beyond that is the supervisor's job.
    sleep "${VLLM_LIVENESS_GRACE_S}"
    if ! vllm_container_alive; then
        fail "vLLM container ${VLLM_CONTAINER} exited within ${VLLM_LIVENESS_GRACE_S}s"
        vllm_container_logs >&2
        return 1
    fi
    pass "vLLM container ${VLLM_CONTAINER} launched (model load in progress; supervisor reports row)"
}

# wait_for_vllm_model — strict readiness probe for `smoke`. Blocks until
# /v1/models returns AND the served id matches MODEL_PROFILE.
wait_for_vllm_model() {
    local hint="vLLM cold launch can take 60+ min (FlashInfer JIT). Override with VLLM_MODEL_READY_TIMEOUT_S=NN."
    wait_with_progress \
        "vLLM model ${MODEL_PROFILE} at ${VLLM_DIRECT_URL}" \
        "${VLLM_MODEL_READY_TIMEOUT_S}" \
        "${hint}" \
        vllm_container_logs \
        vllm_models_ready \
        || return 1
    local served
    served="$(served_model_id || true)"
    if [[ "${served}" != "${MODEL_PROFILE}" ]]; then
        fail "vLLM served model mismatch: ${served:-unknown} (expected ${MODEL_PROFILE})"
        return 1
    fi
    pass "vLLM served model: ${served}"
}

vllm_proxy_pid_alive() {
    if [[ ! -f "${LOG_DIR}/vllm-proxy.pid" ]]; then
        return 1
    fi
    local pid
    pid="$(cat "${LOG_DIR}/vllm-proxy.pid" 2>/dev/null || true)"
    [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1
}

vllm_proxy_port_bound() {
    port_is_listening 8000
}

vllm_proxy_logs() {
    tail -n 20 "${LOG_DIR}/vllm-proxy.log" 2>/dev/null || true
}

vllm_proxy_models_ready() {
    curl -fsS http://127.0.0.1:8000/v1/models >/dev/null 2>&1
}

start_vllm_proxy() {
    if [[ "${START_VLLM_PROXY}" != "true" ]]; then
        info "START_VLLM_PROXY=${START_VLLM_PROXY}; assuming vllm-proxy is managed externally."
        return 0
    fi
    step "Starting vllm-proxy mutator (host:8000 -> vLLM:${VLLM_PORT})"
    local proxy_py="${NEMOCLAW_THOR}/manyforge/scripts/proxy/vllm-proxy.py"
    if [[ ! -f "${proxy_py}" ]]; then
        fail "vllm-proxy.py not found at ${proxy_py}"
        info "Set START_VLLM_PROXY=false to skip, or point NEMOCLAW_THOR at the right repo."
        return 1
    fi
    # Reuse only when both signals agree this is a launcher-managed proxy:
    # PID file points at a live process AND port 8000 is bound. Plain
    # `pgrep -f vllm-proxy.py` is too loose — it can match a stale
    # unmanaged process from a prior shell or a different user, leading
    # to silent failures where the launcher believes it has a proxy but
    # the upstream URL is wrong.
    if vllm_proxy_pid_alive && vllm_proxy_port_bound; then
        pass "Reusing launcher-managed vllm-proxy (PID $(cat "${LOG_DIR}/vllm-proxy.pid" 2>/dev/null))"
        return 0
    fi
    if pgrep -f 'vllm-proxy.py' >/dev/null 2>&1; then
        fail "Found a vllm-proxy.py process that is NOT launcher-managed"
        info "  Stale PID file or another shell's process. Inspect with:"
        info "    pgrep -af 'vllm-proxy.py'"
        info "  Either kill the stray process or remove ${LOG_DIR}/vllm-proxy.pid and rerun."
        return 1
    fi
    if port_is_listening 8000; then
        fail "Port 8000 is in use by an unmanaged process; refusing to start proxy."
        port_listeners 8000 >&2
        return 1
    fi
    ensure_log_dir
    # Proxy history budget = the fail-open trim ceiling. The proxy's rung 1-4
    # trim ladder keeps requests under it and NEVER hard-fails (rung 4
    # hard-trims oldest content + warns). Selected via
    # MANYFORGE_PROXY_HISTORY_BUDGET:
    #   auto (default) -> proportional to the model context window, so it scales
    #                     for EVERY profile: THOR_TARGET_MAX_MODEL_LEN tokens *
    #                     ~4 chars/token * PCT% (MANYFORGE_PROXY_HISTORY_BUDGET_PCT,
    #                     default 90), leaving headroom for the generation.
    #   off            -> disabled (0): forward everything untrimmed.
    #   <integer>      -> exact char budget.
    # An explicit MANYFORGE_PROXY_MAX_REQUEST_CHARS (or legacy
    # OPENCLAW_PROXY_MAX_REQUEST_CHARS) always wins over the mode.
    if [[ -z "${MANYFORGE_PROXY_MAX_REQUEST_CHARS:-}" && -z "${OPENCLAW_PROXY_MAX_REQUEST_CHARS:-}" ]]; then
        local _hb="${MANYFORGE_PROXY_HISTORY_BUDGET:-auto}"
        case "${_hb}" in
            off|OFF|none|0)
                export MANYFORGE_PROXY_MAX_REQUEST_CHARS=0
                info "Proxy history budget: off (trimming disabled)"
                ;;
            auto|AUTO|"")
                local _pbpct="${MANYFORGE_PROXY_HISTORY_BUDGET_PCT:-90}"
                local _pcfg="${THOR_RUNTIME_CONFIG:-${HOME}/.config/nemoclaw-thor/config.env}"
                local _pctx=""
                [[ -r "${_pcfg}" ]] && _pctx="$(grep -E '^THOR_TARGET_MAX_MODEL_LEN=' "${_pcfg}" | tail -1 | cut -d= -f2 | tr -d '"')"
                if [[ "${_pctx}" =~ ^[0-9]+$ && "${_pbpct}" =~ ^[0-9]+$ ]]; then
                    export MANYFORGE_PROXY_MAX_REQUEST_CHARS=$(( _pctx * 4 * _pbpct / 100 ))
                    info "Proxy history budget: auto -> ${MANYFORGE_PROXY_MAX_REQUEST_CHARS} chars (${_pbpct}% of ${_pctx}-token context)"
                else
                    export MANYFORGE_PROXY_MAX_REQUEST_CHARS=0
                    warn "Proxy history budget: auto requested but THOR_TARGET_MAX_MODEL_LEN unavailable in ${_pcfg}; disabling. Set MANYFORGE_PROXY_HISTORY_BUDGET=<chars> for a fixed size."
                fi
                ;;
            *[!0-9]*)
                export MANYFORGE_PROXY_MAX_REQUEST_CHARS=0
                warn "Proxy history budget: invalid MANYFORGE_PROXY_HISTORY_BUDGET='${_hb}' (want off|auto|<integer-chars>); disabling."
                ;;
            *)
                export MANYFORGE_PROXY_MAX_REQUEST_CHARS="${_hb}"
                info "Proxy history budget: fixed ${MANYFORGE_PROXY_MAX_REQUEST_CHARS} chars"
                ;;
        esac
    fi
    (
        cd "${NEMOCLAW_THOR}"
        env \
            OPENCLAW_PROXY_BIND=0.0.0.0 \
            OPENCLAW_PROXY_LISTEN_PORT=8000 \
            OPENCLAW_PROXY_UPSTREAM="http://127.0.0.1:${VLLM_PORT}" \
            OPENCLAW_PROXY_LOG_PATH="${LOG_DIR}/vllm-proxy.jsonl" \
            OPENCLAW_PROXY_OVERRIDE_MAX_TOKENS="${OPENCLAW_PROXY_OVERRIDE_MAX_TOKENS}" \
            OPENCLAW_PROXY_THINKING_TOKEN_BUDGET="${OPENCLAW_PROXY_THINKING_TOKEN_BUDGET}" \
            MANYFORGE_PROXY_UPSTREAM_TIMEOUT_S="${MANYFORGE_PROXY_UPSTREAM_TIMEOUT_S}" \
            MANYFORGE_PROXY_MAX_REQUEST_CHARS="${MANYFORGE_PROXY_MAX_REQUEST_CHARS:-}" \
            OPENCLAW_PROXY_TOOL_SURFACE="${OPENCLAW_ASSISTANT_TOOL_SURFACE}" \
            setsid python3 "${proxy_py}" </dev/null >"${LOG_DIR}/vllm-proxy.log" 2>&1 &
        echo $! >"${LOG_DIR}/vllm-proxy.pid"
    )
    # Liveness check: pid alive + port bound after a brief grace.
    # Full /v1/models readiness is the assistant_supervisor's job and
    # appears in the Composer status popup.
    sleep "${PROXY_LIVENESS_GRACE_S}"
    if ! vllm_proxy_pid_alive; then
        fail "vllm-proxy exited within ${PROXY_LIVENESS_GRACE_S}s; see ${LOG_DIR}/vllm-proxy.log"
        vllm_proxy_logs >&2
        return 1
    fi
    if ! vllm_proxy_port_bound; then
        # Port bind can lag the PID by a fraction of a second on slow hosts;
        # extend the grace once before failing.
        sleep 2
        if ! vllm_proxy_port_bound; then
            fail "vllm-proxy did not bind :8000 within $((PROXY_LIVENESS_GRACE_S + 2))s"
            vllm_proxy_logs >&2
            return 1
        fi
    fi
    pass "vllm-proxy launched on :8000 (PID $(cat "${LOG_DIR}/vllm-proxy.pid" 2>/dev/null || echo '?'); supervisor reports row)"
}

# wait_for_vllm_proxy — strict readiness probe for `smoke`. Blocks until
# proxy /v1/models proxies through (i.e. vLLM upstream is also ready).
wait_for_vllm_proxy() {
    if [[ "${START_VLLM_PROXY}" != "true" ]]; then
        info "START_VLLM_PROXY=${START_VLLM_PROXY}; skipping proxy readiness check"
        return 0
    fi
    local hint="vllm-proxy proxies through to vLLM; it can't return /v1/models until vLLM upstream is ready. Override with VLLM_PROXY_READY_TIMEOUT_S=NN."
    wait_with_progress \
        "vllm-proxy /v1/models on :8000" \
        "${VLLM_PROXY_READY_TIMEOUT_S}" \
        "${hint}" \
        vllm_proxy_logs \
        vllm_proxy_models_ready
}

configure_nemoclaw() {
    # configure-local-provider.sh wires the OpenShell `vllm-local` provider +
    # inference route + in-sandbox OpenClaw gateway: that is the direct/openclaw
    # lane path. The Hermes lane seeds its own model routing into the sandbox at
    # setup-hermes.sh create time and runs its own gateway, so this step is not
    # only unnecessary for Hermes but wrong — it resolves the OpenClaw sandbox
    # name (my-assistant) and tries to start an OpenClaw gateway inside the
    # Hermes sandbox.
    if [[ "${ASSISTANT_PROVIDER}" == "hermes" ]]; then
        # The Hermes lane self-seeds its model routing, so skip the provider /
        # inference-route config. But the host OpenShell gateway is lane-
        # independent (setup-hermes's `openshell sandbox create` needs it), so
        # still ensure it is up — and not stale after an OpenShell upgrade — via
        # the NemoClaw-Thor gateway-ensure (which restarts a version-drifted
        # gateway). checks.sh is self-contained (defines its own log helpers).
        info "Hermes lane self-seeds model routing; skipping provider config, ensuring the host gateway only."
        if ! (cd "${NEMOCLAW_THOR}" && source setup/checks.sh 2>/dev/null && ensure_openshell_gateway_running); then
            warn "OpenShell gateway ensure failed; setup-hermes may be unable to reach the gateway."
        fi
        return 0
    fi
    step "Configuring NemoClaw/OpenShell provider"
    ensure_log_dir
    local configure_log="${LOG_DIR}/configure-local-provider.log"
    if (cd "${NEMOCLAW_THOR}" && ./setup/configure-local-provider.sh "${MODEL_PROFILE}") 2>&1 | tee "${configure_log}"; then
        return 0
    fi
    if openshell sandbox list 2>/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -Eq '^No sandboxes found\.?$'; then
        warn "No live sandbox exists; continuing because Composer can reach vLLM directly via the bridge."
        return 0
    fi
    fail "NemoClaw/OpenShell provider configuration failed; see ${configure_log}"
    return 1
}

provision_openclaw_sandbox() {
    if [[ "${ASSISTANT_PROVIDER}" != "openclaw" ]]; then
        return 0
    fi
    if [[ "${PROVISION_OPENCLAW_SANDBOX}" != "true" ]]; then
        info "PROVISION_OPENCLAW_SANDBOX=${PROVISION_OPENCLAW_SANDBOX}; skipping sandbox provisioning"
        return 0
    fi
    step "Provisioning OpenClaw sandbox (manyforge-composer skill + egress preset + MCP server)"
    local provisioner="${NEMOCLAW_THOR}/manyforge/setup-manyforge-assistant.sh"
    if [[ ! -x "${provisioner}" ]]; then
        fail "openclaw provisioner not found at: ${provisioner}"
        info "Set NEMOCLAW_THOR, or set PROVISION_OPENCLAW_SANDBOX=false to skip."
        return 1
    fi
    "${provisioner}" "${OPENCLAW_ASSISTANT_SANDBOX:-my-assistant}"
}

start_bridge() {
    case "${ASSISTANT_PROVIDER}" in
        openclaw) start_bridge_openclaw ;;
        hermes)
            # Phase 4 of THREE-LANE-MIGRATION-PLAN.md.
            # The case-match above (around line 74-115) gates the
            # provider on HERMES_LANE_PHASE4_ENABLED, so we should
            # never reach this branch unless it is set. Fail loudly
            # rather than silently falling through to the Direct
            # bridge.
            if [[ "${HERMES_LANE_PHASE4_ENABLED:-false}" != "true" ]]; then
                echo "ERROR: start_bridge() reached the hermes branch with HERMES_LANE_PHASE4_ENABLED=false." >&2
                echo "       The earlier provider-id gate should have rejected this." >&2
                return 1
            fi
            start_bridge_hermes
            ;;
        direct|nemoclaw|"") start_bridge_nemoclaw ;;
        *)
            # Defensive — the provider-id case at line 74 already
            # rejects unknown values, so we should not reach here.
            echo "ERROR: start_bridge() reached an unknown ASSISTANT_PROVIDER='${ASSISTANT_PROVIDER}'." >&2
            return 1
            ;;
    esac
}

bridge_logs() {
    tail -n 20 "${BRIDGE_LOG}" 2>/dev/null || true
}

bridge_healthz_ready() {
    curl -fsS "${BRIDGE_URL}/healthz" >/dev/null 2>&1
}

start_bridge_nemoclaw() {
    step "Starting ManyForge assistant bridge (nemoclaw / direct model lane)"
    if bridge_pid_alive; then
        pass "Reusing bridge from ${BRIDGE_PID_FILE}"
        return 0
    fi
    if port_is_listening 8100; then
        fail "Port 8100 is already in use by an unmanaged process; refusing to start another bridge."
        port_listeners 8100 >&2
        return 1
    fi
    install_bridge_deps
    local python
    python="$(bridge_python)"
    ensure_log_dir
    # 2026-06-05: auto-size the direct-lane cross-request history budget to the
    # served model's context window instead of the static 64K-era default
    # (196608 chars). 75% of ctx at ~4 chars/token == ctx * 3. This scales
    # automatically for every profile (each sets THOR_TARGET_MAX_MODEL_LEN,
    # published to config.env by NemoClaw-Thor start-model.sh). An explicit
    # operator-set DIRECT_LANE_HISTORY_CHAR_BUDGET always wins.
    if [[ -z "${DIRECT_LANE_HISTORY_CHAR_BUDGET:-}" ]]; then
        local _thor_cfg="${THOR_RUNTIME_CONFIG:-${HOME}/.config/nemoclaw-thor/config.env}"
        local _ctx=""
        [[ -r "${_thor_cfg}" ]] && _ctx="$(grep -E '^THOR_TARGET_MAX_MODEL_LEN=' "${_thor_cfg}" | tail -1 | cut -d= -f2 | tr -d '"' )"
        if [[ "${_ctx}" =~ ^[0-9]+$ ]]; then
            export DIRECT_LANE_HISTORY_CHAR_BUDGET=$(( _ctx * 3 ))
            info "Direct-lane history budget auto-sized to ${DIRECT_LANE_HISTORY_CHAR_BUDGET} chars (75% of ${_ctx}-token context window)"
        fi
    fi
    (
        cd "${MANYFORGE_ROOT}"
        env \
            BRIDGE_HOST=127.0.0.1 \
            DIRECT_LANE_HISTORY_CHAR_BUDGET="${DIRECT_LANE_HISTORY_CHAR_BUDGET:-196608}" \
            BRIDGE_PORT=8100 \
            BRIDGE_UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL}" \
            BRIDGE_MANYFORGE_BASE_URL="${COMPOSER_URL}" \
            BRIDGE_MODEL="${MODEL_PROFILE}" \
            BRIDGE_REQUEST_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
            BRIDGE_MAX_WALL_TIME_S="${ASSISTANT_TIMEOUT_S}" \
            BRIDGE_MAX_TURNS="${ASSISTANT_MAX_TURNS}" \
            BRIDGE_AUDIT_LOG="${BRIDGE_AUDIT_LOG}" \
            MANYFORGE_STATE_CONTEXT="${MANYFORGE_STATE_CONTEXT:-first_then_every_n}" \
            MANYFORGE_STATE_CONTEXT_EVERY_N="${MANYFORGE_STATE_CONTEXT_EVERY_N:-3}" \
            BRIDGE_STATE_CONTEXT="${BRIDGE_STATE_CONTEXT:-${MANYFORGE_STATE_CONTEXT:-first_then_every_n}}" \
            BRIDGE_STATE_CONTEXT_EVERY_N="${BRIDGE_STATE_CONTEXT_EVERY_N:-${MANYFORGE_STATE_CONTEXT_EVERY_N:-3}}" \
            setsid "${python}" -m manyforge_assistant_bridge.bridge \
            </dev/null >"${BRIDGE_LOG}" 2>&1 &
        echo $! >"${BRIDGE_PID_FILE}"
    )
    # Liveness check: bridge can be alive while its upstream (vLLM/gateway)
    # is still warming up. /healthz readiness is the supervisor's job and
    # appears in the Composer status popup.
    sleep "${BRIDGE_LIVENESS_GRACE_S}"
    if ! bridge_pid_alive; then
        fail "Bridge exited within ${BRIDGE_LIVENESS_GRACE_S}s; see ${BRIDGE_LOG}"
        bridge_logs >&2
        return 1
    fi
    pass "Bridge launched at ${BRIDGE_URL} (supervisor reports readiness)"
}

start_bridge_hermes() {
    # Phase 4 of THREE-LANE-MIGRATION-PLAN.md. The Hermes lane bridge is a
    # FastAPI service (NemoClaw-Thor manyforge/lanes/hermes/service.py) that
    # submits each Composer turn as a Hermes /v1/runs run and observes its
    # event stream. It runs host-side (like the direct bridge), talking to the
    # Hermes gateway inside the sandbox at HERMES_BASE_URL. The Hermes SANDBOX
    # itself (policy + mcp_servers + gateway) is provisioned separately by
    # scripts/setup-hermes.sh per the Phase 0.5 spike's order-of-ops.
    step "Starting Hermes assistant bridge (hermes / native /v1/runs lane)"
    local port=8300
    if bridge_pid_alive; then
        pass "Reusing bridge from ${BRIDGE_PID_FILE}"
        return 0
    fi
    # Idempotent adoption of an externally-started hermes bridge (mirrors the
    # openclaw lane): adopt iff its /healthz self-identifies as provider=hermes.
    if port_is_listening "${port}"; then
        local healthz_resp
        healthz_resp="$(curl -fsS -m 2 "${BRIDGE_URL}/healthz" 2>/dev/null || true)"
        if [[ "${healthz_resp}" == *'"provider":"hermes"'* ]]; then
            local adopted_pid
            adopted_pid="$(ss -tlnp 2>/dev/null | awk -v p=":${port}" '$4 ~ p {print}' \
                | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)"
            if [[ -n "${adopted_pid}" ]] && kill -0 "${adopted_pid}" 2>/dev/null; then
                ensure_log_dir
                echo "${adopted_pid}" > "${BRIDGE_PID_FILE}"
                pass "Adopted externally-started hermes bridge at ${BRIDGE_URL} (pid ${adopted_pid})"
                return 0
            fi
        fi
        fail "Port ${port} is in use by a non-hermes service; refusing to start another bridge."
        port_listeners "${port}" >&2
        return 1
    fi
    local hermes_root="${NEMOCLAW_THOR}/manyforge"
    if [[ ! -f "${hermes_root}/lanes/hermes/service.py" ]]; then
        fail "hermes bridge service not found at: ${hermes_root}/lanes/hermes/service.py"
        info "Set NEMOCLAW_THOR or fall back to ASSISTANT_PROVIDER=direct."
        return 1
    fi
    # Bridge venv: setup-hermes.sh creates lanes/hermes/.venv with
    # fastapi+uvicorn+httpx(+prometheus). HERMES_BRIDGE_PYTHON overrides.
    local python="${HERMES_BRIDGE_PYTHON:-${hermes_root}/lanes/hermes/.venv/bin/python}"
    [[ -x "${python}" ]] || python="python3"
    local hermes_sandbox="${HERMES_ASSISTANT_SANDBOX:-hermes-assistant}"
    # Auto-extract the per-sandbox Hermes API_SERVER_KEY (minted by
    # setup-hermes.sh into /sandbox/.hermes/.env). Read it inside the container
    # and keep it out of argv/logs: export it so the bridge subshell inherits it
    # rather than passing it on the `env` command line (which `ps` would expose).
    if [[ -z "${API_SERVER_KEY:-}" ]]; then
        local _hermes_cid
        _hermes_cid="$(docker ps --format '{{.Names}}' 2>/dev/null | grep -E "^openshell-${hermes_sandbox}-" | head -1)"
        if [[ -n "${_hermes_cid}" ]]; then
            API_SERVER_KEY="$(docker exec -u root "${_hermes_cid}" bash -lc 'set -a; . /sandbox/.hermes/.env 2>/dev/null; set +a; printf "%s" "${API_SERVER_KEY:-}"' 2>/dev/null || true)"
        fi
        if [[ -z "${API_SERVER_KEY:-}" ]]; then
            warn "Could not auto-extract Hermes API_SERVER_KEY from sandbox '${hermes_sandbox}'; bridge will start unauthenticated. Export API_SERVER_KEY to override."
        else
            info "Extracted Hermes API_SERVER_KEY from sandbox '${hermes_sandbox}' (kept out of logs)."
        fi
    fi
    export API_SERVER_KEY
    ensure_log_dir
    (
        cd "${hermes_root}"
        env \
            HERMES_BASE_URL="${HERMES_BASE_URL:-http://127.0.0.1:8642}" \
            SANDBOX_NAME="${hermes_sandbox}" \
            MANYFORGE_MODEL="${MODEL_PROFILE}" \
            MANYFORGE_PRINCIPAL="${MANYFORGE_PRINCIPAL:-hermes-${hermes_sandbox}}" \
            MANYFORGE_HERMES_RUN_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
            MANYFORGE_HERMES_AUDIT_PATH="${BRIDGE_AUDIT_LOG%/*}/hermes-bridge-audit.jsonl" \
            setsid "${python}" -m uvicorn lanes.hermes.service:app \
            --host 127.0.0.1 --port "${port}" \
            </dev/null >"${BRIDGE_LOG}" 2>&1 &
        echo $! >"${BRIDGE_PID_FILE}"
    )
    sleep "${BRIDGE_LIVENESS_GRACE_S}"
    if ! bridge_pid_alive; then
        fail "Hermes bridge exited within ${BRIDGE_LIVENESS_GRACE_S}s; see ${BRIDGE_LOG}"
        bridge_logs >&2
        info "Common cause: lanes/hermes/.venv missing fastapi/uvicorn/httpx — run scripts/setup-hermes.sh."
        return 1
    fi
    pass "Hermes bridge launched at ${BRIDGE_URL} (supervisor reports readiness)"
}

start_bridge_openclaw() {
    step "Starting OpenClaw assistant bridge (openclaw / persistent-gateway lane)"
    if bridge_pid_alive; then
        pass "Reusing bridge from ${BRIDGE_PID_FILE}"
        return 0
    fi
    # 2026-06-03: idempotent adoption of an externally-started bridge.
    # If port 8200 is listening and the listener responds to /healthz
    # with our expected envelope, we trust it and write its PID into
    # ${BRIDGE_PID_FILE} so future launcher invocations see it as
    # managed. This handles the case where an operator manually started
    # the bridge during stack recovery (e.g. after a partial launcher
    # failure) — refusing to recognize it would force a destructive
    # restart that would interrupt in-flight requests.
    if port_is_listening 8200; then
        local hostport="${BRIDGE_URL#http://}"
        hostport="${hostport#https://}"
        # Verify the bridge is OUR bridge (not some unrelated service)
        # by hitting its /healthz endpoint and matching the provider tag.
        local healthz_resp
        healthz_resp="$(curl -fsS -m 2 "${BRIDGE_URL}/healthz" 2>/dev/null || true)"
        if [[ "${healthz_resp}" == *'"provider":"openclaw"'* ]]; then
            local adopted_pid
            adopted_pid="$(ss -tlnp 2>/dev/null | awk -v port=":${BRIDGE_PORT:-8200}" '$4 ~ port {print}' \
                | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)"
            if [[ -n "${adopted_pid}" ]] && kill -0 "${adopted_pid}" 2>/dev/null; then
                ensure_log_dir
                echo "${adopted_pid}" > "${BRIDGE_PID_FILE}"
                pass "Adopted externally-started bridge at ${BRIDGE_URL} (pid ${adopted_pid})"
                return 0
            fi
        fi
        fail "Port 8200 is in use by a non-OpenClaw service; refusing to start another bridge."
        port_listeners 8200 >&2
        return 1
    fi
    local launcher="${NEMOCLAW_THOR}/manyforge/start-openclaw-assistant-bridge.sh"
    if [[ ! -x "${launcher}" ]]; then
        fail "openclaw bridge launcher not found at: ${launcher}"
        info "Set NEMOCLAW_THOR or fall back to ASSISTANT_PROVIDER=direct."
        return 1
    fi
    ensure_log_dir
    # 2026-06-03: ensure the OpenClaw gateway is listening in the sandbox
    # before starting the bridge. The bridge's CLI shell-out path
    # invokes `openclaw agent` which expects the gateway at
    # ws://127.0.0.1:18789. If the gateway is dead (e.g. after an
    # earlier daemon-kill during tools-mode flip), `openclaw agent`
    # falls back to embedded mode and most tool calls fail with a
    # `GatewayTransportError: gateway closed`. Start it lazily here so
    # the operator doesn't have to remember to do it.
    local sandbox_name="${OPENCLAW_ASSISTANT_SANDBOX:-my-assistant}"
    local cluster_container="${OPENCLAW_ASSISTANT_CLUSTER_CONTAINER:-openshell-cluster-nemoclaw}"
    # OpenShell typically hosts the sandbox as a single docker container
    # whose name starts with "openshell-${sandbox_name}-". Find it once.
    local sandbox_container
    sandbox_container="$(docker ps --format '{{.Names}}' 2>/dev/null \
        | grep -E "^openshell-${sandbox_name}-" | head -1)"
    if [[ -n "${sandbox_container}" ]]; then
        if ! docker exec "${sandbox_container}" sh -c "ss -tlnp 2>/dev/null | grep -q ':18789'" 2>/dev/null; then
            info "  OpenClaw gateway not listening in ${sandbox_container} — starting it"
            docker exec -d "${sandbox_container}" bash -c \
                'cd /sandbox; exec openclaw gateway --allow-unconfigured --bind loopback --auth token >/sandbox/openclaw-gateway.log 2>&1'
            # Wait up to 30s for the gateway to come up.
            local _gateway_attempt
            for _gateway_attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
                if docker exec "${sandbox_container}" sh -c "ss -tlnp 2>/dev/null | grep -q ':18789'" 2>/dev/null; then
                    pass "  OpenClaw gateway listening on :18789 in sandbox"
                    break
                fi
                sleep 2
            done
            if ! docker exec "${sandbox_container}" sh -c "ss -tlnp 2>/dev/null | grep -q ':18789'" 2>/dev/null; then
                fail "  OpenClaw gateway failed to start in ${sandbox_container} (see /sandbox/openclaw-gateway.log)"
                # Don't return — the bridge can still run; just warn.
            fi
        else
            pass "  OpenClaw gateway already listening in ${sandbox_container}"
        fi
    else
        info "  No sandbox container matching openshell-${sandbox_name}-* found; bridge will spawn agents via nemoclaw exec only"
    fi
    # REVISED 2026-06-03 per THREE-LANE-MIGRATION-PLAN route fix:
    # OPENCLAW_ASSISTANT_USE_GATEWAY defaults to false because OpenClaw
    # 2026.5.22 does not expose /v1/chat/completions on the gateway
    # HTTP server — only /v1/responses, /v1/embeddings, /v1/models,
    # /v1/token. The gateway_http transport returns 404 "Not Found"
    # in 50ms for every call. CLI shell-out (the false branch) uses
    # `openclaw agent` via `nemoclaw exec`, which uses OpenClaw's
    # internal agent runner and reaches the inference provider as
    # intended. Set to "true" to force the legacy gateway_http path
    # (will break until upstream OpenClaw exposes /v1/chat/completions
    # or the bridge migrates to /v1/responses).
    (
        cd "${NEMOCLAW_THOR}"
        env \
            OPENCLAW_ASSISTANT_USE_GATEWAY="${OPENCLAW_ASSISTANT_USE_GATEWAY:-false}" \
            OPENCLAW_ASSISTANT_AGENT=manyforge-composer \
            OPENCLAW_ASSISTANT_LOCAL=false \
            OPENCLAW_ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}" \
            OPENCLAW_ASSISTANT_BRIDGE_HOST=127.0.0.1 \
            OPENCLAW_ASSISTANT_BRIDGE_PORT=8200 \
            OPENCLAW_ASSISTANT_COMPACT_EVERY_N="${OPENCLAW_ASSISTANT_COMPACT_EVERY_N}" \
            OPENCLAW_ASSISTANT_COMPACT_TIMEOUT_S="${OPENCLAW_ASSISTANT_COMPACT_TIMEOUT_S}" \
            OPENCLAW_ASSISTANT_METRICS_ENABLED="${OPENCLAW_ASSISTANT_METRICS_ENABLED}" \
            OPENCLAW_ASSISTANT_LOOP_TOOL_THRESHOLD="${OPENCLAW_ASSISTANT_LOOP_TOOL_THRESHOLD}" \
            OPENCLAW_ASSISTANT_LOOP_ARGS_THRESHOLD="${OPENCLAW_ASSISTANT_LOOP_ARGS_THRESHOLD}" \
            OPENCLAW_ASSISTANT_TOOL_SURFACE="${OPENCLAW_ASSISTANT_TOOL_SURFACE}" \
            MANYFORGE_STATE_CONTEXT="${MANYFORGE_STATE_CONTEXT:-first_then_every_n}" \
            MANYFORGE_STATE_CONTEXT_EVERY_N="${MANYFORGE_STATE_CONTEXT_EVERY_N:-3}" \
            OPENCLAW_ASSISTANT_STATE_CONTEXT="${OPENCLAW_ASSISTANT_STATE_CONTEXT:-${MANYFORGE_STATE_CONTEXT:-first_then_every_n}}" \
            OPENCLAW_ASSISTANT_STATE_CONTEXT_EVERY_N="${OPENCLAW_ASSISTANT_STATE_CONTEXT_EVERY_N:-${MANYFORGE_STATE_CONTEXT_EVERY_N:-3}}" \
            setsid "${launcher}" </dev/null >"${BRIDGE_LOG}" 2>&1 &
        echo $! >"${BRIDGE_PID_FILE}"
    )
    # Liveness check only: bridge process up, openclaw gateway readiness
    # is reported by the supervisor.
    sleep "${BRIDGE_LIVENESS_GRACE_S}"
    if ! bridge_pid_alive; then
        fail "OpenClaw bridge exited within ${BRIDGE_LIVENESS_GRACE_S}s; see ${BRIDGE_LOG}"
        bridge_logs >&2
        return 1
    fi
    pass "OpenClaw bridge launched at ${BRIDGE_URL} (supervisor reports readiness)"
}

# wait_for_bridge — strict /healthz probe for `smoke`.
wait_for_bridge() {
    local hint="Bridge /healthz returns 200 once vLLM upstream + (for openclaw) gateway are responsive. Override with ASSISTANT_BRIDGE_READY_TIMEOUT_S=NN."
    wait_with_progress \
        "Assistant bridge /healthz at ${BRIDGE_URL}" \
        "${ASSISTANT_BRIDGE_READY_TIMEOUT_S}" \
        "${hint}" \
        bridge_logs \
        bridge_healthz_ready
}

# start_assistant_supervisor — boots the new Python supervisor that
# probes vLLM / proxy / OpenClaw / bridge and POSTs to
# /api/runtime/external/assistant_supervisor, so the Composer monitor
# popup gets per-component rows for the AI stack.
start_assistant_supervisor() {
    step "Starting assistant supervisor (status feeder)"
    if supervisor_pid_alive; then
        pass "Reusing supervisor from ${SUPERVISOR_PID_FILE}"
        return 0
    fi
    install_bridge_deps
    local python
    python="$(bridge_python)"
    ensure_log_dir
    (
        cd "${MANYFORGE_ROOT}"
        env \
            COMPOSER_URL="${COMPOSER_URL}" \
            VLLM_DIRECT_URL="${VLLM_DIRECT_URL}" \
            UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL}" \
            START_VLLM_PROXY="${START_VLLM_PROXY}" \
            ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER}" \
            BRIDGE_URL="${BRIDGE_URL}" \
            MODEL_PROFILE="${MODEL_PROFILE}" \
            setsid "${python}" -m manyforge_assistant_bridge.supervisor \
            </dev/null >"${SUPERVISOR_LOG}" 2>&1 &
        echo $! >"${SUPERVISOR_PID_FILE}"
    )
    # Supervisor must stay alive past the grace period — without it the
    # Composer popup is missing the assistant rows. Poll once per second:
    # if the supervisor exits at +Ns we report it immediately with the
    # log tail; otherwise we wait the full grace and confirm it's still
    # running. wait_with_progress isn't appropriate here because its
    # probe returns success on the FIRST tick (PID is alive immediately
    # after launch), which would exit at t=0 without proving anything.
    info "Waiting ${ASSISTANT_SUPERVISOR_GRACE_S}s for supervisor to stay alive past import..."
    local elapsed=0
    while (( elapsed < ASSISTANT_SUPERVISOR_GRACE_S )); do
        if ! supervisor_pid_alive; then
            fail "Assistant supervisor exited at +${elapsed}s during ${ASSISTANT_SUPERVISOR_GRACE_S}s grace; see ${SUPERVISOR_LOG}"
            if [[ -f "${SUPERVISOR_LOG}" ]]; then
                info "last 20 lines of ${SUPERVISOR_LOG}:"
                tail -n 20 "${SUPERVISOR_LOG}" >&2 || true
            fi
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    pass "Assistant supervisor running (pid $(cat "${SUPERVISOR_PID_FILE}" 2>/dev/null || echo '?'))"
}

# stop_assistant_stack — tears down everything start_* started. Idempotent.
stop_assistant_stack() {
    step "Stopping assistant stack"
    # Supervisor first so it can post a final "stopping" status if needed.
    if supervisor_pid_alive; then
        local pid
        pid="$(cat "${SUPERVISOR_PID_FILE}")"
        kill "${pid}" >/dev/null 2>&1 || true
    fi
    rm -f "${SUPERVISOR_PID_FILE}"
    pkill -f '[m]anyforge_assistant_bridge.supervisor' 2>/dev/null || true

    if bridge_pid_alive; then
        local pid
        pid="$(cat "${BRIDGE_PID_FILE}")"
        kill "${pid}" >/dev/null 2>&1 || true
    fi
    rm -f "${BRIDGE_PID_FILE}"
    pkill -f '[m]anyforge_assistant_bridge.bridge' 2>/dev/null || true
    pkill -f '[o]penclaw_assistant_bridge.service' 2>/dev/null || true

    if [[ -f "${LOG_DIR}/vllm-proxy.pid" ]]; then
        local pid
        pid="$(cat "${LOG_DIR}/vllm-proxy.pid")"
        kill "${pid}" >/dev/null 2>&1 || true
        rm -f "${LOG_DIR}/vllm-proxy.pid"
    fi
    pkill -f '[v]llm-proxy.py' 2>/dev/null || true

    docker rm -f "${VLLM_CONTAINER}" >/dev/null 2>&1 || true

    # Free unified memory now that vLLM is gone. On Thor this is the
    # only reliable way to return CUDA-mapped pages to the OS;
    # without it `free -h` still reports the model's footprint as
    # buff/cache until something else evicts it. See drop_caches()
    # docstring for the DROP_CACHES + sudo notes.
    drop_caches
}
