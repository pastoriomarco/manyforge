#!/usr/bin/env python3
"""Refuse any error code the program uses that the error catalog lacks.

The catalog is `manyforge_core/resources/error_catalog.tsv`: one line per error,
three tab-separated columns `code`, `compact text`, `full text`. The area is the
code prefix; there is no severity column.

A code is "used" when a snake_case string literal (two or more segments) appears
at one of these error-construction sites in non-test sources:

- C++: the first literal argument of a call (template arguments allowed)
  whose name ends in `Error`, `Failure`, `Refusal`, `Rejection` or
  `Exception`, or is one of the helpers in `_CALLS` or `_CPP_CALLS`; an
  `ErrorDTO{...}` initializer; a `kError*`/`k*ReasonCode` constant; a
  `{"code", ...}` canonical member; a closed `{"reason", "..."}` or
  `{"reason_code", "..."}` pair. Per file: the listed argument of a
  `_MESSAGE_FIRST` helper (`reject<T>("message", "code")`) and a literal
  assigned to a `_REASON_VARS` variable.
- Python: the first literal argument of a call whose name ends in `Error`,
  `Failure`, `Refusal`, `Rejection` or `Exception`, or a `_CALLS` helper with
  an optional leading underscore, or a `_PY_CALLS` name; a `"code": "..."`,
  `"reason_code": "..."` or `"error_code": "..."` member; a `code=`,
  `reason_code=` or `error_code=` keyword or assignment. Per file: the
  `_PY_REASONS` forms and the `_PY_CONSTANTS` constants.
- A `_CODE_VOCABULARIES` entry whose constant or quoted code a non-test,
  non-generated source references.
- TypeScript: a code compared in a branch (`code === '...'`, `code !== '...'`).
- C++, any file: the argument of a function whose string parameter is named
  `code`, `reason_code` or `error_code` (declared in the file or a same-name
  header; for example `Require(cond, "code", "message")`); the literal arms
  of a conditional code argument (`Error(c ? "a" : "b", ...)`); a literal
  returned by a code-returning `..._error` function; `Err({"code", ...})`.
- Python, by syntax: the forms above also as a conditional or an `or`
  default (`"a" if c else "b"`, `reason or "a"`), a `..._CODE` constant or
  `code` class attribute, and a same-file helper or class with a `code`
  parameter.

Codes built at runtime are enumerated in `_RUNTIME_FAMILIES` from the closed
set their variable part ranges over; they count as used while the producer
still contains the template. A few operator-facing codes are produced where no
pattern can see them (a reason assignment, a projection reason, a reason-code
constant); `_INDIRECT_CODES` names each with its producer file, and the code
counts as used while that file still contains it. `--list` prints every used code; `--seed` prints
the missing ones as catalog rows with a humanized compact text and an empty
full text, for a human to complete.
"""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "manyforge_core/resources/error_catalog.tsv"

_CODE = r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+"
_ERROR_NAME = r"[A-Za-z_]\w*(?:Error|Failure|Refusal|Rejection|Exception)"
# Helpers whose literal argument is an error code (C++ and Python).
_CALLS = (
    "fail",
    "invalid",
    "reject",
    "refuse",
    "refused",
    "failure",
    "coordinator_error",
    "stage_error",
    "ErrorResponse",
    "DetailedErrorResponse",
    "ApplyErrorResponse",
    "ApplyKnownNoEffectErrorResponse",
    "ApplyErrorResponseFromJournalEvidence",
    "RetainTerminalFailure",
    "retain_pre_effect_failure",
    "TypedError",
    "RequestInvalid",
)
# Python-only helpers and error classes, matched by exact name (a bare `error`
# would match logger calls).
_PY_CALLS = (
    "_error",
    "_fault_class",
    "_diagnostic",
    "_reason",
    "_trace_unavailable",
    "_record_node_failure",
    "_record_owner_failure",
    "_record_owner_error",
    "mark_malformed",
    "ApplyRefused",
    "ApplyStepFailed",
    "_NodeRefused",
)
# C++-only helpers whose literal argument is an error code, with an optional
# leading underscore.
_CPP_CALLS = (
    "error",
    "Error",
    "err",
    "err_structured",
    "rejected",
    "Reject",
    "Rejected",
    "reject_owned",
    "reject_state",
    "failed",
    "Fail",
    "CoreFail",
    "AlarmFail",
    "StoreFail",
    "HistorianFail",
    "FailNative",
    "fail_physical",
    "failure_profile",
    "unknown",
    "stale",
    "not_sent",
    "Invalid",
    "InvalidConfiguration",
    "invalid_profile",
    "invalid_publication",
    "mark_invalid",
    "ErrorValue",
    "PublishUnavailable",
    "source_unavailable",
    "RefusedBeforeEffect",
    "LatchAndThrow",
    "emit",
    "bb_error",
    "channel_error",
    "digest_input_error",
    "global_rule_error",
    "gripper_runtime_error",
    "projection_error",
    "rule_error",
    "snapshot_error",
    "source_error",
    "state_error",
    "state_view_error",
    "stop_error",
    "stop_acceptance_input_error",
    "switch_error",
    "terminal_input_error",
    "ambiguous_resolution_error",
    "candidate_error",
    "planning_error",
    "readiness_error",
    "resolution_error",
    "route_publication_error",
    "AuthoringNotReady",
    "carrier_frame_unresolved",
    "invalid_value",
    "measured_source_unavailable",
)


