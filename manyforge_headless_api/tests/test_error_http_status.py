"""The generated code -> HTTP status table answers exactly what the rules
of Headless's former ``_error_status`` answered, for every catalogued code;
the catalog checker (scripts/ci/check-error-catalog.py) keeps every code the
program produces catalogued."""

from __future__ import annotations

import ast
import importlib.util
from pathlib import Path

from manyforge_headless_api.app import _error_status
from manyforge_headless_api.error_http_status import DEFAULT_HTTP_STATUS, HTTP_STATUS

_ROOT = Path(__file__).resolve().parents[2]


def _generator():
    spec = importlib.util.spec_from_file_location(
        "manyforge_error_http_status_generator",
        _ROOT / "manyforge_core/tools/generate_error_catalog.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The reference: Headless's status rules as they were before the table
# (``app._error_status`` and ``component_model.RUN_REFUSAL_CODES``), copied
# unchanged.
_REFERENCE_RUN_REFUSAL_CODES: frozenset[str] = frozenset(
    {
        "cell_latched",
        "component_not_enabled",
        "component_unhealthy",
        "rule_holds",
        "rule_unknown",
        "input_not_fresh",
        "run_in_progress",
        "program_motion_running",
        "manual_operation_in_progress",
        "program_not_executable",
        "execution_host_internal_fault",
        "manager_unavailable",
        "program_scene_preparation_failed",
        "program_cycle_reset_required",
        "program_run_history_full",
        "program_end_effector_tcp_unavailable",
        "program_motion_resume_incomplete",
        "io_manual_write_motion_executing",
    }
)


def _reference_status(code: str) -> int:
    if code == "draft_candidate_response_too_large":
        return 413
    if code == "component_frame_lookup_invalid":
        return 400
    if code in {"component_frame_lookup_stale", "component_reference_missing", "frame_missing"}:
        return 409
    if code == "program_run_started_then_failed":
        return 422
    if code == "full_stack_restart_required":
        return 409
    if code in {
        "headless_projection_response_invalid",
        "headless_projection_response_oversized",
        "manager_command_response_invalid",
        "execution_command_response_invalid",
        "node_environment_projection_invalid",
        "assistant_mode_context_invalid",
        "source_model_context_invalid",
        "source_model_projection_invalid",
        "program_applied_selection_invalid",
        "program_runner_response_invalid",
        "viewer_model_asset_invalid",
        "assistant_mode_projection_invalid",
    }:
        return 502
    if code == "headless_api_internal_failure":
        return 500
    if code.endswith("_not_found") or code in {
        "missing_reference",
        "operator_alarm_not_found",
        "planning_job_unknown",
    }:
        return 404
    if (
        "conflict" in code
        or "stale" in code
        or "fence" in code
        or code.startswith(("program_cycle_state_", "program_run_state_"))
        or code
        in {
            "invalid_state_transition",
            "command_not_allowed",
            "manual_handover_required",
            "object_already_exists",
            "program_run_admission_failed",
            "program_run_already_active",
            "program_applied_non_executable",
            "program_not_selected",
            # Preparation runs before any claim or effect; it refuses the input.
            "activation_preparation_failed",
            "program_reference_validation_failed",
            "prepared_activation_unverified",
            "authoring_not_ready",
            "program_run_draft_applied_mismatch",
            "activation_preparer_busy",
            "model_sources_unknown",
            "program_node_preview_busy",
            "deployment_candidate_cell_mismatch",
            "persistent_key_request_replayed",
            # An operator-alarm condition interlock refuses motion before any effect.
            "operator_alarm_interlock_active",
            "request_result_expired",
            # Component model v2: refusals before dispatch.
            "component_action_in_progress",
            "component_action_unsupported",
            "owner_not_prepared",
            "io_output_controller_owned",
            # A position reset while the simulation isn't playing.
            "simulation_not_playing",
            # An assistant mode read while the Deployment has no assistant.
            "assistant_component_absent",
            # Pick/place planning jobs: a held name, a release before the end.
            "planning_job_exists",
            "planning_job_unfinished",
        }
        # RunReadiness reasons and run refusals.
        or code in _REFERENCE_RUN_REFUSAL_CODES
    ):
        return 409
    if "unavailable" in code or code.endswith("_busy") or code in {
        "assistant_ai_model_absent",
        "headless_api_closed",
        "kernel_owner_unavailable",
        "execution_owner_not_ready",
        "operator_alarm_state_unavailable",
        "program_applied_selection_not_active",
        "program_runner_unavailable",
        "program_run_scheduling_failed",
        "program_cycle_reconciliation_required",
        "request_ledger_full",
        "scene_projection_commit_uncertain",
        # A runtime unit launching or preparing (contract 1.3).
        "runtime_unit_launch_in_progress",
    }:
        return 503
    if (
        code == "invalid_request"
        or code.endswith("_invalid")
        or code.endswith("_rejected")
    ):
        return 400
    if code in {
        # A Scene change whose result would exceed the Scene size limit.
        "scene_too_large",
        "confirmation_required",
        "request_key_required",
        "start_identity_required",
        "operator_alarm_acknowledgement_confirmation_required",
        "operator_alarm_acknowledgement_reason_required",
        "operator_alarm_acknowledge_all_confirmation_required",
        "operator_alarm_acknowledge_all_duplicate_member",
        "operator_alarm_limit_invalid",
        "operator_alarm_state_filter_invalid",
    }:
        return 400
    if code == "operator_alarm_membership_mismatch":
        return 409
    return 500


def test_the_table_names_every_catalogued_code_and_nothing_else() -> None:
    generator = _generator()
    assert set(HTTP_STATUS) == {code for code, _compact, _full in generator.read_catalog()}
    assert DEFAULT_HTTP_STATUS == 500


def test_the_table_answers_the_reference_status_for_every_catalogued_code() -> None:
    differences = {
        code: (status, _reference_status(code))
        for code, status in HTTP_STATUS.items()
        if status != _reference_status(code)
    }
    assert differences == {}


def test_the_rule_spec_is_the_reference_and_agrees_with_headless() -> None:
    generator = _generator()
    assert generator.RUN_REFUSAL_CODES == _REFERENCE_RUN_REFUSAL_CODES
    for code in HTTP_STATUS:
        assert generator.http_status(code) == _reference_status(code) == _error_status(code), code


def test_an_uncatalogued_code_answers_the_default() -> None:
    assert "example_uncatalogued_refused" not in HTTP_STATUS
    assert _error_status("example_uncatalogued_refused") == DEFAULT_HTTP_STATUS == 500


def test_every_code_the_rules_name_is_catalogued() -> None:
    # A code the rules name explicitly kept its old status only if the
    # table has it; the substring rules are covered by the catalog checker.
    tree = ast.parse(
        (_ROOT / "manyforge_core/tools/generate_error_catalog.py").read_text(encoding="utf-8"))
    named = {
        node.value
        for definition in tree.body
        if (isinstance(definition, ast.FunctionDef) and definition.name == "http_status")
        or (isinstance(definition, ast.Assign)
            and getattr(definition.targets[0], "id", None) == "RUN_REFUSAL_CODES")
        for node in ast.walk(definition)
        if isinstance(node, ast.Constant) and isinstance(node.value, str)
        and node.value.count("_") and not node.value.startswith("_")
        and not node.value.endswith("_")
    }
    assert named - set(HTTP_STATUS) == set()
    # The runner's refusal of a second Run keeps base's 409.
    assert HTTP_STATUS["program_run_already_active"] == 409
