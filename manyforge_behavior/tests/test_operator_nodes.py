"""Ask operator and Stop motion groups: Program validation and node behaviour."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import py_trees
import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.nodes.operator import MotionStopGroups, OperatorRequestChoice
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopSnapshot
from manyforge_behavior.orchestration_ports import NodeAttemptIdentity
from manyforge_behavior.program import Program, ProgramValidationError

_ROOT = Path(__file__).resolve().parents[2]
_CATALOG = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
_RUNNING = py_trees.common.Status.RUNNING
_SUCCESS = py_trees.common.Status.SUCCESS
_FAILURE = py_trees.common.Status.FAILURE
_OPTIONS = [
    {"option_id": "continue", "label": "Bin emptied, continue", "result": "success"},
    {"option_id": "abort", "label": "Abort this cycle", "result": "failure"},
]


def _document(node: dict[str, Any]) -> dict[str, Any]:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["catalog"] = {
        "api_version": _CATALOG.api_version,
        "content_version": _CATALOG.content_version,
    }
    document["blackboard"] = {
        "namespaces": [{"namespace_id": "cell", "owner": "program"}],
        "keys": [
            {"key": "cell.answer", "type": "string", "access": "read_write", "persistence": "run"},
            {"key": "cell.count", "type": "int64", "access": "read_write", "persistence": "run"},
        ],
    }
    document["execution_groups"] = {"arm": {"execution_policy": "independent"}}
    document["tree"]["children"][0]["execution_groups"] = ["arm"]
    document["tree"]["children"].append({"children": [], **node})
    return document


def _program(document: dict[str, Any]) -> Program:
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v4",
    )
    return Program.from_dict(document, _CATALOG)


def _ask(**parameters: Any) -> dict[str, Any]:
    return {
        "node_id": "operator.request_choice",
        "instance_id": "ask",
        "parameters": {"message": "Bin is full", "options": deepcopy(_OPTIONS), **parameters},
    }


def _stop(groups: list[str]) -> dict[str, Any]:
    return {"node_id": "motion.stop_groups", "instance_id": "stop", "parameters": {"groups": groups}}


@pytest.mark.parametrize(
    "node",
    [
        _ask(),
        _ask(severity="fault", output_key="cell.answer", hold_motion=["arm"]),
        _ask(hold_motion=["all"]),
        _stop(["arm"]),
        _stop(["all"]),
    ],
)
def test_valid_operator_nodes_are_accepted(node) -> None:
    _program(_document(node))


@pytest.mark.parametrize(
    "node, message",
    [
        (_ask(hold_motion=["tools"]), "not a declared execution group"),
        (_ask(hold_motion=["arm", "arm"]), "names a motion group twice"),
        (_ask(hold_motion=["all", "arm"]), "all cannot be combined"),
        (_stop(["tools"]), "not a declared execution group"),
        (_stop([]), "at least 1"),
        (_ask(options=_OPTIONS[:1]), "at least 2"),
        (_ask(options=[{**_OPTIONS[0]}, {**_OPTIONS[1], "option_id": "continue"}]), "repeats another option"),
        (_ask(options=[{**_OPTIONS[0], "option_id": "Continue"}, _OPTIONS[1]]), "must be an identifier"),
        (_ask(options=[{**_OPTIONS[0], "label": " "}, _OPTIONS[1]]), "non-empty text"),
        (_ask(message=" "), "non-empty text"),
        (_ask(output_key="cell.count"), "expected 'string'"),
    ],
)
def test_invalid_operator_nodes_are_refused(node, message) -> None:
    with pytest.raises(ProgramValidationError, match=message):
        _program(_document(node))


@pytest.mark.parametrize("name", ["options", "hold_motion"])
def test_the_answers_and_held_groups_are_literal_only(name) -> None:
    node = _ask()
    node["value_sources"] = {name: {"kind": "parameter", "parameter_id": "anything"}}
    with pytest.raises(ProgramValidationError):
        _program(_document(node))


class _Attempts:
    def __init__(self) -> None:
        self.count = 0

    def begin(self, node_instance_id: str) -> NodeAttemptIdentity:
        self.count += 1
        return NodeAttemptIdentity(1, node_instance_id, self.count)


class _Store:
    def __init__(self) -> None:
        self.writes: list[tuple[str, Any]] = []

    def write(self, key: str, value: Any, *, metadata=None) -> None:
        self.writes.append((key, value))


class _Resolver:
    def __init__(self, parameters: dict[str, Any]) -> None:
        self._parameters = parameters
        self.value_store = _Store()

    def snapshot(self, **_kwargs: Any) -> SimpleNamespace:
        return SimpleNamespace(parameters=self._parameters, blackboard={})


def _choice(**parameters: Any) -> tuple[OperatorRequestChoice, _Resolver, list[dict[str, Any]]]:
    resolver = _Resolver(
        {"message": "Bin is full", "severity": "warning", "options": deepcopy(_OPTIONS),
         "hold_motion": [], **parameters}
    )
    events: list[dict[str, Any]] = []
    node = OperatorRequestChoice(
        name="ask", parameters={}, value_sources={}, input_resolver=resolver,
        attempt_source=_Attempts(), audit_sink=events.append,
    )
    return node, resolver, events


def _tick(node: py_trees.behaviour.Behaviour) -> py_trees.common.Status:
    for _ in node.tick():
        pass
    return node.status


def test_the_question_is_open_until_answered_then_the_option_decides() -> None:
    node, resolver, events = _choice(output_key="cell.answer")
    assert _tick(node) is _RUNNING
    question = node.question()
    assert question["node_attempt_id"] == 1 and question["severity"] == "warning"
    assert [row["option_id"] for row in question["options"]] == ["continue", "abort"]
    assert node.program_stop_request() is None  # No hold_motion: no Stop.
    assert [event["kind"] for event in events] == ["program_operator_choice_requested"]
    for _ in range(3):
        assert _tick(node) is _RUNNING  # No timeout.
    with pytest.raises(ValueError):
        node.answer(2, "continue")  # Another attempt.
    with pytest.raises(ValueError):
        node.answer(1, "later")  # No such option.
    assert node.answer(1, "abort")["result"] == "failure"
    assert node.question() is None
    with pytest.raises(ValueError):
        node.answer(1, "continue")  # Answered already.
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "operator_choice_failed"
    assert node.failure_message == "The operator chose Abort this cycle"
    assert resolver.value_store.writes == [("cell.answer", "abort")]


def test_a_success_option_succeeds_without_an_output_key() -> None:
    node, resolver, _events = _choice()
    _tick(node)
    node.answer(1, "continue")
    assert _tick(node) is _SUCCESS
    assert resolver.value_store.writes == []


def test_held_groups_are_requested_once_and_the_answer_waits_for_the_stop() -> None:
    node, _resolver, events = _choice(hold_motion=["arm"])
    assert _tick(node) is _RUNNING
    attempt, groups, key = node.program_stop_request()
    assert (attempt, groups, len(key)) == (1, ("arm",), 16)
    assert node.program_stop_request()[2] == key  # Resent with the same key.
    node.answer(1, "continue")
    assert _tick(node) is _RUNNING  # The Stop is not accepted yet.
    node.note_program_stop_accepted(1, 7)
    assert node.program_stop_request() is None
    assert events[-1] == {
        "kind": "program_motion_groups_stopped", "node_instance_id": "ask",
        "node_attempt_id": 1, "group_ids": ["arm"],
    }
    assert _tick(node) is _SUCCESS


def test_a_program_stop_interrupts_an_open_question() -> None:
    node, _resolver, _events = _choice(hold_motion=["arm"])
    _tick(node)
    node.stop(py_trees.common.Status.INVALID)
    assert node.outcome == "interrupted"
    assert node.question() is None and node.program_stop_request() is None


class _StopState:
    def __init__(self) -> None:
        self.scope = OperatorMotionStopSnapshot()

    def snapshot(self) -> OperatorMotionStopSnapshot:
        return self.scope


def _stop_node(state: _StopState) -> MotionStopGroups:
    return MotionStopGroups(
        name="stop", parameters={}, value_sources={}, input_resolver=_Resolver({"groups": ["arm"]}),
        attempt_source=_Attempts(), operator_stop_state=state,
        execution_selections={"all": ("move", "stop"), "arm": ("move",)},
    )


def test_stop_motion_groups_succeeds_once_its_stop_has_completed() -> None:
    state = _StopState()
    node = _stop_node(state)
    assert _tick(node) is _RUNNING
    assert node.program_stop_request()[:2] == (1, ("arm",))
    node.note_program_stop_accepted(1, 3)
    assert _tick(node) is _RUNNING  # The scope read predates the accepted Stop.
    state.scope = OperatorMotionStopSnapshot(3, frozenset({"move"}), frozenset({"move"}))
    assert _tick(node) is _RUNNING  # Still stopping.
    state.scope = OperatorMotionStopSnapshot(3, frozenset({"move"}))
    assert _tick(node) is _SUCCESS


def test_stop_motion_groups_fails_when_a_stop_ended_uncontained() -> None:
    state = _StopState()
    node = _stop_node(state)
    _tick(node)
    node.note_program_stop_accepted(1, 3)
    state.scope = OperatorMotionStopSnapshot(4, frozenset({"move"}), faulted_nodes=frozenset({"move"}))
    assert _tick(node) is _FAILURE
    assert node.failure_reason_code == "motion_group_stop_failed"
