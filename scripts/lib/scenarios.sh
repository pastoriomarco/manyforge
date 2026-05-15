# shellcheck shell=bash
# scripts/lib/scenarios.sh — named (lane, assistant, program, deployment) bundles.
#
# A scenario is a four-tuple: lane × assistant × program path × deployment path.
# The launcher exposes `--scenario NAME` as shorthand, or `--program / --deployment`
# for free-form invocations.
#
# Update this table when new program/deployment pairs are added under examples/.

if [[ -n "${_MANYFORGE_LIB_SCENARIOS:-}" ]]; then return 0; fi
_MANYFORGE_LIB_SCENARIOS=1

source "$(dirname "${BASH_SOURCE[0]}")/runner.sh"

# Each line: name|lane|assistant|program|deployment|description
_SCENARIO_TABLE=(
    "ur5-pick-place|manyforge-only|off|/workspace/examples/pick_and_place.program.yaml|/workspace/examples/pick_and_place.deployment.yaml|UR5 pick-and-place baseline (12-step)"
    "ur5-simple-pick-place|manyforge-only|off|/workspace/manyforge_behavior/tests/fixtures/pick_and_place.program.yaml|/workspace/examples/pick_and_place.deployment.yaml|UR5 3-step minimal test fixture (was demo.sh --simple)"
    "ur5-interrupt-pick-place|manyforge-only|off|/workspace/examples/interrupt_pick_and_place.program.yaml|/workspace/examples/interrupt_pick_and_place.deployment.yaml|UR5 pick-and-place with interrupt + resume (pause/resume demo)"
    "ur10e-pick-place|manyforge-only|off|/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml|/workspace/examples/pick_and_place_ur10e_robotiq.deployment.yaml|UR10e + Robotiq 2F-140 self-contained preview"
    "ur5-semantic-shape-pick|manyforge-only|on|/workspace/examples/semantic_shape_pick.program.yaml|/workspace/examples/semantic_shape_pick.deployment.yaml|UR5 semantic shape pick (vision LLM)"
    "ur5-vision-pose-pick|manyforge-only|on|/workspace/examples/vision_guided_pose_pick.program.yaml|/workspace/examples/vision_guided_pose_pick.deployment.yaml|UR5 vision-guided pose pick (vision LLM)"
    "ur10e-scene-authoring|manyforge-only|on|/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml|/workspace/examples/assistant_modes_scene_authoring.deployment.yaml|UR10e + Robotiq with full Cosmos assistant + scene authoring"
    "isaac-ur10e-light-scene|isaac-sim|off|/workspace/examples/isaac_manipulator_scene_light.program.yaml|/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml|Isaac commanded sim with UR10e light manipulator scene"
    "isaac-ur10e-light-scene-assistant|isaac-sim|on|/workspace/examples/isaac_manipulator_scene_light.program.yaml|/workspace/examples/isaac_manipulator_scene_light_assistant.deployment.yaml|Isaac commanded sim with UR10e light manipulator scene + full Cosmos assistant overlay"
)

# _SCENARIO_ALIASES — backward-compat for the old short names that the
# v1 launcher shipped with. Old runbooks keep working; the user sees a
# one-time deprecation warning per launch invocation pointing at the
# canonical name.
#
# Format: old_name|canonical_name
_SCENARIO_ALIASES=(
    "pick-place|ur5-pick-place"
    "interrupt|ur5-interrupt-pick-place"
    "ur10e|ur10e-pick-place"
    "shape-pick|ur5-semantic-shape-pick"
    "vision-pose|ur5-vision-pose-pick"
    "assistant-authoring|ur10e-scene-authoring"
    "isaac-light|isaac-ur10e-light-scene"
    "isaac-assistant|isaac-ur10e-light-scene-assistant"
)

# scenario_canonical NAME — resolve alias to canonical; echo canonical
# (or the input unchanged if not an alias). Emits a one-time deprecation
# warning per launch invocation via the _MANYFORGE_ALIAS_WARNED sentinel.
scenario_canonical() {
    local name="$1"
    local row alias canonical
    for row in "${_SCENARIO_ALIASES[@]}"; do
        alias="${row%%|*}"
        canonical="${row#*|}"
        if [[ "${alias}" == "${name}" ]]; then
            if [[ -z "${_MANYFORGE_ALIAS_WARNED:-}" ]]; then
                warn "scenario name '${alias}' is deprecated; use '${canonical}' (this is the only warning this run)"
                export _MANYFORGE_ALIAS_WARNED=1
            fi
            printf '%s\n' "${canonical}"
            return 0
        fi
    done
    printf '%s\n' "${name}"
}

# scenario_lookup NAME  — echoes the matching pipe-delimited row to stdout.
# Returns 1 if no match.
scenario_lookup() {
    local name="$1"
    # Resolve any deprecated alias to its canonical name first; this emits
    # a one-time deprecation warning per launch invocation via
    # scenario_canonical's _MANYFORGE_ALIAS_WARNED sentinel.
    name="$(scenario_canonical "${name}")"
    local row
    for row in "${_SCENARIO_TABLE[@]}"; do
        if [[ "${row%%|*}" == "${name}" ]]; then
            printf '%s\n' "${row}"
            return 0
        fi
    done
    return 1
}

scenario_field() {
    local row="$1"
    local idx="$2"   # 1=name, 2=lane, 3=assistant, 4=program, 5=deployment, 6=description
    printf '%s\n' "${row}" | awk -F'|' -v i="${idx}" '{print $i}'
}

# scenarios_for LANE ASSISTANT  — list scenario names compatible with the
# selected lane + assistant axis. Use ASSISTANT="any" to skip the filter.
scenarios_for() {
    local lane="$1"
    local assistant="$2"
    local row name row_lane row_assistant
    for row in "${_SCENARIO_TABLE[@]}"; do
        name="$(scenario_field "${row}" 1)"
        row_lane="$(scenario_field "${row}" 2)"
        row_assistant="$(scenario_field "${row}" 3)"
        if [[ "${row_lane}" != "${lane}" ]]; then
            continue
        fi
        if [[ "${assistant}" != "any" && "${row_assistant}" != "${assistant}" ]]; then
            continue
        fi
        printf '%s\n' "${name}"
    done
}

# scenario_describe NAME  — pretty-print to stderr.
scenario_describe() {
    local row="$1"
    info "  name:        $(scenario_field "${row}" 1)"
    info "  lane:        $(scenario_field "${row}" 2)"
    info "  assistant:   $(scenario_field "${row}" 3)"
    info "  program:     $(scenario_field "${row}" 4)"
    info "  deployment:  $(scenario_field "${row}" 5)"
    info "  notes:       $(scenario_field "${row}" 6)"
}

scenarios_print_table() {
    printf '\n  %-36s %-15s %-11s %s\n' "scenario" "lane" "assistant" "description" >&2
    printf '  %-36s %-15s %-11s %s\n' "--------" "----" "---------" "-----------" >&2
    local row name lane assistant description
    for row in "${_SCENARIO_TABLE[@]}"; do
        name="$(scenario_field "${row}" 1)"
        lane="$(scenario_field "${row}" 2)"
        assistant="$(scenario_field "${row}" 3)"
        description="$(scenario_field "${row}" 6)"
        printf '  %-36s %-15s %-11s %s\n' "${name}" "${lane}" "${assistant}" "${description}" >&2
    done
    printf '\n' >&2
}