# C++ template arguments, one nesting level (`reject<std::unique_ptr<T>>(`).
_TEMPLATE = r"(?:<(?:[^;(){}<>]|<[^;(){}<>]*>)*>\s*)?"


def _call(names: str) -> re.Pattern[str]:
    # The literal may follow C++ template arguments and up to three non-literal
    # arguments (for example `RetainTerminalFailure(journal, intent, "code", ...)`).
    return re.compile(
        r"\b(?:" + names + r")\s*" + _TEMPLATE + r"\(\s*"
        r"(?:[A-Za-z_][\w:.\->*&]*\s*,\s*){0,3}"
        r"(?:std::string\s*\(\s*)?[\"'](" + _CODE + r")[\"']"
    )


_CALL = _call(_ERROR_NAME + r"|_?(?:" + "|".join(_CALLS + ("unavailable",)) + r")")
_PATTERNS = {
    ".cpp": (
        _CALL,
        _call(r"_?(?:" + "|".join(_CPP_CALLS) + r")"),
        re.compile(r"ErrorDTO(?:\s+\w+)?\s*\{\s*(?:std::string\s*\(\s*)?\"(" + _CODE + r")\""),
        re.compile(r"\bk\w*(?:Error|ReasonCode|Code)\w*\s*=\s*\"(" + _CODE + r")\""),
        re.compile(r"\{\s*\"code\"\s*,\s*(?:CanonicalValue\s*\(\s*)?\"(" + _CODE + r")\""),
        # An ErrorDTO braced into a result: `Result<T>::Err({"code", ...})`.
        re.compile(r"\bErr\s*\(\s*\{\s*\"(" + _CODE + r")\""),
        re.compile(r"\bSegmentLimitFailure\s*\{\s*\"(" + _CODE + r")\""),
    ),
    ".py": (
        _CALL,
        _call("|".join(_PY_CALLS)),
        re.compile(r"[\"'](?:reason_|error_)?code[\"']\s*:\s*[\"'](" + _CODE + r")[\"']"),
        re.compile(r"\b(?:reason_|error_)?code\s*=\s*[\"'](" + _CODE + r")[\"']"),
    ),
    ".ts": (
        re.compile(r"\b[cC]ode\s*[!=]==\s*'(" + _CODE + r")'"),
        re.compile(r"'(" + _CODE + r")'\s*[!=]==\s*[\w.?]*[cC]ode\b"),
    ),
}
_PATTERNS[".hpp"] = _PATTERNS[".cpp"]
# C++ `{"reason", "..."}` / `{"reason_code", "..."}` pair in an ErrorDTO or
# detail initializer; the closing brace keeps out field lists such as
# `shape(v, {"reason", "last_accepted_source_sequence", ...})`, and a pair
# that is a later argument closing a call keeps out a two-name list such as
# `closed(fact, {"reason_code", "later_count"})` (a sole argument such as
# `details.insert({"reason_code", ...})` still counts).
_CPP_REASON_MEMBER = re.compile(
    r"(,\s*)?\{\s*\"reason(?:_code)?\"\s*,\s*(?:std::string\s*\(\s*)?\"(" + _CODE + r")\""
    r"\s*\)?\s*\}(\s*\))?"
)
# File -> {helper: zero-based argument index} for local C++ helpers whose code
# is not the first literal argument (for example `reject<T>("message", "code")`
# or `error(code, "message", "reason")`). The argument counts when it is a
# literal or a conditional between two literals.
_RESOURCE_KERNEL = {"reject": 1, "refuse": 1, "error": 2}
_MESSAGE_FIRST = {
    **{
        f"manyforge_core/src/{name}": _RESOURCE_KERNEL
        for name in (
            "resource_addressed_execution_kernel.cpp",
            "resource_addressed_execution_kernel_internal.hpp",
            "resource_addressed_kernel_admission.cpp",
            "resource_addressed_kernel_cell_latch.cpp",
            "resource_addressed_kernel_events.cpp",
            "resource_addressed_kernel_global_io_rules.cpp",
            "resource_addressed_kernel_immediate_step.cpp",
            "resource_addressed_kernel_io_rules.cpp",
            "resource_addressed_kernel_operations.cpp",
            "resource_addressed_kernel_program_blackboard.cpp",
            "resource_addressed_kernel_runtime.cpp",
            "resource_addressed_kernel_status.cpp",
            "resource_addressed_kernel_stop.cpp",
            "resource_addressed_kernel_takeover.cpp",
            "resource_addressed_kernel_takeover_fallback.cpp",
        )
    },
    "manyforge_core/src/resource_addressed_kernel_io_rules.cpp": {
        **_RESOURCE_KERNEL, "rule_error": 1},
    "manyforge_core/src/resource_addressed_kernel_operations.cpp": {
        **_RESOURCE_KERNEL, "mark_failed": 2},
    "manyforge_core/src/collision_validation_coordinator.cpp": {"record_violation": 3},
    "manyforge_core/src/execution_kernel.cpp": {
        "reject": 2,
        "settle_local_failure": 5,
        "finalize_prepared_motion_cancellation_if_requested": 4,
    },
    "manyforge_core/src/kernel_cell_admission_owner.cpp": {"Reject": 1},
    "manyforge_core/src/motion_target_resolution.cpp": {"invalid_target_shape": 1},
    "manyforge_core/src/operation_intent.cpp": {"invalid": 1},
    "manyforge_core/src/ordered_contact_validation.cpp": {"make_release_violation": 3},
    "manyforge_core/src/prepared_motion_evidence_codec.cpp": {"require_digest": 1},
    "manyforge_core/src/prepared_operation_participant_codec.cpp": {"embedded": 1},
    "manyforge_core/src/execution_port_participant.cpp": {"fault": 2},
    "manyforge_core/src/participant_stop_stage_helper.cpp": {"stage_error": 1},
    "manyforge_core/src/parallel_collision_validation_worker_pool.cpp": {"exception_bundle": 1},
    "manyforge_core/src/resource_slot_table.cpp": {
        "reject": 1, "reject_value": 1, "refuse": 1},
    "manyforge_core/src/runtime_contract_manifest_codec.cpp": {
        "reject": 2, "canonical_failure": 1},
    "manyforge_core/src/runtime_participant_binding.cpp": {"invalid": 1},
    "manyforge_core/src/runtime_participant_policy.cpp": {"invalid": 1},
    "manyforge_core/src/runtime_participant_registry.cpp": {"reject": 1},
    "manyforge_collision/src/manyforge_collision_engine.cpp": {"refusal": 1},
    "manyforge_component_manager/src/manager_runtime_convergence_coordinator.cpp": {
        "reject": 2},
    "manyforge_perception_provider/src/provider_model.cpp": {"Finish": 1},
    "manyforge_roboplan/src/cartesian_planning_contract.cpp": {
        "fail": 1, "cartesian_incomplete_error": 1, "set_path_parameterization_error": 1},
    "manyforge_roboplan/src/managed_projection_registry.cpp": {"WorkerFailure": 2},
    "manyforge_roboplan/src/process_contained_candidate_client.cpp": {"error": 2},
    "manyforge_roboplan/src/product_backed_runtime_participant_resolver.cpp": {"Reject": 1},
    "manyforge_roboplan/src/retained_focused_adapters.cpp": {
        "adapter_error": 2, "validation_error": 2},
    "manyforge_roboplan/src/retained_planner_scene_projection.cpp": {
        "projection_error": 1, "invalid": 1, "error": 2},
    "manyforge_roboplan/src/roboplan_candidate_computer.cpp": {"fail": 2, "make_error": 2},
    "manyforge_roboplan/src/roboplan_planner_backend.cpp": {"error": 2},
    "manyforge_roboplan/src/sealed_program_execution_capture.cpp": {"CaptureError": 1},
    "manyforge_xarm_control/src/xarm_arm_adapter.cpp": {"unconfirmed": 1},
}
# File -> C++ reason variables whose literal assignments are codes
# (`result.reason = "..."`). Status reasons (resource_addressed_kernel_status.cpp,
# scene_runtime_state_owner.cpp) are deliberately absent.
_REASON_VARS = {
    "manyforge_core/src/ordered_contact_validation.cpp": (
        "*error_reason", "summary.invalid_reason"),
    "manyforge_core/src/runtime_collision_lookahead_session.cpp": (
        "result.reason", "error_reason", "out.error_reason"),
    "manyforge_roboplan/src/cartesian_planning_contract.cpp": ("reason",),
    "manyforge_xarm_control/src/xarm_driver_services.cpp": ("result.reason",),
}
# File -> Python reason forms that reach an operator, API client or the
# assistant: "assignment" (`reason = "..."`, `reason="..."`) and/or "member"
# (`"reason": "..."`).
_PY_REASONS = {
    "manyforge_assistant_bridge/bridge.py": ("member",),
    "manyforge_behavior/manyforge_behavior/production_ports.py": ("member",),
    "manyforge_behavior/manyforge_behavior/program_run_owner.py": ("assignment",),
    "manyforge_headless_api/control_state.py": ("assignment",),
    "manyforge_operator/isaac.py": ("assignment",),
    "manyforge_program_runner/runtime.py": ("assignment", "member"),
}
_PY_REASON_FORMS = {
    "assignment": re.compile(r"(?<![\w.])reason\s*=\s*[\"'](" + _CODE + r")[\"']"),
    "member": re.compile(r"[\"']reason[\"']\s*:\s*[\"'](" + _CODE + r")[\"']"),
}
# File -> Python upper-case constants that hold codes (a `..._CODE` constant
# needs no entry).
_PY_CONSTANTS = {
    "manyforge_behavior/manyforge_behavior/nodes/interruption.py": (
        "NODE_INTERRUPTED_REASON",),
    "manyforge_program_runner/persistent_keys.py": (
        "REASON_NOT_DECLARED",
        "REASON_DECLARATION_CHANGED",
        "REASON_VALUE_INVALID",
        "FAULT_APPLY_NOT_OBSERVED",
    ),
}
# Producer file -> codes it produces at a site no pattern above can see.
_INDIRECT_CODES = {
    "manyforge_behavior/manyforge_behavior/program_run_owner.py": (
        "selected_motion_stop_settling",
        "selected_owner_assessment_unavailable",
        "selected_owner_recovery_unavailable",
    ),
    "manyforge_behavior/manyforge_behavior/orchestration_values.py": (
        "node_input_unavailable",
        "motion_value_above_program_limit",
    ),
    "manyforge_core/include/manyforge_core/types/carried_object_lost.hpp": (
        "carried_object_lost",
    ),
    # The other arm of a conditional whose first arm a pattern sees.
    "manyforge_behavior/manyforge_behavior/assistant_program_port.py": (
        "model_request_failed",
        "model_response_schema_invalid",
        "model_confidence_below_threshold",
    ),
    "manyforge_core/src/execution_collision_support.cpp": (
        "grasp_contact_before_terminal_window",
    ),
    "manyforge_core/src/resource_addressed_kernel_events.cpp": (
        "motion_admission_closed",
        "runtime_scale_reconcile_unavailable",
    ),
    "manyforge_core/src/resource_addressed_kernel_io_rules.cpp": (
        "motion_interrupt_signal_trip",
    ),
    "manyforge_core/src/resource_addressed_kernel_status.cpp": (
        "operation_recovery_invalid",
    ),
    "manyforge_core/src/sqlite_operation_journal.cpp": (
        "operation_journal_busy",
        "operation_journal_start_invalid",
    ),
    "manyforge_core/src/prepared_operation_participant_codec.cpp": (
        "program_run_id_invalid",
    ),
    "manyforge_headless_api/identity.py": ("start_identity_required",),
    "manyforge_deployment/src/io_signal_catalog.cpp": (
        "ai_model_configuration_invalid",
    ),
    "manyforge_deployment/src/program_reference_validator.cpp": (
        "collision_body_missing",
    ),
    "manyforge_headless/src/sqlite_event_history_store.cpp": (
        "historian_storage_busy",
    ),
    "manyforge_headless_api/control_state.py": (
        "source_recovered",
        "control_projection_source_unavailable",
    ),
    "manyforge_roboplan/src/geometric_contact_input_provider.cpp": (
        "contact_bodies_static",
        "mesh_depth_vertex_cap_reached",
        "mesh_not_closed",
        "active_tcp_contact_bodies_unavailable",
    ),
    "manyforge_roboplan/src/retained_gripper_route_publication_owner.cpp": (
        "route_predecessor_gripper_rollback_failed",
    ),
    "manyforge_roboplan/src/retained_kinematic_state_projector.cpp": (
        "retained_state_projection_failed",
    ),
    "manyforge_roboplan/src/retained_ros2_control_runtime_handle.cpp": (
        "ros2_control_hardware_state_timeout",
    ),
    "manyforge_roboplan/src/ros_boolean_output_provider.cpp": (
        "io_output_acceptance_unknown",
    ),
    "manyforge_scene/src/logical_scene_owner_store.cpp": (
        "active_bundle_publication_not_current",
    ),
    "manyforge_headless_api/component_overview.py": (
        "applied_program_unavailable",
        "execution_target_unavailable",
        "controlled_stop_capability_unavailable",
        "admission_unavailable",
        "alarm_owner_unavailable",
        "component_model_unavailable",
        "owner_response_rejected",
        "source_model_unavailable",
        "stop_state_unavailable",
        "source_stale",
    ),
    "manyforge_core/src/execution_kernel.cpp": (
        "collision_resource_candidate_snapshot_unavailable",
        "relative_start_state_unavailable",
        "goal_frame_start_state_unavailable",
        "retained_runtime_collision_bundle_invalid",
        "retained_runtime_collision_inventory_invalid",
        "runtime_collision_lookahead_resource_conflict",
        "execution_progress_generation_unavailable",
        "runtime_lookahead_trajectory_unavailable",
        "runtime_lookahead_projection_revision_unavailable",
        "snapshot_unavailable",
    ),
    "manyforge_core/src/participant_stop_stage_helper.cpp": (
        "controlled_hard_stop_brake_invalid",
        "controlled_hard_stop_elapsed_invalid",
        "controlled_hard_stop_limits_unavailable",
        "controlled_hard_stop_rejected",
        "controlled_hard_stop_not_installed",
    ),
    "manyforge_component_manager/src/runtime_observation.cpp": (
        "critical_progress_stale",
        "process_presence_stale",
        "source_report_stale",
    ),
    "manyforge_core/src/prepared_execution.cpp": (
        "execution_backend_status_error_invalid",
    ),
    "manyforge_composer/backend/execution_observation_stream.py": (
        "execution_observation_projection_invalid",
    ),
    "manyforge_deployment/src/retained_io_policy_compiler.cpp": (
        "global_interrupt_group_invalid",
        "global_interrupt_key_type_conflict",
        "global_interrupt_scope_invalid",
        "retained_global_rule_invalid",
    ),
    "manyforge_deployment/src/activation_pose_source_internal.cpp": (
        "ai_model_api_key_env_invalid",
        "scene_pose_source_relative_reference_gripper_link_invalid",
        "scene_pose_source_relative_references_conflict",
    ),
    "manyforge_ros2_control_execution/src/ros2_gripper_command_backend.cpp": (
        "gripper_command_subscriber_unavailable",
        "gripper_state_feedback_stale",
        "gripper_state_feedback_unavailable",
        "gripper_state_publisher_unavailable",
        "gripper_transport_kind_invalid",
        "ros_gripper_command_goal_rejected",
        "scene_projection_unavailable",
        "scene_projection_worker_unavailable",
    ),
    "manyforge_headless_api/cli.py": (
        "headless_cli_invalid",
    ),
    "manyforge_program_runner/cli.py": (
        "program_runner_configuration_invalid",
    ),
    "manyforge_core/src/io_rule_outcome_codec.cpp": (
        "io_rule_outcome_invalid",
    ),
    "manyforge_headless/src/node_environment_projection.cpp": (
        "manager_runtime_unavailable",
    ),
    "manyforge_headless_api/control_dispatch.py": (
        "manual_handover_required",
        "manual_operation_scope_stale",
    ),
    "manyforge_core/include/manyforge_core/types/operator_alarm_attention.hpp": (
        "operator_alarm_state_unavailable",
    ),
    "manyforge_core/include/manyforge_core/prepared_execution_membership.hpp": (
        "prepared_contract_conflict",
        "prepared_replacement_conflict",
        "prepared_replacement_stale",
    ),
    "manyforge_program_runner/runtime.py": (
        "program_execution_owner_unavailable",
        "program_runtime_owner_unavailable",
    ),
    "manyforge_behavior/manyforge_behavior/nodes/v2_owner.py": (
        "program_node_io_source_unavailable",
    ),
    "manyforge_roboplan/src/roboplan_cartesian_trajectory_dynamics.cpp": (
        "cartesian_trajectory_interpolant_joint_mapping_invalid",
    ),
    "manyforge_core/src/runtime_collision_query_session.cpp": (
        "runtime_contact_release_policy_invalid",
        "runtime_query_snapshot_invalid",
    ),
    "manyforge_core/src/soft_on_path_stop_planner.cpp": (
        "runtime_scale_bounds_invalid",
        "soft_on_path_interpolation_invalid",
    ),
    "manyforge_core/include/manyforge_core/ports/iexecution_state_sink.hpp": (
        "scene_state_unavailable",
    ),
    "manyforge_core/include/manyforge_core/types/program_execution_control.hpp": (
        "selection_release_unavailable",
    ),
    "manyforge_ros2_control_execution/src/ros2_control_capability.cpp": (
        "soft_on_path_capability_config_invalid",
    ),
    "manyforge_core/src/trajectory_splice.cpp": (
        "trajectory_splice_milestones_invalid",
    ),
    "manyforge_headless/src/program_link.cpp": (
        "program_runner_socket_unavailable",
    ),
    "manyforge_roboplan/src/ros_managed_runtime_host.cpp": (
        "program_reset_position_unavailable",
    ),
}
# Reason-code vocabularies: a listed code counts as used only while a non-test
# source references its constant (`reason_codes::kName`, `ReasonCode.NAME`) or
# quotes the code itself outside the vocabularies.
# Closed sets that runtime-built codes vary over, each copied from its source
# (tests/test_error_catalog.py checks the enum-backed ones still match).
_PROGRAM_CYCLE_STATES = (  # manyforge_program_runner/cycle_journal.py STATES
    "idle_after_reset", "starting", "running", "resetting",
    "terminal_awaiting_reset", "recovery_required")
