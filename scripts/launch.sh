#!/usr/bin/env bash
# scripts/launch.sh — ManyForge unified launcher.
#
# Replaces the seven scripts/demo-*.sh launchers with one NemoClaw-style
# wizard. Selects: lane (manyforge-only | isaac-sim | real-robot) × assistant (on | off)
# × scenario (named bundle of program + deployment YAML).
#
# Operator-facing test recipe + smoke sequence:
#   docs/operations/LAUNCHER_QUICKSTART.md
# Backward-compat shim behavior + migration notes:
#   scripts/MIGRATION.md
# Lib module helpers:
#   scripts/lib/*.sh
#
# MAINTAINER NOTE — keep the quickstart and MIGRATION docs in sync with
# this file. If you add/remove a verb, flag, lane, scenario, or env var
# below, update docs/operations/LAUNCHER_QUICKSTART.md in the same
# commit. CI does not enforce this; the AGENTS.md branch+commit
# workflow and the PR template checklist call it out.
#
# Verbs:
#   start (default)   Run wizard / non-interactive bring-up
#   status            Print component state + popup URL
#   stop              Tear down everything this launcher started
#   restart           stop + start
#   smoke             start + run the matching smoke test
#   preflight         Run port/docker/image preflight only
#
# CLI flags:
#   --lane {manyforge-only|isaac-sim|real-robot}
#   --assistant {on|off}
#   --scenario NAME            (or --program PATH --deployment PATH)
#   --program PATH
#   --deployment PATH
#   --non-interactive          (env: MANYFORGE_NON_INTERACTIVE=1)
#   --yes / -y                 (env: MANYFORGE_ASSUME_YES=1) — skip review gate
#   -h | --help
#
# Common env overrides (REBUILD, STOP_KNOWN_CONFLICTS, COMPOSER_URL,
# MODEL_PROFILE, ASSISTANT_PROVIDER, START_VLLM_PROXY, …) are read by the
# relevant lib module.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export MANYFORGE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Capture the operator's EXPLICITLY-requested model profile and assistant lane
# before lib/assistant.sh defaults MODEL_PROFILE / ASSISTANT_PROVIDER — so the
# wizard can tell a real preset (env or flag) apart from the built-in default
# and knows whether to prompt.
MODEL_PROFILE_REQUESTED="${MODEL_PROFILE:-}"
ASSISTANT_PROVIDER_REQUESTED="${ASSISTANT_PROVIDER:-}"

# shellcheck source=lib/runner.sh
source "${SCRIPT_DIR}/lib/runner.sh"
# shellcheck source=lib/prompts.sh
source "${SCRIPT_DIR}/lib/prompts.sh"
# shellcheck source=lib/preflight.sh
source "${SCRIPT_DIR}/lib/preflight.sh"
# shellcheck source=lib/scenarios.sh
source "${SCRIPT_DIR}/lib/scenarios.sh"
# shellcheck source=lib/compose.sh
source "${SCRIPT_DIR}/lib/compose.sh"
# shellcheck source=lib/composer.sh
source "${SCRIPT_DIR}/lib/composer.sh"
# shellcheck source=lib/isaac.sh
source "${SCRIPT_DIR}/lib/isaac.sh"
# shellcheck source=lib/real_robot.sh
source "${SCRIPT_DIR}/lib/real_robot.sh"
# shellcheck source=lib/assistant.sh
source "${SCRIPT_DIR}/lib/assistant.sh"

install_error_trap

# ── State variables (populated by CLI parsing and the wizard) ────────────────
# 2026-06-03: accept env-set ASSISTANT / LANE / SCENARIO so non-interactive
# callers (CI, scripts) can drive the launcher without remembering the exact
# CLI flag spelling. CLI flags override env. Empty env is treated as unset
# (the picker runs) so interactive use is unchanged.
LANE="${LANE:-}"
ASSISTANT="${ASSISTANT:-}"
SCENARIO="${SCENARIO:-}"
PLATFORM="${PLATFORM:-}"
PROGRAM_PATH=""
DEPLOYMENT_PATH=""
REAL_ROBOT_RESOLVER=""
REAL_ROBOT_CELL_PATH=""
REAL_ROBOT_PROGRAM_PATH=""
REAL_ROBOT_DEPLOYMENT_TEMPLATE=""
VERB="start"
STOP_KNOWN_CONFLICTS="${STOP_KNOWN_CONFLICTS:-false}"

# ── CLI parsing ───────────────────────────────────────────────────────────────

