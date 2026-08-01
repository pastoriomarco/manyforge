# shellcheck shell=bash
# Assistant-lane configuration shell and typed fail-before-effects entrypoints.
#
# The complete lifecycle implementation is pinned at
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# 6a5935026a93984d4374921b4f6f8b08f770eced. It is not a runnable fallback.
#
# Historical responsibility mapping by symbol:
#   resolve_assistant_bridge_url
#       provider selection and old fixed endpoint map -> future client config;
#   bridge_pid_alive, supervisor_pid_alive, ensure_log_dir, drop_caches,
#   served_model_id, bridge_python, install_bridge_deps
#       process/resource/deployment helpers -> future bounded operator support;
#   vllm_container_alive, vllm_container_logs, vllm_models_ready, start_model,
#   wait_for_vllm_model, vllm_proxy_pid_alive, vllm_proxy_port_bound,
#   vllm_proxy_logs, vllm_proxy_models_ready, start_vllm_proxy,
#   wait_for_vllm_proxy
#       model-serving lifecycle and readiness -> assistant deployment owner;
#   configure_nemoclaw, provision_openclaw_sandbox
#       platform configuration and sandbox provisioning -> deployment owner;
#   start_bridge, bridge_logs, bridge_healthz_ready, start_bridge_nemoclaw,
#   start_bridge_hermes, start_bridge_openclaw, wait_for_bridge
#       provider-specific bridge lifecycle/readiness -> future assistant client;
#   start_assistant_supervisor
#       old Composer status feeder -> headless status client; and
#   stop_assistant_stack
#       bounded teardown -> future deployment workflow.
#
# Only value defaults and provider-name validation remain. Every public
# effectful entrypoint fails before network, process, package, file, Docker,
# OpenShell, or Composer activity; no historical function body is dormant.

if [[ -n "${_MANYFORGE_LIB_ASSISTANT:-}" ]]; then
    return 0
fi
_MANYFORGE_LIB_ASSISTANT=1

_manyforge_assistant_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=composable-cell-runtime-availability.sh
source "${_manyforge_assistant_lib_dir}/composable-cell-runtime-availability.sh"

# Owner-neutral operator selections retained for launch-plan rendering. They
# do not imply an endpoint or available product lane.
MODEL_PROFILE="${MODEL_PROFILE:-gemma4-12b-it-gguf}"
ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER:-openclaw}"
START_VLLM_PROXY="${START_VLLM_PROXY:-true}"
ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S:-300}"
VLLM_PORT="${VLLM_PORT:-8050}"
UPSTREAM_BASE_URL="${UPSTREAM_BASE_URL:-http://127.0.0.1:8000/v1}"
BRIDGE_URL="${BRIDGE_URL:-}"

manyforge_fail_assistant_runtime_unavailable() {
    printf '%s\n' \
        "${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: the assistant lane returns with the qualified headless assistant client." \
        >&2
    return 78
}

resolve_assistant_bridge_url() {
    case "${ASSISTANT_PROVIDER}" in
        direct|nemoclaw|openclaw|hermes|"") ;;
        *)
            printf 'unknown assistant provider: %s\n' \
                "${ASSISTANT_PROVIDER}" >&2
            return 2
            ;;
    esac
    manyforge_fail_assistant_runtime_unavailable
}

start_model() {
    manyforge_fail_assistant_runtime_unavailable
}

wait_for_vllm_model() {
    manyforge_fail_assistant_runtime_unavailable
}

start_vllm_proxy() {
    manyforge_fail_assistant_runtime_unavailable
}

wait_for_vllm_proxy() {
    manyforge_fail_assistant_runtime_unavailable
}

configure_nemoclaw() {
    manyforge_fail_assistant_runtime_unavailable
}

provision_openclaw_sandbox() {
    manyforge_fail_assistant_runtime_unavailable
}

start_bridge() {
    manyforge_fail_assistant_runtime_unavailable
}

wait_for_bridge() {
    manyforge_fail_assistant_runtime_unavailable
}

start_assistant_supervisor() {
    manyforge_fail_assistant_runtime_unavailable
}

stop_assistant_stack() {
    manyforge_fail_assistant_runtime_unavailable
}

unset _manyforge_assistant_lib_dir
