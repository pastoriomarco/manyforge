"""Per-node interrupt rules: catalog fields, condition validation, key union,
node outcome keys and the Runner's typed-result mapping (schema02/03)."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.interrupt_condition import (
    InterruptConditionError,
    check_condition_structure,
    condition_keys,
)
from manyforge_behavior.interrupt_outcome import (
    contained_stage_from_stop_terminal,
    contained_stages_from_stop_terminal,
    map_interrupt_result,
)
from manyforge_behavior.orchestration_values import ProgramValueStore
from manyforge_behavior.program import (
    INTERRUPT_KEY_INITIAL_REQUIRED,
    NODE_OUTCOME_VALUES,
    Program,
    ProgramValidationError,
    require_global_rule_keys,
    require_global_rule_operands,
    require_interrupt_key_initials,
)

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_NODES = ("motion.move_until_signal", "motion.probe_search")
_SIGNAL = {"signal": {"target_role": "arm", "resource_role": "part_present"}}
_GOAL = {
    "frame_ref": {"kind": "target_frame", "target_role": "arm", "frame_role": "base"},
    "position_m": [0.3, 0.0, 0.2],
    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
}
_KEYS = [
    {"key": "cell.count", "type": "int64", "access": "read_write", "persistence": "run"},
    {"key": "cell.done", "type": "bool", "access": "write", "persistence": "run"},
    {"key": "cell.force", "type": "float64", "access": "read", "persistence": "run",
     "initial": 0.5},
    {"key": "cell.label", "type": "string", "access": "read_write", "persistence": "run"},
    {"key": "cell.twice", "type": "int64", "access": "read", "persistence": "run",
     "expression": "key.cell.count * 2"},
    {"key": "cell.place", "type": "stored_pose", "access": "read_write", "persistence": "run"},
]


def _compare(operand: dict[str, Any], op: str, value: Any) -> dict[str, Any]:
    return {"compare": {"operand": operand, "op": op, "value": value}}


def _key(name: str) -> dict[str, Any]:
    return {"key": name}


def _rich_condition() -> dict[str, Any]:
    return {
        "any": [
            _compare(_SIGNAL, "==", True),
            {
                "all": [
                    _compare(_key("cell.count"), ">=", 10),
                    _compare(_key("cell.force"), "<", 2),
                    _compare(_key("cell.twice"), "!=", 4.5),
                    _compare(_key("cell.done"), "==", False),
                    {"not": _compare(_key("cell.label"), "==", "skip")},
                    _compare(_key("node_outcome.probe"), "==", "signal_trip"),
                ]
            },
        ]
    }


def _document(
    condition: Any = None,
    *,
    node_id: str = "motion.move_until_signal",
    extra: dict[str, Any] | None = None,
    value_sources: dict[str, Any] | None = None,
) -> dict[str, Any]:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    arm = document["target_roles"]["arm"]
    arm["required_resource_roles"] = sorted({*arm["required_resource_roles"], "part_present"})
    capabilities = {row["capability_id"] for row in arm["required_capabilities"]}
    for name in _NODES:
        for requirement in _CATALOG.get(name).required_capabilities:
            if requirement.scope == "target_binding" and requirement.when_parameter is None:
                capabilities.add(requirement.capability_id)
    arm["required_capabilities"] = [
        {"capability_id": capability, "parameters": {}, "disposition": "required"}
        for capability in sorted(capabilities)
    ]
    document["blackboard"] = {
        "namespaces": [{"namespace_id": "cell", "owner": "program"}],
        "keys": sorted(deepcopy(_KEYS), key=lambda row: row["key"]),
    }
    parameters: dict[str, Any] = {
        "target_role": "arm",
        "base_frame_role": "base",
        "tcp_frame_role": "tcp",
        "goal": deepcopy(_GOAL),
        "stop_mode": "soft_on_path",
        "interrupt_condition": _rich_condition() if condition is None else condition,
        **(extra or {}),
    }
    node: dict[str, Any] = {
        "node_id": node_id,
        "instance_id": "search",
        "parameters": parameters,
        "children": [],
    }
    if value_sources:
        node["value_sources"] = value_sources
    probe = {
        "node_id": "motion.move_until_signal",
        "instance_id": "probe",
        "parameters": {
            "target_role": "arm",
            "base_frame_role": "base",
            "tcp_frame_role": "tcp",
            "goal": deepcopy(_GOAL),
            "stop_mode": "soft_on_path",
            "interrupt_condition": _compare(_SIGNAL, "==", True),
        },
        "children": [],
    }
    document["tree"]["children"] = [node, probe]
    return document


def _program(document: dict[str, Any]) -> Program:
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return Program.from_dict(document, _CATALOG)


# Catalog -----------------------------------------------------------------


@pytest.mark.parametrize("node_id", _NODES)
def test_catalog_declares_the_per_node_interrupt_fields(node_id: str) -> None:
    parameters = {parameter.id: parameter for parameter in _CATALOG.get(node_id).parameters}
    assert parameters["stop_mode"].required
    assert "interrupt_policy_id" not in parameters  # contract 63
    # The ordinary motion fields of the move (same as motion.move_to_pose).
    assert parameters["base_frame_role"].required and parameters["tcp_frame_role"].required
    pose = {parameter.id: parameter for parameter in _CATALOG.get("motion.move_to_pose").parameters}
    for setting in ("velocity_scale", "acceleration_scale", "joint_position_tol_rad",
                    "translation_tol_m", "rotation_tol_rad", "planning_timeout_s",
                    "execution_timeout_s", "cache_mode"):
        assert parameters[setting].default == pose[setting].default
    assert "automatic_retries" not in parameters  # spec 410 §4.1
    condition = parameters["interrupt_condition"]
    assert condition.required and condition.schema.schema_type == "interrupt_condition"
    for optional in ("on_signal", "on_no_signal", "stop_entry_stage_id", "success_through_stage_id"):
        assert not parameters[optional].required and not parameters[optional].has_default
    assert parameters["on_signal"].schema.enum_values == ("success", "failure")
    assert parameters["priority"].default == 0
    assert dict(parameters["priority"].schema.constraints) == {
        "min_inclusive": 0,
        "max_inclusive": 100,
    }
    assert parameters["success_through_stage_id"].schema.schema_type == "string"
    assert "fallback_outcome" not in parameters  # contract 70
    registry = _CATALOG.schema_registry
    json_schema = registry.to_json_schema(condition.schema)
    assert json_schema["properties"]["not"] == {"$ref": "#"}
    assert registry.schema_hash(condition.schema)
    assert validate_value(condition.schema, _rich_condition(), registry=registry) == []
    issues = validate_value(condition.schema, {"all": []}, registry=registry)
    assert issues and "interrupt_condition_invalid" in issues[0].message
    assert issues[0].path == "$.all"


# Program validation ------------------------------------------------------


def test_a_condition_over_signals_and_every_scalar_key_kind_is_accepted() -> None:
    program = _program(_document())
    # Write-only, computed and system node_outcome keys are all eligible; the
    # computed key's dependency cell.count is named on its own here.
    assert program.interrupt_condition_keys == (
        "cell.count",
        "cell.done",
        "cell.force",
        "cell.label",
        "cell.twice",
        "node_outcome.probe",
    )
    assert condition_keys(_compare(_key("cell.twice"), "==", 2)) == {"cell.twice"}


def test_the_key_union_names_only_condition_operands() -> None:
    program = _program(_document(_compare(_key("cell.twice"), ">", 1)))
    assert program.interrupt_condition_keys == ("cell.twice",)
    assert _program(_document(_compare(_SIGNAL, "!=", False))).interrupt_condition_keys == ()


def _deep(depth: int) -> dict[str, Any]:
    condition = _compare(_SIGNAL, "==", True)
    for _ in range(depth - 1):
        condition = {"not": condition}
    return condition


@pytest.mark.parametrize(
    "condition, message",
    [
        ({"all": []}, "interrupt_condition_invalid: must be a non-empty list"),
        ({"xor": [_compare(_SIGNAL, "==", True)]}, "unknown operator 'xor'"),
        ({"compare": {"operand": _SIGNAL, "op": "==", "value": True, "x": 1}}, "unknown fields"),
        (_compare({"key": "cell.count", "signal": _SIGNAL["signal"]}, "==", 1), "exactly one operand"),
        (_compare({"signal": {"target_role": "arm"}}, "==", True), "is missing fields"),
        (_compare(_SIGNAL, "~", True), "must be one of"),
        (_compare(_key("cell.count"), "==", _key("cell.count")), "no operand-to-operand"),
        (_deep(9), "exceeds the depth bound 8"),
        ({"any": [_compare(_SIGNAL, "==", True)] * 65}, "exceeds the bound of 64 nodes"),
        (_compare({"param": "speed"}, "==", 1), "interrupt_condition_key_refused: a parameter operand"),
        (_compare(_key("cell.missing"), "==", 1), "interrupt_condition_key_refused: 'cell.missing'"),
        (_compare(_key("cell.place"), "==", 1), "compares only bool, int64, float64 or string"),
        (_compare(_SIGNAL, "<", True), "io_signal_type_mismatch: a bool operand accepts only"),
        (_compare(_key("cell.label"), ">=", "a"), "a string operand accepts only"),
        (_compare(_key("cell.done"), "==", 1), "io_signal_type_mismatch: a int64 literal"),
        (_compare(_key("cell.count"), "==", True), "a bool literal does not compare"),
        (_compare(_key("cell.label"), "==", 3), "no coercion"),
        (
            _compare({"signal": {"target_role": "hand", "resource_role": "x"}}, "==", True),
            "does not name a declared target role",
        ),
        (
            _compare({"signal": {"target_role": "arm", "resource_role": "x"}}, "==", True),
            "is not a resource role required by target role 'arm'",
        ),
    ],
)
def test_invalid_conditions_are_refused_with_their_reason(condition, message) -> None:
    with pytest.raises(ProgramValidationError, match=message.replace("(", r"\(")):
        _program(_document(condition))


@pytest.mark.parametrize("literal", [2**53, -(2**53), float(2**60)])
def test_integer_literals_beyond_the_safe_range_are_refused(literal) -> None:
    # Whole documents already refuse them (I-JSON); the grammar refuses too.
    with pytest.raises(InterruptConditionError, match="outside the exact range"):
        check_condition_structure(_compare(_key("cell.count"), "==", literal))


def test_seventeen_distinct_operands_are_accepted() -> None:
    # Contract 137: no separate input limit; only the node and depth bounds.
    signals = [{"signal": {"target_role": "arm", "resource_role": f"in{index}"}} for index in range(17)]
    check_condition_structure({"any": [_compare(signal, "==", True) for signal in signals]})
    keys = [f"cell.k{index}" for index in range(17)]
    check_condition_structure({"any": [_compare(_key(key), "==", 1) for key in keys]})


def test_largest_legal_tree_passes_and_one_more_node_is_refused() -> None:
    # Root + 63 compares = 64 nodes, each on a distinct input (the 63 maximum).
    compares = [_compare(_key(f"cell.k{index}"), "==", 1) for index in range(64)]
    check_condition_structure({"any": compares[:63]})
    with pytest.raises(InterruptConditionError, match="exceeds the bound of 64 nodes"):
        check_condition_structure({"any": compares})


def test_repeated_operand_uses_are_accepted() -> None:
    names = ["cell.count", "cell.force", "cell.twice"]
    condition = {"any": [_compare(_key(names[index % 3]), ">", index) for index in range(17)]}
    assert _program(_document(condition)).interrupt_condition_keys == tuple(sorted(names))


@pytest.mark.parametrize(
    "extra, message",
    [
        ({"priority": 101}, "priority"),
        ({"success_through_stage_id": 3}, "must be a string"),
        ({"fallback_outcome": "fail_node"}, "unknown catalog parameters"),
        ({"on_signal": "maybe"}, "success, failure"),
        ({"stop_entry_stage_id": 3}, "must be a string"),
    ],
)
def test_rule_settings_are_validated(extra, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(_document(extra=extra))


def test_rule_settings_are_accepted() -> None:
    _program(
        _document(
            extra={
                "on_signal": "failure",
                "on_no_signal": "success",
                "priority": 100,
                "stop_entry_stage_id": "controller_goal_cancel",
                "success_through_stage_id": "software_hard_stop",
            }
        )
    )


@pytest.mark.parametrize(
    "mutate, message",
    [
        (lambda bb: bb["namespaces"].append({"namespace_id": "interrupt_state", "owner": "program"}),
         "interrupt_state is reserved"),
        (lambda bb: bb["keys"].append({"key": "interrupt_state.guard", "type": "string",
                                       "access": "read", "persistence": "run"}),
         "reserved interrupt_state namespace"),
    ],
)
def test_the_interrupt_state_namespace_is_reserved(mutate, message) -> None:
    document = _document()
    mutate(document["blackboard"])
    document["blackboard"]["keys"].sort(key=lambda row: row["key"])
    with pytest.raises(ProgramValidationError, match=message):
        _program(document)


def test_a_node_rule_references_no_deployment_policy() -> None:
    with pytest.raises(ProgramValidationError, match="unknown catalog parameters"):
        _program(_document(extra={"interrupt_policy_id": "guard"}))


@pytest.mark.parametrize(
    "parameter",
    [
        "interrupt_condition",
        "interrupt_condition.not",
        "priority",
        "on_signal",
        "stop_entry_stage_id",
        "success_through_stage_id",
    ],
)
def test_interrupt_rule_fields_stay_literal(parameter: str) -> None:
    with pytest.raises(ProgramValidationError, match="cannot dynamically select"):
        _program(
            _document(
                value_sources={parameter: {"kind": "blackboard_key", "reference": "cell.label"}}
            )
        )


# Node outcome keys -------------------------------------------------------


def test_interrupt_motion_nodes_publish_their_outcome_key() -> None:
    program = _program(_document())
    for instance_id in ("search", "probe"):
        declaration = program.blackboard_keys[f"node_outcome.{instance_id}"]
        assert (declaration.type, declaration.access) == ("string", "read")
    assert {"signal_trip", "no_signal_trip"} <= set(NODE_OUTCOME_VALUES)
    store = ProgramValueStore(program)
    store.write_node_outcome("search", "no_signal_trip")
    store.write_node_outcome("probe", "signal_trip")
    assert store.read("node_outcome.probe") == "signal_trip"


def test_a_write_only_condition_key_can_be_published() -> None:
    program = _program(_document())
    store = ProgramValueStore(program)
    baseline: list[Any] = []
    store.subscribe_published_keys(
        program.interrupt_condition_keys, baseline=baseline.append, on_change=lambda _c: None
    )
    assert [change.key for change in baseline[0]] == list(program.interrupt_condition_keys)


def test_the_cell_level_union_is_the_applied_programs_condition_keys() -> None:
    from manyforge_program_runner.runtime import ProgramRuntime

    program = _program(_document(_compare(_key("cell.label"), "==", "go")))
    owner = SimpleNamespace(_applied_condition_keys=())
    assert ProgramRuntime._cell_bbkey_union(owner) == ()
    # A Run start (or cold Apply) teaches the union; Reset reuses it.
    assert ProgramRuntime._learn_condition_keys(owner, program) == ("cell.label",)
    assert ProgramRuntime._cell_bbkey_union(owner) == ("cell.label",)


# Runner mapping ----------------------------------------------------------

_CHAIN = ("controlled_stop", "controller_goal_cancel", "software_hard_stop")


def _row(
    cause: str | None,
    *,
    stop_result: str | None = None,
    trip: bool = False,
    losses: tuple[dict[str, Any], ...] = (),
    operation_id: str = "op",
    error: dict[str, Any] | None = None,
    blackboard: dict[str, Any] | None = None,
) -> dict[str, Any]:
    trip_row: dict[str, Any] | None = None
    if trip:
        trip_row = {"triggering_acquisition": None}
        if blackboard is not None:
            trip_row["blackboard"] = blackboard
    return {
        "registration": {
            "owner_epoch": "kernel-1",
            "policy_id": "door_rule",
            "scope": {"kind": "operation", "operation_id": operation_id},
        },
        "evidence": {
            "state": "retired",
            "trip": trip_row,
            "source_losses": list(losses),
            "evaluation_error": None,
        },
        "stop_id": "stop" if stop_result else None,
        "stop_generation": 1 if stop_result else None,
        "stop_certificate_id": "cert" if stop_result else None,
        "stop_certificate_revision": 1 if stop_result else None,
        "stop_result": stop_result,
        "terminal_cause": cause,
        "error": error,
    }


def _operation(*rows: dict[str, Any]) -> dict[str, Any]:
    return {"operation_id": "op", "participants": [{"io_rule_outcomes": list(rows)}]}


def _map(node_id: str, row: dict[str, Any], *, contained: str | None = None, **parameters: Any):
    return map_interrupt_result(
        node_id, parameters, _operation(row), stop_chain=_CHAIN, contained_stage_id=contained
    )


_NOMINAL = dict(stop_result="succeeded_nominal", trip=True)
_ESCALATED = dict(stop_result="succeeded_escalated", trip=True)
_LOSS = ({"source_id": "gripper.io", "source_epoch": "e1", "kind": "stale"},)
_THROUGH_CANCEL = {"success_through_stage_id": "controller_goal_cancel"}


@pytest.mark.parametrize(
    "node_id, row, parameters, expected",
    [
        # Trip contained by the entry stage: on_signal (default success).
        ("motion.move_until_signal", _row("signal_trip", **_NOMINAL), {}, (True, "signal_trip")),
        ("motion.probe_search", _row("signal_trip", **_NOMINAL), {}, (True, "signal_trip")),
        ("motion.probe_search", _row("signal_trip", **_NOMINAL), {"on_signal": "failure"},
         (False, "signal_trip")),
        # Natural completion without a trip: existing spec 260 mapping.
        ("motion.move_until_signal", _row("no_signal_trip"), {}, (True, "no_signal_trip")),
        ("motion.probe_search", _row("no_signal_trip"), {}, (False, "no_signal_trip")),
        ("motion.probe_search", _row("no_signal_trip"), {"on_no_signal": "success"},
         (True, "no_signal_trip")),
        ("motion.move_until_signal", _row("no_signal_trip"), {"on_no_signal": "failure"},
         (False, "no_signal_trip")),
        # Contract 70: a later stage counts only up to success_through_stage_id.
        ("motion.move_until_signal", _row("failed", **_ESCALATED), {}, (False, "failed")),
        ("motion.move_until_signal", _row("signal_trip", **_ESCALATED), {}, (False, "failed")),
        ("motion.move_until_signal", _row("failed", **_ESCALATED), _THROUGH_CANCEL,
         (True, "signal_trip")),
        ("motion.move_until_signal", _row("signal_trip", **_ESCALATED), _THROUGH_CANCEL,
         (True, "signal_trip")),
        ("motion.move_until_signal", _row("failed", **_ESCALATED),
         {**_THROUGH_CANCEL, "on_signal": "failure"}, (False, "signal_trip")),
        # Never Success from a match or Stop acceptance alone, nor from
        # unknown or failed containment, even with a threshold.
        ("motion.move_until_signal", _row(None, trip=True), _THROUGH_CANCEL, (False, "failed")),
        ("motion.move_until_signal", _row("failed", stop_result="partial", trip=True),
         _THROUGH_CANCEL, (False, "failed")),
        ("motion.move_until_signal", _row("failed", stop_result="failed_uncontained", trip=True),
         _THROUGH_CANCEL, (False, "failed")),
        # Source loss: Failure through normal failure handling (46c).
        ("motion.move_until_signal", _row("failed", stop_result="succeeded_nominal", losses=_LOSS),
         {}, (False, "failed")),
        ("motion.move_until_signal", _row("no_signal_trip", losses=_LOSS), {}, (False, "failed")),
        ("motion.move_until_signal", _row("failed", **_ESCALATED, losses=_LOSS),
         _THROUGH_CANCEL, (False, "failed")),
        # Results of another operation do not count.
        ("motion.move_until_signal", _row("signal_trip", **_NOMINAL, operation_id="x"), {},
         (False, "failed")),
    ],
)
def test_typed_results_map_to_node_results(node_id, row, parameters, expected) -> None:
    result = _map(node_id, row, contained="controller_goal_cancel", **parameters)
    assert (result.success, result.node_outcome) == expected
    assert result.node_outcome in NODE_OUTCOME_VALUES


@pytest.mark.parametrize(
    "parameters, contained, expected",
    [
        # Omitted threshold: only the entry stage counts (today's rule).
        ({}, "controller_goal_cancel", (False, "motion_interrupt_stop_failed")),
        # Contained at the threshold: success, no escalated-failure handling.
        (_THROUGH_CANCEL, "controller_goal_cancel", (True, None)),
        ({"success_through_stage_id": "software_hard_stop"}, "controller_goal_cancel", (True, None)),
        ({"success_through_stage_id": "software_hard_stop"}, "software_hard_stop", (True, None)),
        # Contained after the threshold: Failure.
        (_THROUGH_CANCEL, "software_hard_stop", (False, "motion_interrupt_stop_failed")),
        # Entry selection shifts the base; the threshold is entry-relative.
        ({"stop_entry_stage_id": "controller_goal_cancel", **_THROUGH_CANCEL},
         "software_hard_stop", (False, "motion_interrupt_stop_failed")),
        ({"stop_entry_stage_id": "controller_goal_cancel",
          "success_through_stage_id": "software_hard_stop"}, "software_hard_stop", (True, None)),
        # Unproven containing stage or an inconsistent threshold: Failure.
        (_THROUGH_CANCEL, None, (False, "motion_interrupt_containing_stage_unproven")),
        (_THROUGH_CANCEL, "unknown_stage", (False, "motion_interrupt_containing_stage_unproven")),
        ({"stop_entry_stage_id": "software_hard_stop", **_THROUGH_CANCEL}, "software_hard_stop",
         (False, "motion_interrupt_containing_stage_unproven")),
    ],
)
def test_success_through_stage_threshold(parameters, contained, expected) -> None:
    result = _map("motion.move_until_signal", _row("failed", **_ESCALATED), contained=contained,
                  **parameters)
    assert (result.success, result.reason_code) == expected
    assert not result.effect_uncertain


def _scope_key(participant: str, operation: str = "op") -> dict[str, Any]:
    return {
        "origin_stop_generation": 1,
        "origin_stop_id": "stop",
        "subject": {"kind": "participant", "operation_id": operation, "participant_id": participant},
    }


def _attempt(participant: str, stage_id: str, index: int, **patch: Any) -> dict[str, Any]:
    return {
        "authority": "authoritative",
        "error": None,
        "operation_id": patch.pop("operation", "op"),
        "outcome": "succeeded",
        "participant_id": participant,
        "participant_stop_invocation_plan_digest": f"plan-{participant}",
        "scope_key": _scope_key(participant, patch.pop("scope_operation", "op")),
        "stage": {"stage_id": stage_id, "stage_index": index, "stage_kind": "x", "action_class": "controlled"},
        **patch,
    }


def _terminal(*attempts: dict[str, Any], **patch: Any) -> dict[str, Any]:
    return {
        "api_version": "manyforge.stop_terminal_projection.v1",
        "details_truncated": False,
        "stop_id": "stop",
        "stop_generation": 1,
        "stop_certificate_id": "cert",
        "stop_certificate_revision": 1,
        "attempted_stages": list(attempts),
        "participant_settlement": [
            {"scope_key": _scope_key(name), "participant_stop_invocation_plan_digest": f"plan-{name}"}
            for name in ("arm", "arm2")
        ],
        **patch,
    }


def test_contained_stage_reads_authoritative_attempts_of_the_rows_own_certificate() -> None:
    row = _row("failed", **_ESCALATED)
    terminal = _terminal(
        _attempt("arm", "controlled_stop", 0, outcome="failed"),
        _attempt("arm", "software_hard_stop", 2, authority="superseded_late"),
        _attempt("arm", "software_hard_stop", 2, error={"code": "e"}),
        _attempt("other", "software_hard_stop", 2, operation="other", scope_operation="other"),
        _attempt("arm", "controller_goal_cancel", 1),
    )
    assert contained_stages_from_stop_terminal(row, terminal, operation_id="op") == {
        "arm": "controller_goal_cancel"
    }
    assert contained_stage_from_stop_terminal(row, terminal, operation_id="op") == (
        "controller_goal_cancel"
    )
    # Another certificate, or a bounded summary without details: not proven.
    assert contained_stages_from_stop_terminal(
        row, {**terminal, "stop_certificate_revision": 2}, operation_id="op") is None
    assert contained_stages_from_stop_terminal(
        row, {**terminal, "details_truncated": True}, operation_id="op") is None
    # Another Stop identity or generation (SX-2215): not proven.
    for patch in ({"stop_id": "stop-2"}, {"stop_generation": 2}, {"stop_generation": None}):
        assert contained_stages_from_stop_terminal(
            row, {**terminal, **patch}, operation_id="op") is None
    assert contained_stages_from_stop_terminal(
        {**row, "stop_generation": None}, terminal, operation_id="op") is None
    # A plan digest that is not the participant's own: not proven.
    wrong = _terminal(_attempt("arm", "controller_goal_cancel", 1,
                               participant_stop_invocation_plan_digest="plan-other"))
    assert contained_stages_from_stop_terminal(row, wrong, operation_id="op") is None
    # Natural finish / pre-dispatch no-effect: no attempt, no containing stage.
    assert contained_stages_from_stop_terminal(row, _terminal(), operation_id="op") == {}
    assert contained_stage_from_stop_terminal(row, _terminal(), operation_id="op") is None


def test_coordinated_stop_is_judged_per_participant() -> None:
    row = _row("failed", **_ESCALATED)
    terminal = _terminal(
        _attempt("arm", "controlled_stop", 0),
        _attempt("arm2", "software_hard_stop", 2),
    )
    stages = contained_stages_from_stop_terminal(row, terminal, operation_id="op")
    assert stages == {"arm": "controlled_stop", "arm2": "software_hard_stop"}
    # No single guessed stage for several participants.
    assert contained_stage_from_stop_terminal(row, terminal, operation_id="op") is None
    assert contained_stage_from_stop_terminal(
        row, terminal, operation_id="op", participant_id="arm2") == "software_hard_stop"
    chains = {"arm": _CHAIN, "arm2": _CHAIN}
    within = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "software_hard_stop"},
        _operation(row), stop_chain=chains, contained_stage_id=stages)
    assert within.success and within.node_outcome == "signal_trip"
    beyond = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "controller_goal_cancel"},
        _operation(row), stop_chain=chains, contained_stage_id=stages)
    assert (beyond.success, beyond.reason_code) == (False, "motion_interrupt_stop_failed")
    unproven = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "software_hard_stop"},
        _operation(row), stop_chain={"arm": _CHAIN}, contained_stage_id=stages)
    assert unproven.reason_code == "motion_interrupt_containing_stage_unproven"


_MOTION = "execution.trajectory.execute"


def _targets_reply(
    stop_response: Any, *, graph: str = "graph-1", extra: tuple[dict[str, Any], ...] = ()
) -> dict[str, Any]:
    motion_command: dict[str, Any] = {"stop_response": stop_response}
    if stop_response is None:
        motion_command["stop_response_error"] = {"code": "unavailable", "message": "x", "details": {}}
    return {
        "ok": True,
        "identity": {"owner_epoch": "kernel-1", "retained_bundle_id": "bundle-1",
                     "graph_revision": graph},
        "targets": {
            "schema_version": "manyforge.execution_targets.v3",
            "components": [{"controllable_component_id": "arm.component", "targets": [
                {"command_target_id": "arm.target", "operation_capability_id": _MOTION,
                 "presentation": "motion", "command": motion_command},
                *extra,
            ]}],
        },
    }


_STOP_RESPONSE = {
    "control_binding_id": "arm.binding", "control_binding_revision": "rev-1",
    "control_route_id": "route", "nominal_stop_mode": "soft_on_path",
    "stages": [{"stage_id": stage, "stage_kind": "x", "command_kind": "y"} for stage in _CHAIN],
}


def _chains_from_targets(reply: dict[str, Any], **participant: Any):
    from manyforge_behavior.production_ports import NativeKernelProgramPort

    port = object.__new__(NativeKernelProgramPort)
    port._kernel = SimpleNamespace(targets=lambda: reply)  # noqa: SLF001
    capture = SimpleNamespace(retained_bundle_id="bundle-1", graph_revision="graph-1")
    port._context = lambda run_id: SimpleNamespace(capture=capture)  # noqa: SLF001
    task = SimpleNamespace(identity=SimpleNamespace(program_run_id="run"))
    row = _row("failed", **_ESCALATED)
    operation = {"operation_id": "op", "participants": [{
        "participant_id": "arm", "controllable_component_id": "arm.component",
        "command_target_id": "arm.target", "capability_id": _MOTION,
        "control_binding_id": "arm.binding", "control_binding_revision": "rev-1",
        "io_rule_outcomes": [row], **participant,
    }]}
    chains = port._interrupt_stop_chains(task, operation)  # noqa: SLF001
    stages = contained_stages_from_stop_terminal(
        row, _terminal(_attempt("arm", "software_hard_stop", 2)), operation_id="op")
    return chains, map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "software_hard_stop"},
        operation, stop_chain=chains, contained_stage_id=stages)


def test_stop_chain_comes_from_the_used_targets_motion_command() -> None:
    # SX-2211: the qualified stage order of the exact binding the operation used.
    chains, result = _chains_from_targets(_targets_reply(_STOP_RESPONSE))
    assert chains == {"arm": _CHAIN}
    assert (result.success, result.node_outcome) == (True, "signal_trip")
    _chains, beyond = _chains_from_targets(_targets_reply({
        **_STOP_RESPONSE, "stages": _STOP_RESPONSE["stages"][:2]}))
    assert beyond.reason_code == "motion_interrupt_containing_stage_unproven"


@pytest.mark.parametrize(
    "reply, participant",
    [
        # Unavailable: null plus stop_response_error.
        (_targets_reply(None), {}),
        # Another binding revision or id than the operation used.
        (_targets_reply({**_STOP_RESPONSE, "control_binding_revision": "rev-2"}), {}),
        (_targets_reply(_STOP_RESPONSE), {"control_binding_id": "other.binding"}),
        # Another command target or capability; never another target's chain.
        (_targets_reply(_STOP_RESPONSE), {"command_target_id": "other.target"}),
        (_targets_reply(_STOP_RESPONSE), {"capability_id": "other.capability"}),
        (_targets_reply(None, extra=({
            "command_target_id": "arm.target2", "operation_capability_id": _MOTION,
            "presentation": "motion", "command": {"stop_response": _STOP_RESPONSE},
        },)), {}),
        # Targets of another Applied product than the Run's; unreadable.
        (_targets_reply(_STOP_RESPONSE, graph="graph-2"), {}),
        ({"ok": True, "identity": {}, "targets": None}, {}),
    ],
)
def test_unavailable_or_foreign_stop_chain_is_unproven(reply, participant) -> None:
    chains, result = _chains_from_targets(reply, **participant)
    assert chains == {}
    assert (result.success, result.reason_code) == (
        False, "motion_interrupt_containing_stage_unproven")


def test_failure_reasons_and_effect_uncertainty() -> None:
    loss = _map("motion.move_until_signal", _row("failed", stop_result="succeeded_nominal",
                                                  losses=_LOSS))
    assert (loss.reason_code, loss.effect_uncertain) == ("interrupt_source_unavailable", False)
    assert "gripper.io" in loss.message
    uncontained = _map(
        "motion.move_until_signal",
        _row("failed", stop_result="failed_uncontained", trip=True,
             error={"code": "motion_interrupt_backend_stop_failed", "message": "", "details": {}}),
    )
    assert uncontained.reason_code == "motion_interrupt_backend_stop_failed"
    assert uncontained.effect_uncertain
    escalated = _map("motion.move_until_signal", _row("failed", **_ESCALATED))
    assert (escalated.reason_code, escalated.effect_uncertain) == (
        "motion_interrupt_stop_failed",
        False,
    )
    missing = map_interrupt_result(
        "motion.probe_search", {}, {"participants": []}
    )
    assert missing is not None and not missing.success and missing.effect_uncertain
    malformed = _map("motion.probe_search", {**_row("signal_trip"), "terminal_cause": "won"})
    assert malformed.reason_code == "motion_interrupt_result_invalid"


def test_an_ordinary_stop_keeps_the_existing_stop_handling() -> None:
    assert _map("motion.move_until_signal", _row("stopped", stop_result="succeeded_nominal")) is None


# Compact BB evidence (kernel-rule-result-wire02) ----------------------------


def _bb_scope(**overrides: Any) -> dict[str, Any]:
    return {
        "owner_epoch": "kernel-1",
        "program_runner_owner_epoch": "runner-1",
        "program_binding_id": "sha256:" + "a" * 64,
        "active_publication_generation": 3,
        "cycle_generation": 7,
        "claim_generation": 2,
        "program_run_id": "run-1",
        **overrides,
    }


def _trip_bb(**overrides: Any) -> dict[str, Any]:
    return {
        "scope": _bb_scope(),
        "source_id": "program_bbkeys",
        "source_epoch": "bb-epoch-1",
        "message_sequence": "4",
        "received_at_ns": "1234",
        "values": [
            {"name": "cell.count", "value_type": "int64", "value": "-12"},
            {"name": "cell.door_open", "value_type": "bool", "value": True},
            {"name": "cell.label", "value_type": "string", "value": ""},
            {"name": "cell.speed", "value_type": "float64", "value": 0.5},
        ],
        **overrides,
    }


def test_trip_bb_evidence_becomes_the_history_row() -> None:
    result = _map("motion.move_until_signal",
                  _row("signal_trip", **_NOMINAL, blackboard=_trip_bb()))
    assert result.success
    assert result.message.startswith(
        'rule door_rule tripped: cell.count=-12, cell.door_open=true, cell.label="", '
        "cell.speed=0.5 (frame 4)"
    )
    # Between Runs the scope has neither claim nor Run.
    idle = _trip_bb(scope=_bb_scope(claim_generation=None, program_run_id=None))
    assert _map("motion.move_until_signal", _row("signal_trip", **_NOMINAL, blackboard=idle)).success


def test_trip_bb_evidence_must_match_its_causal_acquisition() -> None:
    row = _row("signal_trip", **_NOMINAL, blackboard=_trip_bb())
    row["evidence"]["trip"]["triggering_acquisition"] = {
        "source_id": "program_bbkeys", "source_epoch": "bb-epoch-1",
        "source_sequence": "4", "source_cursor": {},
    }
    assert _map("motion.move_until_signal", row).success
    row["evidence"]["trip"]["triggering_acquisition"]["source_sequence"] = "3"
    assert _map("motion.move_until_signal", row).reason_code == "motion_interrupt_result_invalid"


@pytest.mark.parametrize(
    "blackboard",
    [
        {**_trip_bb(), "extra": 1},
        {k: v for k, v in _trip_bb().items() if k != "received_at_ns"},
        _trip_bb(scope={**_bb_scope(), "extra": 1}),
        _trip_bb(scope=_bb_scope(owner_epoch="kernel-2")),
        _trip_bb(scope=_bb_scope(claim_generation=None)),
        _trip_bb(scope=_bb_scope(active_publication_generation=0)),
        _trip_bb(scope=_bb_scope(cycle_generation="7")),
        _trip_bb(message_sequence="0"),
        _trip_bb(message_sequence="04"),
        _trip_bb(message_sequence=4),
        _trip_bb(message_sequence=str(2**64)),
        _trip_bb(received_at_ns="-1"),
        _trip_bb(values=[]),
        _trip_bb(values=[{"name": "b", "value_type": "bool", "value": True},
                         {"name": "a", "value_type": "bool", "value": True}]),
        _trip_bb(values=[{"name": "a", "value_type": "bool", "value": True}] * 2),
        _trip_bb(values=[{"name": "a", "value_type": "bool", "value": None}]),
        _trip_bb(values=[{"name": "a", "value_type": "bool", "value": 1}]),
        _trip_bb(values=[{"name": "a", "value_type": "int64", "value": 3}]),
        _trip_bb(values=[{"name": "a", "value_type": "int64", "value": "-0"}]),
        _trip_bb(values=[{"name": "a", "value_type": "int64", "value": "+3"}]),
        _trip_bb(values=[{"name": "a", "value_type": "int64", "value": str(2**63)}]),
        _trip_bb(values=[{"name": "a", "value_type": "float64", "value": True}]),
        _trip_bb(values=[{"name": "a", "value_type": "float64", "value": "0.5"}]),
        _trip_bb(values=[{"name": "a", "value_type": "string", "value": 1}]),
        _trip_bb(values=[{"name": "a", "value_type": "pose", "value": {}}]),
        _trip_bb(values=[{"name": "a", "value_type": "bool", "value": True, "x": 1}]),
    ],
)
def test_malformed_trip_bb_evidence_is_refused(blackboard) -> None:
    result = _map("motion.move_until_signal",
                  _row("signal_trip", **_NOMINAL, blackboard=blackboard))
    assert (result.success, result.reason_code, result.effect_uncertain) == (
        False, "motion_interrupt_result_invalid", True,
    )


def _loss_bb(**overrides: Any) -> dict[str, Any]:
    return {
        "scope": _bb_scope(),
        "source_id": "program_bbkeys",
        "source_epoch": "bb-epoch-1",
        "last_accepted_message_sequence": "0",
        "unavailable_key": "cell.door_open",
        **overrides,
    }


def test_loss_bb_evidence_names_the_unavailable_key() -> None:
    loss = {"source_id": "program_bbkeys", "source_epoch": "bb-epoch-1", "kind": "unavailable",
            "blackboard": _loss_bb()}
    result = _map("motion.move_until_signal", _row("failed", stop_result="succeeded_nominal",
                                                    losses=(loss,)))
    assert result.reason_code == "interrupt_source_unavailable"
    assert result.message.endswith("key cell.door_open unavailable (last frame 0)")
    whole = {**loss, "blackboard": _loss_bb(unavailable_key=None,
                                             last_accepted_message_sequence="9")}
    result = _map("motion.move_until_signal", _row("no_signal_trip", losses=(whole,)))
    assert result.message.endswith("BB source unavailable (last frame 9)")


@pytest.mark.parametrize(
    "blackboard",
    [
        _loss_bb(source_epoch="bb-epoch-2"),
        _loss_bb(source_id="other"),
        _loss_bb(last_accepted_message_sequence="-1"),
        _loss_bb(unavailable_key=""),
        {**_loss_bb(), "values": []},
        _loss_bb(scope=_bb_scope(program_run_id=None)),
    ],
)
def test_malformed_loss_bb_evidence_is_refused(blackboard) -> None:
    loss = {"source_id": "program_bbkeys", "source_epoch": "bb-epoch-1", "kind": "unavailable",
            "blackboard": blackboard}
    result = _map("motion.move_until_signal", _row("no_signal_trip", losses=(loss,)))
    assert result.reason_code == "motion_interrupt_result_invalid"


@pytest.mark.parametrize("stop_result", ["succeeded_nominal", "succeeded_escalated", "failed_uncontained", None])
def test_interrupted_fault_is_a_failure_never_a_trip_success(stop_result) -> None:
    row = _row("interrupted_fault", stop_result=stop_result, trip=True,
               error={"code": "motion_interrupted_fault", "message": "", "details": {}})
    result = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "software_hard_stop"},
        _operation(row), stop_chain=_CHAIN, contained_stage_id="controlled_stop")
    assert result is not None and not result.success
    assert result.node_outcome == "failed"
    assert result.reason_code == "motion_interrupted_fault"
    assert result.effect_uncertain == (stop_result in {None, "failed_uncontained"})


def test_a_bounded_summary_without_attempts_is_unknown_and_fails() -> None:
    # SX-2211: no attempted_stages (bounded summary) is unknown, never
    # inferred from the configured chain.
    row = _row("failed", **_ESCALATED)
    summary = {
        key: value for key, value in _terminal(_attempt("arm", "controlled_stop", 0)).items()
        if key != "attempted_stages"
    }
    summary["details_truncated"] = True
    assert contained_stages_from_stop_terminal(row, summary, operation_id="op") is None
    result = map_interrupt_result(
        "motion.move_until_signal", {"success_through_stage_id": "software_hard_stop"},
        _operation(row), stop_chain=_CHAIN,
        contained_stage_id=contained_stages_from_stop_terminal(row, summary, operation_id="op"))
    assert (result.success, result.reason_code) == (
        False, "motion_interrupt_containing_stage_unproven")


# ---- Contract 87: interrupt condition keys declare `initial:` ----


def _key_rule(rule_id: str, key: str, value: Any) -> dict[str, Any]:
    return {"rule_id": rule_id, "scope": "all_motion", "condition": _compare(_key(key), "==", value)}


def test_node_rule_keys_without_initial_are_refused_where_the_deployment_is_known() -> None:
    program = _program(_document(_compare(_key("cell.count"), ">=", 1)))
    with pytest.raises(ProgramValidationError, match=INTERRUPT_KEY_INITIAL_REQUIRED) as caught:
        require_interrupt_key_initials(program)
    assert "cell.count (read by a node rule)" in str(caught.value)
    # A key with initial (0.5 here; false / 0 are equally valid) passes.
    require_interrupt_key_initials(_program(_document(_compare(_key("cell.force"), "<", 2))))


def test_global_rule_keys_without_initial_are_refused_and_unused_keys_are_unchanged() -> None:
    program = _program(_document(_compare(_SIGNAL, "==", True)))
    require_interrupt_key_initials(program)  # cell.count has no initial but no rule reads it.
    with pytest.raises(ProgramValidationError, match="global rule 'armed'"):
        require_interrupt_key_initials(program, [_key_rule("armed", "cell.label", "x")])
    require_interrupt_key_initials(program, [_key_rule("ok", "cell.force", 1.0)])


def test_false_and_zero_initials_count_as_declared() -> None:
    document = _document(_compare(_key("cell.count"), ">=", 1))
    for row in document["blackboard"]["keys"]:
        if row["key"] == "cell.count":
            row["initial"] = 0
        if row["key"] == "cell.done":
            row["access"] = "read_write"
            row["initial"] = False
    program = _program(document)
    require_interrupt_key_initials(program, [_key_rule("off", "cell.done", True)])


def test_system_keys_need_no_initial_their_start_value_is_defined() -> None:
    # Contract 105: node_outcome.* starts at `none`, interrupt_state.* follows
    # its rule; neither needs `initial:`.
    condition = {
        "all": [
            _compare(_key("node_outcome.probe"), "!=", "completed"),
            _compare(_key("node_outcome.probe"), "!=", "none"),
            _compare(_key("interrupt_state.guard"), "==", "tripped"),
        ]
    }
    program = _program(_document(condition))
    assert not program.blackboard_keys["interrupt_state.guard"].has_initial
    require_interrupt_key_initials(program)
    require_interrupt_key_initials(program, [_key_rule("after_pick", "node_outcome.probe", "none")])


# ---- Contract 106: global rules' keys against the Program and the Deployment ----


def test_a_program_missing_a_global_rule_key_is_refused_naming_rule_and_key() -> None:
    program = _program(_document(_compare(_SIGNAL, "==", True)))
    require_global_rule_keys(program, [_key_rule("ok", "cell.force", 1.0),
                                       _key_rule("pick_done", "node_outcome.probe", "completed")])
    with pytest.raises(ProgramValidationError, match="global_rule_key_undeclared") as caught:
        require_global_rule_keys(program, [_key_rule("door", "cell.door_open", True),
                                           _key_rule("gone", "node_outcome.ghost", "none")])
    assert "global rule 'door' reads cell.door_open" in str(caught.value)
    assert "global rule 'gone' reads node_outcome.ghost" in str(caught.value)


def test_global_rules_cannot_read_rule_states_but_programs_can() -> None:
    # Contract 111: refused in Deployment conditions; node rules keep them
    # (test_system_keys_need_no_initial_their_start_value_is_defined).
    require_global_rule_operands([_key_rule("a", "cell.force", 1.0)])
    with pytest.raises(ProgramValidationError,
                       match="global_rule_state_operand_unsupported: .*global rule 'a' reads interrupt_state.b"):
        require_global_rule_operands([_key_rule("a", "interrupt_state.b", "tripped")])