usage() {
    cat <<EOF
Usage: scripts/launch.sh [VERB] [OPTIONS]

Verbs:
  start (default)             Run wizard / non-interactive bring-up
  status                      Print component state + popup URL
  stop                        Tear down everything this launcher started
  restart                     stop + start
  smoke                       start + run the matching smoke test
  preflight                   Run port/docker/image preflight only

Selection (skip prompts when supplied):
  --lane {manyforge-only|isaac-sim|real-robot}
  --assistant {on|off}
  --provider {direct|openclaw|hermes}
                              Assistant only: which lane/bridge to run.
                              Env: ASSISTANT_PROVIDER. Default: openclaw.
  --platform NAME             Assistant only: target platform (thor|orin|…),
                              filters the recipe list. Default: auto-detected host.
  --model-profile NAME        Assistant only: model recipe (see serving/config.sh).
                              Env: MODEL_PROFILE. Skips the recipe prompt.
  --scenario NAME             Or --program/--deployment for free form
  --program PATH
  --deployment PATH
  --non-interactive           Env: MANYFORGE_NON_INTERACTIVE=1
  --yes, -y                   Skip the review-gate confirmation
  --list-scenarios            Print all named scenarios and exit
  -h, --help                  This help

Common overrides (read by lib modules):
  REBUILD={auto|true|false}    STOP_KNOWN_CONFLICTS={true|false}  (default false)
  COMPOSER_URL=...             COMPOSER_PORT=...

  Start-side timeouts (seconds — applied during 'start'):
    COMPOSER_READY_TIMEOUT_S=600   (blocking — Composer /api/health)
    CTEST_TEST_TIMEOUT_S=60        (per-test deadlock guard for startup C++/RoboPlan ctests)
    VLLM_LIVENESS_GRACE_S=5        (container still running after launch)
    PROXY_LIVENESS_GRACE_S=2       (proxy pid alive + port bound)
    BRIDGE_LIVENESS_GRACE_S=5      (bridge pid alive)
    ASSISTANT_SUPERVISOR_GRACE_S=3  (supervisor pid alive past import)

  Smoke-side timeouts (seconds — applied during 'smoke', strict readiness):
    VLLM_MODEL_READY_TIMEOUT_S=900    (/v1/models + served id matches)
    VLLM_PROXY_READY_TIMEOUT_S=30     (proxy /v1/models proxies through)
    ASSISTANT_BRIDGE_READY_TIMEOUT_S=60 (bridge /healthz returns 200)
    ROS_DISCOVERY_TIMEOUT_S=45        (Isaac topics + actions present)

  Progress display:
    MANYFORGE_PROGRESS_MODE={auto|plain}  (plain = no ANSI redraw; for CI)

  Collision validation:
    MANYFORGE_VALIDATION_COLLISION_WORKERS=N
      Override the default authoritative trajectory-validation worker count
      (default 4; use 1-2 on constrained deployments if controller / observer
      latency qualification requires it).

  Assistant:  MODEL_PROFILE  ASSISTANT_PROVIDER  START_VLLM_PROXY
              OPENCLAW_ASSISTANT_COMPACT_EVERY_N  VLLM_PORT
              OPENCLAW_ASSISTANT_TOOL_SURFACE   (tools|code; default tools — see LANE-COMPARISON)
  Isaac:      ARM_ACTION  GRIPPER_ACTION
              ISAAC_JOINT_STATE_TOPIC  ARM_CONTROLLER_STATE_TOPIC
  Real robot: MANYFORGE_LITE6_ROBOT_IP
              MANYFORGE_LITE6_ROS_DOMAIN_ID  MANYFORGE_REAL_LITE6_IMAGE
              MANYFORGE_REAL_ROBOT_OWNER_READY_TIMEOUT_S  (default 600)

Examples:
  scripts/launch.sh                                  # Interactive wizard
  scripts/launch.sh --lane manyforge-only --scenario ur5-pick-place
  scripts/launch.sh --lane isaac-sim --scenario isaac-ur10e-light-scene start
  MANYFORGE_LITE6_ROBOT_IP=192.168.1.31 scripts/launch.sh \\
    --lane real-robot --assistant off --scenario real-lite6-commissioning
  scripts/launch.sh --lane manyforge-only --assistant on --scenario ur10e-scene-authoring
  scripts/launch.sh stop
EOF
}

parse_args() {
    # Accept the verb in any position; collect positionals and pick up
    # the verb at the end. This makes shim forwarding clean:
    #   demo-isaac-ros2-control.sh start
    #     → launch.sh --lane isaac-sim --scenario isaac-light start
    local positionals=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --lane) LANE="$2"; shift 2 ;;
            --assistant) ASSISTANT="$2"; shift 2 ;;
            --provider) ASSISTANT_PROVIDER_REQUESTED="$2"; shift 2 ;;
            --platform) PLATFORM="$2"; shift 2 ;;
            --model-profile) MODEL_PROFILE_REQUESTED="$2"; shift 2 ;;
            --scenario) SCENARIO="$2"; shift 2 ;;
            --program) PROGRAM_PATH="$2"; shift 2 ;;
            --deployment) DEPLOYMENT_PATH="$2"; shift 2 ;;
            --non-interactive) export MANYFORGE_NON_INTERACTIVE=1; shift ;;
            --yes|-y) export MANYFORGE_ASSUME_YES=1; shift ;;
            --list-scenarios) scenarios_print_table; exit 0 ;;
            -h|--help) usage; exit 0 ;;
            --) shift; positionals+=("$@"); break ;;
            -*)
                fail "Unknown option: $1"
                usage >&2
                exit 2
                ;;
            *)
                positionals+=("$1")
                shift
                ;;
        esac
    done
    if (( ${#positionals[@]} > 1 )); then
        fail "Too many positional arguments: ${positionals[*]}"
        usage >&2
        exit 2
    fi
    if (( ${#positionals[@]} == 1 )); then
        case "${positionals[0]}" in
            start|status|stop|restart|smoke|preflight)
                VERB="${positionals[0]}"
                ;;
            "")
                # empty positional (rare; harmless)
                ;;
            *)
                fail "Unknown verb: ${positionals[0]}"
                usage >&2
                exit 2
                ;;
        esac
    fi
}

# ── Decision phase ─────────────────────────────────────────────────────────────

step_choose_lane() {
    _CURRENT_STEP_LABEL="[1/7] choose lane"
    step "[1/7] Choose lane"
    if [[ -n "${LANE}" ]]; then
        info "Lane preset via flag: ${LANE}"
    else
        LANE="$(pick_from_list "Execution lane" 1 manyforge-only isaac-sim real-robot)"
    fi
    case "${LANE}" in
        manyforge-only|isaac-sim|real-robot) ;;
        *) fail "Invalid lane: ${LANE}"; exit 2 ;;
    esac
    pass "Lane: ${LANE}"
}