_PROGRAM_RUN_STATES = (  # program_run_owner.ProgramRunState
    "running", "intervention_required", "halting", "halted", "stopping",
    "finalizing", "stopped", "succeeded", "failed")
_INTERRUPTED_EXECUTION_STATES = (  # orchestration_ports.INTERRUPTED_EXECUTION_STATES
    "stopped", "interrupted", "cancelled_pre_dispatch")
_FAILED_EXECUTION_STATES = ("failed", "intervention_required")  # orchestration_ports
_PREEMPTION_OUTCOMES = ("preempted", "superseded")  # v2_owner._PREEMPTION_OUTCOMES
_STOP_STAGE_MODES = ("soft_on_path", "spring_back")  # participant_stop_stage_helper `mode`
# ComponentOverviewFacts capture names (service.py) and the shell's required
# sections (control_state.py).
_OVERVIEW_CAPTURES = (
    "runtime", "alarms", "authoring_admission", "program_cycle", "applied_program",
    "applied_program_status", "source_model", "execution_identity",
    "execution_targets", "execution_control")
_SHELL_SECTIONS = ("runtime", "alarms", "authoring_admission", "program_cycle")
# Producer file -> {the template it builds a code from, exactly as written:
# every code it builds}. The codes count as used while the producer still
# contains the template.
_RUNTIME_FAMILIES = {
    "manyforge_program_runner/runtime.py": {
        'f"program_cycle_state_{cycle.state}"': tuple(
            f"program_cycle_state_{state}" for state in _PROGRAM_CYCLE_STATES),
        "f\"program_run_state_{run_state or 'unavailable'}\"": tuple(
            f"program_run_state_{state}" for state in (*_PROGRAM_RUN_STATES, "unavailable")),
    },
    "manyforge_headless_api/service.py": {
        'f"{name}_projection_invalid"': tuple(
            f"{name}_projection_invalid" for name in _OVERVIEW_CAPTURES),
        'f"{name}_source_unavailable"': ("manager_source_unavailable",),
    },
    "manyforge_headless_api/control_state.py": {
        'f"{missing}_projection_unavailable"': tuple(
            f"{name}_projection_unavailable" for name in _SHELL_SECTIONS),
    },
    "manyforge_headless_api/component_overview.py": {
        'f"{tile_id}_unavailable"': (
            "observe_render_unavailable", "plan_preview_unavailable",
            "execute_control_unavailable"),
    },
    "manyforge_behavior/manyforge_behavior/interrupt_outcome.py": {
        'f"motion_{outcome}_mapped_failure"': (
            "motion_signal_trip_mapped_failure", "motion_no_signal_trip_mapped_failure"),
    },
    "manyforge_behavior/manyforge_behavior/production_ports.py": {
        'f"operation_{state}"': tuple(
            f"operation_{state}" for state in _INTERRUPTED_EXECUTION_STATES),
        "f\"operation_{operation['state']}\"": tuple(
            f"operation_{state}"
            for state in (*_INTERRUPTED_EXECUTION_STATES, *_FAILED_EXECUTION_STATES)),
    },
    "manyforge_behavior/manyforge_behavior/nodes/v2_owner.py": {
        'f"motion_{self._outcome}"': tuple(
            f"motion_{outcome}" for outcome in _PREEMPTION_OUTCOMES),
    },
    "manyforge_core/src/participant_stop_stage_helper.cpp": {
        f'mode + "_{suffix}"': tuple(f"{mode}_{suffix}" for mode in _STOP_STAGE_MODES)
        for suffix in (
            "join_failed", "stop_delta_budget_exceeded", "path_ramp_dispatch_failed",
            "path_ramp_dispatch_uncertain")
    },
    "manyforge_composer/backend/routes_pointcloud.py": {
        'f"pointcloud_{disposition}"': tuple(f"pointcloud_{disposition}" for disposition in (
            "source_owner_mismatch", "scene_owner_unavailable", "cell_identity_mismatch",
            "stream_generation_regressed", "sequence_regressed", "sequence_conflict")),
    },
    "manyforge_composer/backend/routes_deployment_draft.py": {
        'f"manager_apply_{terminal}"': ("manager_apply_failed", "manager_apply_unknown_effect"),
    },
}
_CODE_VOCABULARIES = {
    "manyforge_core/include/manyforge_core/types/reason_codes.hpp": re.compile(
        r"\b(k\w+)\s*\[\]\s*=\s*\"(" + _CODE + r")\""),
    "manyforge_behavior/manyforge_behavior/reason_codes.py": re.compile(
        r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*\(?\s*\"(" + _CODE + r")\"", re.M),
}
# Files generated from the catalog: they quote every code, so they never prove
# a vocabulary entry used.
_GENERATED = (
    "manyforge_core/include/manyforge_core/types/error_catalog.generated.hpp",
    "manyforge_headless_api/error_codes.py",
    "manyforge_composer/frontend/src/api/errorCatalog.generated.ts",
)
_VOCABULARY_REFERENCE = re.compile(r"\b(?:reason_codes::|ReasonCode\.)(\w+)")
_QUOTED = re.compile(r"[\"'](" + _CODE + r")[\"']")
_PATTERNS[".tsx"] = _PATTERNS[".ts"]

_SKIP_PARTS = {
    "node_modules",
    "build",
    "install",
    "log",
    "third_party",
    "test",
    "tests",
    "process_tests",
    "__pycache__",
    ".manyforge-dev",
    "qualification",
    "verification",
    "examples",
}


def _is_test(path: Path) -> bool:
    name = path.name
    return (
        name.startswith("test_")
        or name.endswith(("_test.py", ".test.ts", ".test.tsx", "_test.cpp"))
        or name == "conftest.py"
    )


def source_files(root: Path = ROOT):
    for suffix in _PATTERNS:
        for path in root.rglob(f"*{suffix}"):
            relative = path.relative_to(root)
            if _SKIP_PARTS.intersection(relative.parts) or _is_test(path):
                continue
            if relative.parts[0] in {"scripts", "docs", "tools"}:
                continue
            yield path


def _arguments(text: str, start: int) -> list[str]:
    """The top-level arguments of the call whose `(` ends just before `start`."""
    arguments: list[str] = []
    depth = 0
    begin = index = start
    while index < len(text):
        character = text[index]
        if character in "\"'":
            index += 1
            while index < len(text) and text[index] != character:
                index += 2 if text[index] == "\\" else 1
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                arguments.append(text[begin:index])
                return arguments
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append(text[begin:index])
            begin = index + 1
        elif character == ";":
            break
        index += 1
    return arguments


_LITERAL = r"(?:std::string\s*\(\s*)?\"(" + _CODE + r")\"\s*\)?"
_WRAPPED = re.compile(r"\s*(?:std::string\s*)?\((.*)\)\s*", re.S)


def _conditional_parts(argument: str) -> tuple[list[str], list[str]]:
    """`argument` split at its top-level `?` and `:` (never `::`)."""
    parts: list[str] = []
    separators: list[str] = []
    depth = 0
    begin = index = 0
    while index < len(argument):
        character = argument[index]
        if character in "\"'":
            index += 1
            while index < len(argument) and argument[index] != character:
                index += 2 if argument[index] == "\\" else 1
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == ":" and argument[index + 1:index + 2] == ":":
            index += 1
        elif depth == 0 and character in "?:":
            parts.append(argument[begin:index])
            separators.append(character)
            begin = index + 1
        index += 1
    parts.append(argument[begin:])
    return parts, separators


def _argument_literals(argument: str):
    """The codes a C++ argument evaluates to: a literal, or each literal arm
    of a conditional (`c ? "a" : "b"`, chains, parentheses; a non-literal arm
    and every condition are skipped)."""
    literal = re.fullmatch(r"\s*" + _LITERAL + r"\s*", argument)
    if literal:
        yield literal.group(1)
        return
    parts, separators = _conditional_parts(argument)
    if not separators:
        wrapped = _WRAPPED.fullmatch(argument)
        if wrapped:
            yield from _argument_literals(wrapped.group(1))
        return
    for position, part in enumerate(parts[1:], 1):
        before = separators[position - 1]
        after = separators[position] if position < len(separators) else None
        if before == "?" or after != "?":
            yield from _argument_literals(part)


def _message_first_codes(text: str, helpers: dict[str, int]):
    for helper, position in helpers.items():
        call = re.compile(r"\b" + helper + r"\s*" + _TEMPLATE + r"\(")
        for match in call.finditer(text):
            arguments = _arguments(text, match.end())
            if position < len(arguments):
                yield from _argument_literals(arguments[position])


# A C++ function declaration or definition: its name and parameter list.
_CPP_FUNCTION = re.compile(
    r"\b([A-Za-z_]\w*)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?[{;]"
)
_CPP_CODE_PARAMETER = re.compile(
    r"(?:std::string(?:_view)?|char\s*\*)\s*&?\s*(?:code|reason_code|error_code)\s*$")
_CPP_KEYWORDS = {"if", "while", "for", "switch", "return", "catch", "sizeof"}


def _cpp_code_helpers(text: str) -> dict[str, int]:
    """Functions with a string parameter named `code`, `reason_code` or
    `error_code`: name -> that parameter's zero-based position."""
    helpers: dict[str, int] = {}
    for match in _CPP_FUNCTION.finditer(text):
        if match.group(1) in _CPP_KEYWORDS:
            continue
        for position, parameter in enumerate(_arguments(match.group(2) + ")", 0)):
            if _CPP_CODE_PARAMETER.search(parameter.split("=")[0].strip()):
                helpers.setdefault(match.group(1), position)
                break
    return helpers


# A C++ function named `..._error` that returns a code (`std::optional<
# std::string_view>`, `std::string`, ...): every literal it returns is one.
_CPP_CODE_RETURNING = re.compile(
    r"(?:std::)?(?:optional<\s*(?:std::)?string(?:_view)?\s*>|string_view|string)"
    r"\s+\w*_error\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{"
)
_CPP_RETURNED_LITERAL = re.compile(
    r"\breturn\s+(?:std::(?:optional<[^>]*>|string(?:_view)?)\s*\(\s*)?\"(" + _CODE + r")\"")


def _cpp_returned_codes(text: str):
    for match in _CPP_CODE_RETURNING.finditer(text):
        depth, end = 0, len(text)
        for index in range(match.end() - 1, len(text)):
            depth += {"{": 1, "}": -1}.get(text[index], 0)
            if depth == 0:
                end = index
                break
        yield from _CPP_RETURNED_LITERAL.findall(text, match.end(), end)


_CPP_CALL_NAMES = re.compile(
    r"\b(?:" + _ERROR_NAME + r"|_?(?:"
    + "|".join(_CALLS + ("unavailable",) + _CPP_CALLS) + r"))\s*" + _TEMPLATE + r"\("
)
_CPP_LEADING_NAME = re.compile(r"\s*[A-Za-z_][\w:.\->*&]*\s*")


def _cpp_conditional_codes(text: str):
    """The literal arms of a conditional that is the code argument (the
    first argument after up to three names) of an error-construction call."""
    for match in _CPP_CALL_NAMES.finditer(text):
        arguments = _arguments(text, match.end())
        for argument in arguments[:4]:
            if _CPP_LEADING_NAME.fullmatch(argument):
                continue
            if _conditional_parts(argument)[1]:
                yield from _argument_literals(argument)
            break


_PY_CODE_NAMES = frozenset({"code", "reason_code", "error_code"})
# An upper-case constant named `..._CODE` holds a code; `LOG_CODE` is a log tag.
_PY_CODE_CONSTANT = re.compile(r"(?!LOG_CODE$)_?[A-Z][A-Z0-9_]*_CODE")
_PY_ERROR_CALL = re.compile(
    r"(?:" + _ERROR_NAME + r"|_?(?:" + "|".join(_CALLS) + r")|"
    + "|".join(_PY_CALLS) + r")"
)


def _py_literals(node: ast.AST):
    """The codes a Python expression evaluates to: a literal, each literal arm
    of a conditional, each literal of an `or` chain (a default)."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        if re.fullmatch(_CODE, node.value):
            yield node.value
    elif isinstance(node, ast.IfExp):
        yield from _py_literals(node.body)
        yield from _py_literals(node.orelse)
    elif isinstance(node, ast.BoolOp) and isinstance(node.op, ast.Or):
        for value in node.values:
            yield from _py_literals(value)


def _py_name(node: ast.AST) -> str | None:
    if isinstance(node, ast.Name):
        return node.id
    return node.attr if isinstance(node, ast.Attribute) else None


def _py_code_helpers(tree: ast.AST) -> dict[str, int]:
    """Functions (and classes by their `__init__`) with a parameter named
    `code`, `reason_code` or `error_code`: name -> its positional index."""
    helpers: dict[str, int] = {}
    for node in ast.walk(tree):
        function, name = node, getattr(node, "name", None)
        if isinstance(node, ast.ClassDef):
            function = next((item for item in node.body if isinstance(
                item, ast.FunctionDef) and item.name == "__init__"), None)
        if not isinstance(function, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        parameters = [a.arg for a in function.args.posonlyargs + function.args.args]
        if parameters[:1] in (["self"], ["cls"]):
            parameters = parameters[1:]
        for position, parameter in enumerate(parameters):
            if parameter in _PY_CODE_NAMES:
                helpers.setdefault(name, position)
                break
    return helpers


def _py_ast_codes(relative: str, text: str):
    """Python code sites by syntax: the forms of `_PATTERNS[".py"]` also with
    a conditional or an `or` default, a `code` class attribute, and a call of
    a same-file helper or class with a `code` parameter."""
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return
    names = set(_PY_CODE_NAMES)
    members = set(_PY_CODE_NAMES)
    forms = _PY_REASONS.get(relative, ())
    if "assignment" in forms:
        names.add("reason")
    if "member" in forms:
        members.add("reason")
    helpers = _py_code_helpers(tree)
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            name = _py_name(node.func)
            if name in helpers and helpers[name] < len(node.args):
                yield from _py_literals(node.args[helpers[name]])
            if name and _PY_ERROR_CALL.fullmatch(name):
                for argument in node.args[:4]:
                    if not isinstance(argument, (ast.Name, ast.Attribute)):
                        yield from _py_literals(argument)
                        break
            for keyword in node.keywords:
                if keyword.arg in names:
                    yield from _py_literals(keyword.value)
        elif isinstance(node, ast.Dict):
            for key, value in zip(node.keys, node.values):
                if isinstance(key, ast.Constant) and key.value in members:
                    yield from _py_literals(value)
        elif isinstance(node, (ast.Assign, ast.AnnAssign)) and node.value is not None:
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            if any(isinstance(target, ast.Name) and (
                    target.id in names or _PY_CODE_CONSTANT.fullmatch(target.id))
                   for target in targets):
                yield from _py_literals(node.value)


def _file_codes(relative: str, suffix: str, text: str,
                header_helpers: dict[str, int] | None = None):
    """Codes at the error-construction sites of one non-test source file;
    `header_helpers` are the code helpers its same-name headers declare."""
    for pattern in _PATTERNS[suffix]:
        for match in pattern.finditer(text):
            yield match.group(1)
    if suffix in {".cpp", ".hpp"}:
        for match in _CPP_REASON_MEMBER.finditer(text):
            if not (match.group(1) and match.group(3)):
                yield match.group(2)
        yield from _message_first_codes(text, _MESSAGE_FIRST.get(relative, {}))
        yield from _message_first_codes(
            text, {**(header_helpers or {}), **_cpp_code_helpers(text)})
        yield from _cpp_conditional_codes(text)
        yield from _cpp_returned_codes(text)
        for name in _REASON_VARS.get(relative, ()):
            assignment = re.compile(r"(?<![\w.>])" + re.escape(name) + r"\s*=(?!=)([^;]*);")
            for match in assignment.finditer(text):
                yield from _argument_literals(match.group(1))
    if suffix == ".py":
        for form in _PY_REASONS.get(relative, ()):
            yield from (match.group(1) for match in _PY_REASON_FORMS[form].finditer(text))
        for name in _PY_CONSTANTS.get(relative, ()):
            constant = re.compile(
                r"^\s*" + name + r"\s*(?::[^=\n]+)?=\s*[\"'](" + _CODE + r")[\"']", re.M)
            yield from (match.group(1) for match in constant.finditer(text))
        yield from _py_ast_codes(relative, text)


def used_codes(root: Path = ROOT) -> dict[str, str]:
    """Every used code with the first file that uses it."""
    found: dict[str, str] = {}
    quoted: set[str] = set()
    references: set[str] = set()
    paths = sorted(source_files(root))
    header_helpers: dict[str, dict[str, int]] = {}
    for path in paths:
        if path.suffix == ".hpp":
            header_helpers.setdefault(path.stem, {}).update(
                _cpp_code_helpers(path.read_text(encoding="utf-8", errors="replace")))
    for path in paths:
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for code in _file_codes(relative, path.suffix, text, header_helpers.get(path.stem)):
            found.setdefault(code, relative)
        if relative not in _CODE_VOCABULARIES and relative not in _GENERATED:
            quoted.update(_QUOTED.findall(text))
            references.update(_VOCABULARY_REFERENCE.findall(text))
    for relative, codes in _INDIRECT_CODES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for code in codes:
            if f'"{code}"' in text:
                found.setdefault(code, relative)
    for relative, families in _RUNTIME_FAMILIES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for template, codes in families.items():
            if template in text:
                for code in codes:
                    found.setdefault(code, relative)
    for relative, entry in _CODE_VOCABULARIES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, code in entry.findall(text):
            if name in references or code in quoted:
                found.setdefault(code, relative)
    return found


def catalog_codes(path: Path = CATALOG) -> dict[str, tuple[str, str]]:
    rows: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) != 3 or not re.fullmatch(_CODE, columns[0]) or not columns[1]:
            raise SystemExit(f"{path}:{number}: expected code<TAB>compact<TAB>full")
        if columns[0] in rows:
            raise SystemExit(f"{path}:{number}: duplicate code {columns[0]}")
        rows[columns[0]] = (columns[1], columns[2])
    return rows


def humanized(code: str) -> str:
    text = code.replace("_", " ")
    return text[:1].upper() + text[1:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list", action="store_true", help="print every used code")
    mode.add_argument("--seed", action="store_true", help="print missing codes as rows")
    args = parser.parse_args()
    used = used_codes()
    if args.list:
        for code in sorted(used):
            print(f"{code}\t{used[code]}")
        return 0
    catalog = catalog_codes()
    missing = sorted(set(used) - set(catalog))
    if args.seed:
        for code in missing:
            print(f"{code}\t{humanized(code)}\t")
        return 0
    if missing:
        for code in missing:
            print(f"error code {code!r} (used in {used[code]}) is missing from "
                  f"{CATALOG.relative_to(ROOT)}", file=sys.stderr)
        return 1
    print(f"error catalog covers all {len(used)} used codes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