step_choose_assistant() {
    _CURRENT_STEP_LABEL="[2/7] choose assistant"
    step "[2/7] Activate AI assistant?"
    if [[ -n "${ASSISTANT}" ]]; then
        info "Assistant preset via flag: ${ASSISTANT}"
    else
        ASSISTANT="$(pick_from_list "Activate AI assistant" 2 on off)"
    fi
    case "${ASSISTANT}" in
        on|off) ;;
        *) fail "Invalid assistant value: ${ASSISTANT}"; exit 2 ;;
    esac
    pass "Assistant: ${ASSISTANT}"

    # When the assistant is on, choose the lane (direct/openclaw/hermes), the
    # target platform, and the model recipe. The recipe catalog (names + platform
    # tags) is the single source of truth in NemoClaw-Thor's serving/config.sh.
    if [[ "${ASSISTANT}" == "on" ]]; then
        step_choose_assistant_lane_and_model
    fi
}

# Query the NemoClaw-Thor model catalog. config.sh is source-only and prepends
# to PATH at load, so run it in an isolated subshell and capture only the helper
# output — nothing leaks into the launcher's shell.
_model_catalog() {
    ( source "${NEMOCLAW_THOR}/serving/config.sh" >/dev/null 2>&1 && "$@" )
}

step_choose_assistant_lane_and_model() {
    # ── Assistant lane / provider (direct / openclaw / hermes) ───────────────
    # Independent of the model: every lane drives the same vLLM endpoint; the
    # lane only chooses the bridge / agent-loop path.
    local lanes=(direct openclaw hermes)
    if [[ -n "${ASSISTANT_PROVIDER_REQUESTED}" ]]; then
        ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER_REQUESTED}"
        info "Assistant lane preset: ${ASSISTANT_PROVIDER}"
    else
        printf '\n  %sAssistant lanes:%s\n' "${_C_BOLD}" "${_C_RESET}" >&2
        printf '    %-9s %s\n' "direct"   "in-process bridge → vLLM (:8100); fastest, no gateway hops" >&2
        printf '    %-9s %s\n' "openclaw" "OpenClaw gateway agent (:8200); current default" >&2
        printf '    %-9s %s\n' "hermes"   "Hermes sessions + memory/skills (:8300); experimental, opt-in" >&2
        local i default_lane_idx=1
        for (( i = 0; i < ${#lanes[@]}; i++ )); do
            [[ "${lanes[i]}" == "${ASSISTANT_PROVIDER:-openclaw}" ]] && { default_lane_idx=$((i + 1)); break; }
        done
        ASSISTANT_PROVIDER="$(pick_from_list "Assistant lane" "${default_lane_idx}" "${lanes[@]}")"
    fi
    case "${ASSISTANT_PROVIDER}" in
        direct|nemoclaw|openclaw|hermes) ;;
        *) fail "Unknown assistant lane '${ASSISTANT_PROVIDER}'. Choose: direct | openclaw | hermes."; exit 2 ;;
    esac
    # Hermes is gated (opt-in until its bake-off lands); an explicit choice here
    # IS the opt-in, so enable the gate rather than erroring out at bring-up.
    if [[ "${ASSISTANT_PROVIDER}" == "hermes" && "${HERMES_LANE_PHASE4_ENABLED:-false}" != "true" ]]; then
        export HERMES_LANE_PHASE4_ENABLED=true
        info "Hermes lane is experimental (Phase 4, opt-in) — enabling HERMES_LANE_PHASE4_ENABLED=true."
    fi
    export ASSISTANT_PROVIDER
    # BRIDGE_URL was resolved at source time against the default provider;
    # recompute it for the lane just chosen so the Composer endpoint, the
    # supervisor /healthz probe, and the plan/status all target the right port.
    resolve_assistant_bridge_url
    pass "Assistant lane: ${ASSISTANT_PROVIDER} (bridge ${BRIDGE_URL})"

    # ── Platform + model recipe (from the NemoClaw-Thor catalog) ─────────────
    local config="${NEMOCLAW_THOR}/serving/config.sh"
    if [[ ! -r "${config}" ]]; then
        warn "Model catalog ${config} not found; using MODEL_PROFILE=${MODEL_PROFILE}."
        return 0
    fi

    # ── Platform (filters the recipe list) ──────────────────────────────────
    local platforms=()
    mapfile -t platforms < <(_model_catalog list_model_platforms)
    if (( ${#platforms[@]} == 0 )); then
        warn "No platforms in catalog; using MODEL_PROFILE=${MODEL_PROFILE}."
        return 0
    fi
    if [[ -n "${PLATFORM}" ]]; then
        info "Platform preset via flag: ${PLATFORM}"
    else
        # Preselect the auto-detected host platform when it is in the catalog.
        local detected default_idx=1 i
        detected="$(_model_catalog detect_platform || true)"
        if [[ -n "${detected}" ]]; then
            info "Detected host platform: ${detected}"
            for (( i = 0; i < ${#platforms[@]}; i++ )); do
                [[ "${platforms[i]}" == "${detected}" ]] && { default_idx=$((i + 1)); break; }
            done
        fi
        PLATFORM="$(pick_from_list "Target platform" "${default_idx}" "${platforms[@]}")"
    fi
    local p ok=0
    for p in "${platforms[@]}"; do [[ "${p}" == "${PLATFORM}" ]] && ok=1; done
    if (( ! ok )); then
        fail "Unknown platform '${PLATFORM}'. Available: ${platforms[*]}"; exit 2
    fi
    pass "Platform: ${PLATFORM}"

    # ── Model recipe (filtered by platform) ─────────────────────────────────
    local rows=() names=() descs=() row name desc
    mapfile -t rows < <(_model_catalog list_model_profiles "${PLATFORM}")
    if (( ${#rows[@]} == 0 )); then
        fail "No model recipes registered for platform '${PLATFORM}'."; exit 2
    fi
    for row in "${rows[@]}"; do
        name="${row%%$'\t'*}"; desc="${row#*$'\t'}"
        names+=("${name}"); descs+=("${desc}")
    done

    # An explicit request (env MODEL_PROFILE / --model-profile) wins if it is a
    # recipe for the chosen platform — skip the prompt.
    if [[ -n "${MODEL_PROFILE_REQUESTED}" ]]; then
        local req_ok=0
        for name in "${names[@]}"; do [[ "${name}" == "${MODEL_PROFILE_REQUESTED}" ]] && req_ok=1; done
        if (( req_ok )); then
            MODEL_PROFILE="${MODEL_PROFILE_REQUESTED}"; export MODEL_PROFILE
            info "Model recipe preset: ${MODEL_PROFILE}"
            pass "Model recipe: ${MODEL_PROFILE}"
            return 0
        fi
        warn "Requested model '${MODEL_PROFILE_REQUESTED}' is not available for platform '${PLATFORM}'."
        if is_non_interactive; then
            fail "No valid model recipe for ${PLATFORM} in non-interactive mode."; exit 2
        fi
    fi

    # Show the filtered recipe table, then pick by number. Default to the
    # current MODEL_PROFILE if it is available for this platform (preserves the
    # built-in default), else the platform's first recipe.
    printf '\n  %sAvailable recipes for %s:%s\n' "${_C_BOLD}" "${PLATFORM}" "${_C_RESET}" >&2
    local i default_idx=1
    for (( i = 0; i < ${#names[@]}; i++ )); do
        printf '    %-44s %s\n' "${names[i]}" "${descs[i]}" >&2
        [[ "${names[i]}" == "${MODEL_PROFILE:-}" ]] && default_idx=$((i + 1))
    done
    MODEL_PROFILE="$(pick_from_list "Model recipe (${PLATFORM})" "${default_idx}" "${names[@]}")"
    export MODEL_PROFILE
    pass "Model recipe: ${MODEL_PROFILE}"
}

step_choose_scenario() {
    _CURRENT_STEP_LABEL="[3/7] choose scenario"
    step "[3/7] Pick scenario"

    # Resolution order:
    #  1. Free-form --program + --deployment → SCENARIO="custom", no table lookup
    #  2. Named --scenario → look up + verify lane/assistant match
    #  3. Neither → interactive picker filtered by lane × assistant
    # All three paths fall through to the assistant ↔ deployment cross-check below.
    if [[ -n "${PROGRAM_PATH}" && -n "${DEPLOYMENT_PATH}" ]]; then
        info "Free-form program/deployment supplied; skipping scenario lookup."
        SCENARIO="custom"
    elif [[ -n "${SCENARIO}" ]]; then
        local row
        if ! row="$(scenario_lookup "${SCENARIO}")"; then
            fail "Unknown scenario: ${SCENARIO}"
            scenarios_print_table
            exit 2
        fi
        # Canonicalise SCENARIO so an alias input (e.g. --scenario pick-place)
        # is replaced with the resolved canonical name (e.g. ur5-pick-place)
        # for the review-gate summary, status output, and the
        # "to reproduce non-interactively" hint at the end. Without this,
        # the launcher would report the deprecated alias as if it were
        # canonical, and copy-pasted reproduce-commands would propagate
        # the old name into operator scripts.
        SCENARIO="$(scenario_field "${row}" 1)"
        local scenario_lane scenario_assistant
        scenario_lane="$(scenario_field "${row}" 2)"
        scenario_assistant="$(scenario_field "${row}" 3)"
        if [[ "${scenario_lane}" != "${LANE}" ]]; then
            fail "Scenario '${SCENARIO}' is configured for lane=${scenario_lane}, but --lane ${LANE} was selected."
            info "Either pick a different scenario or change --lane to ${scenario_lane}."
            info "Scenarios for --lane ${LANE}:"
            local match
            while IFS= read -r match; do
                [[ -n "${match}" ]] && info "  - ${match}"
            done < <(scenarios_for "${LANE}" "any")
            exit 2
        fi
        if [[ "${scenario_assistant}" != "${ASSISTANT}" ]]; then
            fail "Scenario '${SCENARIO}' is configured for assistant=${scenario_assistant}, but --assistant ${ASSISTANT} was selected."
            info "Either pick a different scenario or change --assistant to ${scenario_assistant}."
            exit 2
        fi
        PROGRAM_PATH="$(scenario_field "${row}" 4)"
        DEPLOYMENT_PATH="$(scenario_field "${row}" 5)"
        REAL_ROBOT_RESOLVER="$(scenario_field "${row}" 8)"
        REAL_ROBOT_CELL_PATH="$(scenario_field "${row}" 9)"
        info "Scenario preset via flag: ${SCENARIO}"
    else
        local candidates=()
        while IFS= read -r name; do
            [[ -n "${name}" ]] && candidates+=("${name}")
        done < <(scenarios_for "${LANE}" "${ASSISTANT}")
        if (( ${#candidates[@]} == 0 )); then
            fail "No scenarios match lane=${LANE} assistant=${ASSISTANT}"
            info "Either pick a different lane/assistant combination, or supply --program PATH --deployment PATH directly."
            scenarios_print_table
            exit 2
        fi
        SCENARIO="$(pick_from_list "Scenario" 1 "${candidates[@]}")"
        local row
        row="$(scenario_lookup "${SCENARIO}")" || { fail "Scenario lookup failed for ${SCENARIO}"; exit 2; }
        # Canonicalise (interactive picker shows canonical names from the
        # table, but a future change might add aliases to the picker too).
        SCENARIO="$(scenario_field "${row}" 1)"
        PROGRAM_PATH="$(scenario_field "${row}" 4)"
        DEPLOYMENT_PATH="$(scenario_field "${row}" 5)"
        REAL_ROBOT_RESOLVER="$(scenario_field "${row}" 8)"
        REAL_ROBOT_CELL_PATH="$(scenario_field "${row}" 9)"
    fi

    # Cross-check: deployment YAML must agree with --assistant choice.
    if [[ "${ASSISTANT}" == "off" ]] && deployment_requires_assistant "${DEPLOYMENT_PATH}"; then
        fail "Deployment ${DEPLOYMENT_PATH} declares assistant.enabled=true but --assistant off was selected."
        info "Either pick --assistant on or choose a non-assistant scenario."
        exit 2
    fi
    # Reverse case: --assistant on with a deployment that does NOT enable
    # the assistant. The assistant stack (vLLM/proxy/bridge/supervisor)
    # will still be brought up, but the AI button stays hidden because the
    # frontend gates on `deployment.assistant.enabled`. Warn so the operator
    # isn't surprised; don't fail (legitimate use: stand up vLLM probes /
    # supervisor rows without exposing the AI overlay to the user).
    if [[ "${ASSISTANT}" == "on" ]] && ! deployment_requires_assistant "${DEPLOYMENT_PATH}"; then
        warn "Deployment ${DEPLOYMENT_PATH} has no assistant block (or assistant.enabled is false)."
        warn "  The assistant stack will be brought up, but the AI button stays HIDDEN in the Composer UI."
        warn "  Add an 'assistant: { enabled: true, provider: …, endpointAlias: … }' block to the deployment"
        warn "  YAML to expose the overlay."
    fi
    case "${SCENARIO}" in
        isaac-lite6-pick-place|isaac-lite6-trocar-foundationpose)
            export ISAAC_TOPIC_CONTROL_PROFILE="${ISAAC_TOPIC_CONTROL_PROFILE:-lite6}"
            export GRIPPER_TRANSPORT_KIND="${GRIPPER_TRANSPORT_KIND:-isaac_joint_state_effort}"
            export REQUIRE_GRIPPER_ACTION="${REQUIRE_GRIPPER_ACTION:-false}"
            ;;
    esac
    if [[ "${LANE}" == "real-robot" ]]; then
        if [[ -z "${REAL_ROBOT_RESOLVER}" || -z "${REAL_ROBOT_CELL_PATH}" ]]; then
            fail "real-robot startup requires an artifact-owned cell scenario; free-form unresolved templates are not accepted."
            exit 2
        fi
        REAL_ROBOT_PROGRAM_PATH="${PROGRAM_PATH}"
        REAL_ROBOT_DEPLOYMENT_TEMPLATE="${DEPLOYMENT_PATH}"
    fi
    pass "Scenario: ${SCENARIO}"
}

# ── Review gate ─────────────────────────────────────────────────────────────

review_gate() {
    if is_non_interactive || [[ "${MANYFORGE_ASSUME_YES:-0}" == "1" ]]; then
        info "Review gate skipped (non-interactive or --yes)."
        return 0
    fi
    cat >&2 <<EOF

${_C_BOLD}Plan:${_C_RESET}
  Lane:        ${LANE}
  Assistant:   ${ASSISTANT}
  Scenario:    ${SCENARIO}
    Program:     ${PROGRAM_PATH}
    Deployment:  ${DEPLOYMENT_PATH}
EOF
    if [[ "${LANE}" == "isaac-sim" ]]; then
        printf '  Compose:     profile=ros-full, services=composer-ros isaac-topic-control\n' >&2
        printf '  Supervisors: isaac_demo_supervisor (auto)' >&2
    elif [[ "${LANE}" == "real-robot" ]]; then
        printf '  Runtime:     sole-owner control service + non-owner Composer service\n' >&2
        printf '  Startup:     observation-only; drives and trajectory controller inactive\n' >&2
        printf '  Cell:        %s\n' "${REAL_ROBOT_CELL_PATH}" >&2
        printf '  Supervisors: robot lifecycle/admission provider\n' >&2
    else
        printf '  Compose:     dev container (detached), no profile\n' >&2
        printf '  Supervisors: (none required for lane)' >&2
    fi
    if [[ "${ASSISTANT}" == "on" ]]; then
        printf ' + assistant_supervisor\n' >&2
        printf '  Assistant:   provider=%s, model=%s, proxy=%s\n' \
            "${ASSISTANT_PROVIDER}" "${MODEL_PROFILE}" "${START_VLLM_PROXY}" >&2
    else
        printf '\n' >&2
    fi
    printf '  Endpoints:   Composer %s   Viewer native Composer panel\n' "${COMPOSER_URL}" >&2
    if [[ "${ASSISTANT}" == "on" ]]; then
        printf '               vLLM     %s   Bridge %s\n' "${UPSTREAM_BASE_URL}" "${BRIDGE_URL}" >&2
    fi
    echo >&2

    if ! confirm "Continue?"; then
        info "Aborted by user."
        exit 0
    fi
}

# ── Execution phase ────────────────────────────────────────────────────────────

step_preflight() {
    _CURRENT_STEP_LABEL="[4/7] preflight"
    step "[4/7] Preflight (ports, docker, image)"
    require_docker
    if [[ "${STOP_KNOWN_CONFLICTS}" == "true" ]]; then
        info "Removing any stale manyforge-e2e-* containers"
        stop_known_conflicts
        if [[ "${ASSISTANT}" == "on" ]]; then
            info "Stopping stale launcher-managed assistant processes"
            stop_assistant_stack
        fi
    else
        preflight_managed_components
    fi
    # Composer port is env-overridable; preflight checks the actual resolved
    # value so a `COMPOSER_PORT=9010 ./scripts/launch.sh start` checks 9010
    # instead of the default 9000.
    local ports=("${COMPOSER_PORT:-9000}")
    if [[ "${LANE}" == "isaac-sim" ]]; then
        # ros-full uses host network; composer-ros listens on COMPOSER_PORT.
        :
    fi
    if [[ "${ASSISTANT}" == "on" ]]; then
        ports+=(8000)
        if [[ "${START_VLLM_PROXY:-true}" == "true" ]]; then
            ports+=("${VLLM_PORT:-8050}")
        fi
        # Bridge port for the chosen lane — derive from BRIDGE_URL (resolved by
        # resolve_assistant_bridge_url) rather than re-mapping the provider here.
        ports+=("${BRIDGE_URL##*:}")
    fi
    if ! preflight_ports "${ports[@]}"; then
        if [[ "${STOP_KNOWN_CONFLICTS}" == "true" ]]; then
            warn "Some ports are still busy after stop_known_conflicts; aborting."
        fi
        fail "Preflight failed. Stop the conflicting processes and retry."
        exit 1
    fi
    case "${LANE}" in
        manyforge-only)
            require_image manyforge-dev:latest "docker compose build dev"
            ;;
        isaac-sim)
            compose_set_lane "${LANE}"
            maybe_build_isaac_image
            ;;
        real-robot)
            real_robot_preflight
            ;;
    esac
}

step_bring_up_lane() {
    _CURRENT_STEP_LABEL="[5/7] bring up lane"
    step "[5/7] Bring up ${LANE} infra"
    case "${LANE}" in
        manyforge-only)
            compose_set_lane "${LANE}"
            if [[ "${ASSISTANT}" == "on" ]]; then
                MANYFORGE_ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER}"
                MANYFORGE_ASSISTANT_ENDPOINT_URL="${BRIDGE_URL}/v1/manyforge/assistant"
                MANYFORGE_ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}"
                export MANYFORGE_ASSISTANT_PROVIDER MANYFORGE_ASSISTANT_ENDPOINT_URL MANYFORGE_ASSISTANT_TIMEOUT_S
            fi
            start_composer_dev_container
            ;;
        isaac-sim)
            if [[ "${ASSISTANT}" == "on" ]]; then
                # Mirrors the manyforge-only branch above: the composer-ros
                # compose service reads these env vars (declared in
                # docker-compose.yaml) and Composer's __main__.py falls back
                # to them when no --assistant-* CLI flag is passed. Without
                # this, the ROS Composer would use the deployment YAML's
                # pinned base_url (e.g. :8100 for nemoclaw) even when the
                # launcher selected openclaw and brought up the bridge on
                # :8200, causing a silent endpoint mismatch.
                MANYFORGE_ASSISTANT_PROVIDER="${ASSISTANT_PROVIDER}"
                MANYFORGE_ASSISTANT_ENDPOINT_URL="${BRIDGE_URL}/v1/manyforge/assistant"
                MANYFORGE_ASSISTANT_TIMEOUT_S="${ASSISTANT_TIMEOUT_S}"
                export MANYFORGE_ASSISTANT_PROVIDER MANYFORGE_ASSISTANT_ENDPOINT_URL MANYFORGE_ASSISTANT_TIMEOUT_S
            fi
            build_ros2_control_runtime
            start_isaac_services
            ;;
        real-robot)
            start_real_robot_lane
            ;;
    esac
    # Early supervisor start (assistant lane only). The supervisor probes
    # the assistant stack components and POSTs status rows to
    # /api/runtime/external/assistant_supervisor. It tolerates Composer
    # being unreachable — post_status swallows OSError/URLError and the
    # poll loop continues — so we start it BEFORE wait_for_composer.
    # The GUI receives the latest heartbeat as soon as Composer's API
    # accepts it. Mirrors how isaac_demo_supervisor is launched inside
    # the isaac-topic-control compose service before composer-ros is
    # responsive.
    if [[ "${ASSISTANT}" == "on" ]]; then
        start_assistant_supervisor
    fi
}

step_wait_and_load() {
    _CURRENT_STEP_LABEL="[6/7] wait_for_composer + load_program"
    step "[6/7] Wait for Composer and load program"
    wait_for_composer
    case "${LANE}" in
        isaac-sim)
            # isaac_demo_supervisor (started by the isaac-topic-control compose
            # service when SUPERVISOR_AUTO_LOAD=true) is the single owner of
            # program-load + home-reset for this lane. The supervisor handles
            # the order-tolerant case where Isaac isn't published yet, applies
            # deferInitialRuntimeReset, and gates the command gate. Calling
            # /api/program/load from the launcher here would race the
            # supervisor and could overwrite its deferred-reset semantics.
            info "isaac-sim lane: program/deployment load is owned by isaac_demo_supervisor"
            info "  (program: ${PROGRAM_PATH})"
            info "  (deployment: ${DEPLOYMENT_PATH})"
            ;;
        manyforge-only)
            load_program "${PROGRAM_PATH}" "${DEPLOYMENT_PATH}"
            ;;
        real-robot)
            load_program "${REAL_ROBOT_PROGRAM_PATH}" "${DEPLOYMENT_PATH}"
            ;;
        *)
            fail "Unknown lane for load step: ${LANE}"
            return 1
            ;;
    esac
}

step_bring_up_assistant() {
    if [[ "${ASSISTANT}" != "on" ]]; then
        info "Skipping step 7 — assistant is off."
        return 0
    fi
    _CURRENT_STEP_LABEL="[7/7] bring up assistant"
    step "[7/7] Bring up assistant stack"
    # Supervisor is already running (started in step 5/7 alongside lane
    # infra). It will pick up each new component as we bring them up and
    # post `waiting` → `ready` rows to Composer in real time.
    start_model
    start_vllm_proxy
    configure_nemoclaw
    provision_openclaw_sandbox
    start_bridge
}

print_final_summary() {
    step "Done"
    runtime_status_summary
    cat <<EOF

  Composer UI:   ${COMPOSER_URL}
  Viewer:        native Composer panel
EOF
    if [[ "${ASSISTANT}" == "on" ]]; then
        cat <<EOF
  vLLM:          ${UPSTREAM_BASE_URL}
  Bridge:        ${BRIDGE_URL}

EOF
    else
        echo
    fi
    info "To reproduce non-interactively:"
    local _assistant_args=""
    if [[ "${ASSISTANT}" == "on" ]]; then
        _assistant_args=" --provider ${ASSISTANT_PROVIDER} --platform ${PLATFORM} --model-profile ${MODEL_PROFILE}"
    fi
    info "  scripts/launch.sh --lane ${LANE} --assistant ${ASSISTANT}${_assistant_args} --scenario ${SCENARIO} --non-interactive --yes"
}

# ── Verbs ────────────────────────────────────────────────────────────────────

verb_start() {
    step_choose_lane
    step_choose_assistant
    step_choose_scenario
    review_gate
    step_preflight
    step_bring_up_lane
    step_wait_and_load
    step_bring_up_assistant
    print_final_summary
}

verb_stop() {
    step "Stopping ManyForge launcher-managed services"
    # Stop assistant stack first (if present) so supervisor can clean up.
    stop_assistant_stack
    # Stop both lanes' Composer footprints; idempotent.
    compose_set_lane isaac-sim 2>/dev/null || true
    stop_isaac_services
    stop_composer_dev_container
    stop_real_robot_lane
    pass "Stop complete."
}

verb_status() {
    step "Service status"
    echo "  Containers:" >&2
    docker ps --format '    {{.Names}}\t{{.Image}}\t{{.Ports}}' \
        | grep -E "(manyforge-e2e-|manyforge-real-robot-(owner|composer)|composer-ros|isaac-topic-control|openshell)" >&2 || true
    echo >&2
    local status_ports=("${COMPOSER_PORT:-9000}" 8000)
    if [[ "${START_VLLM_PROXY:-true}" == "true" ]]; then
        status_ports+=("${VLLM_PORT:-8050}")
    fi
    status_ports+=(8100 8200)
    local port_re
    port_re="$(printf '|%s' "${status_ports[@]}")"
    port_re="${port_re:1}"
    echo "  Listeners (${status_ports[*]}):" >&2
    ss -ltnp 2>/dev/null | grep -E ":(${port_re})\\b" >&2 || true
    echo >&2
    echo "  Runtime status:" >&2
    runtime_status_summary
}

verb_restart() {
    verb_stop
    verb_start
}

verb_smoke() {
    verb_start
    step "Smoke test — strict readiness gate"
    # `start` returned green on launcher-managed-service liveness; smoke now
    # blocks on the external dependencies that the assistant_supervisor and
    # isaac_demo_supervisor normally report into the Composer popup
    # (asynchronously). Failure here fails the smoke run; the underlying
    # `start` had already returned green.
    case "${LANE}" in
        isaac-sim)
            wait_for_isaac_joint_states
            wait_for_arm_action
            if [[ "${REQUIRE_GRIPPER_ACTION:-true}" == "true" ]]; then
                wait_for_gripper_action
            fi
            wait_for_arm_controller_state
            if [[ "${ASSISTANT}" == "on" ]]; then
                wait_for_vllm_model
                wait_for_vllm_proxy
                wait_for_bridge
            fi
            if [[ "${SCENARIO}" == "isaac-lite6-pick-place" || "${SCENARIO}" == "isaac-lite6-trocar-foundationpose" ]]; then
                mkdir -p "${LOG_DIR}"
                local lite6_live_probe_artifact="${LOG_DIR}/${SCENARIO}-lite6-live-smoke.json"
                ros_shell "source /opt/ros/jazzy/setup.bash; python3 /workspace/scripts/ros/validate-isaac-lite6-smoke.py --arm-action-name ${ARM_ACTION} --gripper-action-name ${GRIPPER_ACTION} --gripper-transport-kind ${GRIPPER_TRANSPORT_KIND:-ros_gripper_command} --arm-joint-state-topic ${ARM_JOINT_STATE_TOPIC} --isaac-joint-state-topic ${ISAAC_JOINT_STATE_TOPIC} --json" \
                    | tee "${lite6_live_probe_artifact}"
                python3 "${MANYFORGE_ROOT}/scripts/ros/validate-isaac-lite6-smoke-artifact.py" \
                    "${lite6_live_probe_artifact}"
                pass "Lite6 live smoke artifact: ${lite6_live_probe_artifact}"
                if [[ "${SCENARIO}" == "isaac-lite6-trocar-foundationpose" ]]; then
                    local foundationpose_artifact="${LOG_DIR}/${SCENARIO}-foundationpose-detection3d.json"
                    ros_shell "source /opt/ros/jazzy/setup.bash; source /tmp/manyforge-build/isaac-ros-overlay/install/setup.bash 2>/dev/null || true; python3 /workspace/scripts/ros/validate-foundationpose-detection3d-source.py --topic ${FOUNDATIONPOSE_DETECTION3D_TOPIC:-/output} --object-id graspable --selection ${FOUNDATIONPOSE_DETECTION3D_SELECTION:-first} --confidence-min ${FOUNDATIONPOSE_CONFIDENCE_MIN:-0.0} --allow-zero-confidence --max-position-variance-m2 ${FOUNDATIONPOSE_MAX_POSITION_VARIANCE_M2:-0.0025} --allow-zero-covariance --expected-frame-id ${FOUNDATIONPOSE_EXPECTED_FRAME_ID:-link_base} --expected-raw-frame-id ${FOUNDATIONPOSE_EXPECTED_RAW_FRAME_ID:-camera_color_optical_frame} --min-accepted-samples ${FOUNDATIONPOSE_MIN_ACCEPTED_SAMPLES:-1} --timeout-s ${FOUNDATIONPOSE_VALIDATION_TIMEOUT_S:-30}" \
                        | tee "${foundationpose_artifact}"
                    pass "FoundationPose Detection3D artifact: ${foundationpose_artifact}"
                fi
            else
                ros_shell "source /opt/ros/jazzy/setup.bash; python3 /workspace/scripts/ros/smoke_isaac_arm_action.py --action-name ${ARM_ACTION} --joint-state-topic ${ARM_JOINT_STATE_TOPIC}"
            fi
            ;;
        manyforge-only)
            if [[ "${ASSISTANT}" == "on" ]]; then
                local smoke_args=(
                    --deployment-path "${DEPLOYMENT_PATH}"
                    --program-path "${PROGRAM_PATH}"
                    --strict
                    --output "${LOG_DIR}/launcher-smoke.json"
                )
                wait_for_vllm_model
                wait_for_vllm_proxy
                wait_for_bridge
                (cd "${MANYFORGE_ROOT}" && ./scripts/smoke-assistant-e2e.py "${smoke_args[@]}")
            else
                info "No smoke test wired for manyforge-only without assistant; skipping."
            fi
            ;;
        real-robot)
            python3 \
                "${MANYFORGE_ROOT}/scripts/xarm/validate-lite6-product-lane-smoke.py" \
                --composer-url "${COMPOSER_URL}" \
                --expected-cell-id "${REAL_ROBOT_CELL_ID}" \
                --timeout-s "${MANYFORGE_REAL_ROBOT_SMOKE_TIMEOUT_S:-10}"
            ;;
    esac
}

verb_preflight() {
    # Run the wizard up to step 4 only.
    step_choose_lane
    step_choose_assistant
    step_choose_scenario
    review_gate
    step_preflight
    pass "Preflight ok; not bringing services up (preflight verb)."
}

# ── Main ────────────────────────────────────────────────────────────────────

main() {
    parse_args "$@"
    case "${VERB}" in
        start) verb_start ;;
        stop) verb_stop ;;
        status) verb_status ;;
        restart) verb_restart ;;
        smoke) verb_smoke ;;
        preflight) verb_preflight ;;
        *)
            fail "Unknown verb: ${VERB}"
            usage >&2
            exit 2
            ;;
    esac
}

main "$@"
