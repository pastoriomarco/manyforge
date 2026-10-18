"""io.set_output: one typed boolean write completed by source acceptance.

Fakes only: the native transport and the owner-task port (contract 53).
"""

from __future__ import annotations

from copy import deepcopy
from types import SimpleNamespace
from typing import Any, Mapping

import py_trees
import pytest

from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.nodes.stop_policy import (
    OperatorMotionStopSnapshot,
    OperatorMotionStopState,
)
from manyforge_behavior.nodes.v2_owner import IoOutputCommandAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationHeldError,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_behavior.production_ports import (
    IoSignalRefusedError,
    NativeKernelProgramPort,
    ProductionPortError,
)

_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64
_E = "sha256:" + "e" * 64
_SCHEMA = {"command_schema_id": "cell.valve.io_signal_write", "command_schema_revision": _E}


def _signal(role: str, name: str, direction: str) -> dict[str, str]:
    return {
        "resource_role_id": role,
        "signal_name": name,
        "value_type": "bool",
        "direction": direction,
        "transition_capability": "sampled_only",
        "signal_descriptor_digest": _E,
    }


def _context(*, schema: Any = _SCHEMA, tree: Mapping[str, Any] | None = None):
    target = {
        "target_role_id": "cell_io",
        "component_role_id": "io_module",
        "controllable_component_id": "io.1",
        "command_target_id": "io.command",
        "capability_bindings": [
            {"capability_id": "io.gpio.get_input", "command_schema": None},
            {"capability_id": "io.gpio.set_output", "command_schema": schema},
        ],
        "signal_role_bindings": [
            _signal("part_present", "di_0", "input"),
            _signal("valve", "do_0", "output"),
        ],
    }
    return ProgramRunPortContext(
        program_run_id="run.1",
        applied_record_digest=_D,
        scenario_id="io_scenario",
        source_document_digest=_A,
        program_id="io_program",
        program_revision=_A,
        program_binding_id=_A,
        retained_bundle_id=_B,
        graph_revision=_C,
        node_environment_digest=_D,
        program_binding={
            "program_binding_id": _A,
            "program_id": "io_program",
            "program_revision": _A,
            "retained_bundle_id": _B,
            "graph_revision": _C,
            "target_role_bindings": [target],
        },
        program=(
            {} if tree is None
            else {"program_id": "io_program", "program_revision": _A, "tree": tree}
        ),
    )


class _Transport:
    def __init__(self) -> None:
        self.submissions: list[Mapping[str, Any]] = []
        self.operations: dict[str, dict[str, Any]] = {}

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {
            "owner_epoch": "kernel.epoch.7", "graph_revision": _C, "retained_bundle_id": _B,
            "scene_revision": {"scene_id": _D, "revision_id": 4, "content_hash": _E},
        }}

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submissions.append(deepcopy(body))
        operation_id = f"operation-{len(self.submissions)}"
        self.operations[operation_id] = {
            "operation_id": operation_id, "operation_revision": 1,
            "program_run_id": body["program_run_id"], "state": "queued",
            "active_stop_generation": None, "participants": [],
        }
        return {"ok": True, "accepted": {"operation_id": operation_id, "operation_revision": 1}}

    def read(self, operation_id: str) -> dict[str, Any]:
        return {"ok": True, "operation": deepcopy(self.operations[operation_id]), "resident": True}

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        raise AssertionError("an output write is never reset by these tests")

    acknowledge_completed = stop = reset


def _identity(instance: str = "set-valve") -> NodeAttemptIdentity:
    return NodeAttemptIdentity("run.1", instance, f"attempt:{instance}")


def _inputs(value: Any = True, role: str = "valve") -> OwnerNodeInputs:
    return OwnerNodeInputs({"target_role": "cell_io", "resource_role": role, "value": value})


def _port(context: ProgramRunPortContext | None = None):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(context or _context())
    return port, transport


def _provider_participant(**extra: Any) -> dict[str, Any]:
    return {
        "participant_id": "p.1", "state": "completed", "terminal_error": None,
        "control_binding_id": None, "control_binding_revision": None,
        "capability_provider_binding_id": "io.1.provider", "capability_provider_binding_revision": _E,
        **extra,
    }


# --- Runner emission -------------------------------------------------------


@pytest.mark.parametrize("value", [True, False])
def test_write_uses_the_bound_sealed_schema_and_a_typed_boolean(value: bool) -> None:
    port, transport = _port()
    task = port.operations.start("io.set_output", _identity(), _inputs(value))
    assert task.state is OwnerTaskState.ACTIVE
    (submission,) = transport.submissions
    (participant,) = submission["participants"]
    assert participant["required_capability_id"] == "io.gpio.set_output"
    assert participant["target_role_id"] == "cell_io"
    assert participant["payload"] == {
        "kind": "capability_command",
        "command_schema_id": "cell.valve.io_signal_write",
        "command_schema_revision": _E,
        "command_value": {"kind": "io_signal_write", "resource_role_id": "valve", "value": value},
        "caller_options": [],
    }


@pytest.mark.parametrize(
    "context,inputs,error",
    [
        (_context(schema=None), _inputs(), "no sealed output command schema"),
        (_context(), _inputs(1), "must be a boolean"),
        (_context(), _inputs("true"), "must be a boolean"),
        (_context(), _inputs(role="part_present"), "not an output"),
        (_context(), _inputs(role="unknown"), "not bound to a signal"),
    ],
)
def test_write_refuses_before_any_effect(context, inputs, error) -> None:
    port, transport = _port(context)
    with pytest.raises(ProductionPortError, match=error):
        port.operations.start("io.set_output", _identity(), inputs)
    assert transport.submissions == []


def test_run_preparation_refuses_a_literal_input_role_for_an_output() -> None:
    tree = {
        "node_id": "io.set_output", "instance_id": "out", "children": [],
        "parameters": {"target_role": "cell_io", "resource_role": "part_present", "value": True},
    }
    with pytest.raises(IoSignalRefusedError, match="not an output") as refused:
        _port(_context(tree=tree))
    assert refused.value.code == "io_signal_direction_mismatch"


@pytest.mark.parametrize("evidence", sorted([
    "not_submitted", "submitted", "source_accepted", "source_rejected", "acceptance_unknown",
]))
def test_observation_carries_exact_output_evidence(evidence: str) -> None:
    port, transport = _port()
    task = port.operations.start("io.set_output", _identity(), _inputs())
    transport.operations[task.task_id]["participants"] = [
        _provider_participant(output_evidence=evidence)
    ]
    assert port.operations.observe(task.task_id).metadata["output_evidence"] == evidence


def test_absent_evidence_is_omitted() -> None:
    port, transport = _port()
    task = port.operations.start("io.set_output", _identity(), _inputs())
    transport.operations[task.task_id]["participants"] = [_provider_participant()]
    assert "output_evidence" not in port.operations.observe(task.task_id).metadata


@pytest.mark.parametrize(
    "participant",
    [
        _provider_participant(output_evidence=None),
        _provider_participant(output_evidence="feedback_observed"),
        _provider_participant(capability_provider_binding_id=None),
        _provider_participant(capability_provider_binding_revision=""),
        _provider_participant(control_binding_id="binding"),
        _provider_participant(control_binding_revision=_E),
    ],
)
def test_participant_reader_is_strict(participant: dict[str, Any]) -> None:
    port, transport = _port()
    task = port.operations.start("io.set_output", _identity(), _inputs())
    transport.operations[task.task_id]["participants"] = [participant]
    with pytest.raises(ProductionPortError):
        port.operations.observe(task.task_id)


def test_controller_participant_keeps_its_paired_binding() -> None:
    port, transport = _port()
    task = port.operations.start("io.set_output", _identity(), _inputs())
    transport.operations[task.task_id]["participants"] = [_provider_participant(
        control_binding_id="arm.binding", control_binding_revision=_E,
        capability_provider_binding_id=None, capability_provider_binding_revision=None,
        output_evidence="source_accepted",
    )]
    assert port.operations.observe(task.task_id).metadata["output_evidence"] == "source_accepted"


def test_resolver_qualifies_the_output_node_on_kernel_operations() -> None:
    resolver = build_executable_node_resolver(build_node_environment())
    (row,) = [item for item in resolver.dispositions if item.row.node_id == "io.set_output"]
    assert row.adapter_build_qualified is True
    assert row.owner_port_name == "kernel_operations"
    assert row.unavailable_reason_code is None


# --- Adapter completion ----------------------------------------------------


class _Port:
    """Owner-task port fake: one scripted terminal snapshot per attempt."""

    def __init__(self, terminal: OwnerTaskSnapshot | None = None, *, held: int = 0) -> None:
        self.terminal = terminal
        self.held = held
        self.starts: list[str] = []
        self.acknowledged: list[str] = []
        self.closed: list[str] = []
        self.cancelled: list[str] = []

    def start(self, node_id, identity, inputs):
        if self.held:
            self.held -= 1
            raise OwnerInvocationHeldError(1)
        self.starts.append(identity.node_attempt_id)
        return OwnerTaskSnapshot(task_id="op.1", state=OwnerTaskState.ACTIVE,
                                 metadata={"operation_id": "op.1"})

    def observe(self, task_id):
        assert self.terminal is not None
        return self.terminal

    def acknowledge(self, task_id, identity):
        self.acknowledged.append(task_id)

    def close(self, task_id, identity, *, request_id):
        self.closed.append(task_id)
        return True

    def cancel(self, task_id, identity):
        self.cancelled.append(task_id)


def _terminal(state: OwnerTaskState, evidence: str | None, **metadata: Any) -> OwnerTaskSnapshot:
    return OwnerTaskSnapshot(
        task_id="op.1",
        state=state,
        metadata={
            "operation_id": "op.1",
            **({} if evidence is None else {"output_evidence": evidence}),
            **metadata,
        },
        reason_code=None if state is OwnerTaskState.SUCCEEDED else "command_failed",
        message="" if state is OwnerTaskState.SUCCEEDED else "output write failed",
    )


def _adapter(port: _Port, stop_state: OperatorMotionStopState | None = None):
    adapter = IoOutputCommandAdapter(
        name="set-valve", node_id="io.set_output", node_instance_id="set-valve",
        port=port, owner_port_name="kernel_operations",
        parameters=dict(_inputs().parameters), value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={},
        )),
        operator_stop_state=stop_state,
    )
    adapter.setup_with_descendants()
    return adapter


def _tick(adapter, count: int = 1) -> py_trees.common.Status:
    """Tick like a parent would: a settled leaf is never re-entered here."""
    for _ in range(count):
        adapter.tick_once()
        if adapter.status is not py_trees.common.Status.RUNNING:
            break
    return adapter.status


@pytest.mark.parametrize("evidence", ["source_accepted", "submitted"])
def test_source_acceptance_completes_the_write(evidence: str) -> None:
    port = _Port(_terminal(OwnerTaskState.SUCCEEDED, evidence))
    adapter = _adapter(port)
    assert _tick(adapter, 2) is py_trees.common.Status.SUCCESS
    assert port.starts == ["attempt:set-valve"]
    assert port.acknowledged == ["op.1"]


@pytest.mark.parametrize("evidence", [None, "acceptance_unknown", "not_submitted"])
def test_completion_without_acceptance_is_never_a_success(evidence) -> None:
    port = _Port(_terminal(OwnerTaskState.SUCCEEDED, evidence))
    adapter = _adapter(port)
    assert _tick(adapter, 3) is py_trees.common.Status.FAILURE
    assert adapter.failure_reason_code == "io_output_acceptance_unconfirmed"
    # Retained for Reset: neither acknowledged nor resent.
    assert port.acknowledged == [] and port.starts == ["attempt:set-valve"]
    assert adapter.cached_control_state() == "unrecoverable"


@pytest.mark.parametrize("evidence", ["source_rejected", "not_submitted"])
def test_rejected_or_unsubmitted_write_fails_and_closes(evidence: str) -> None:
    assessment = {"execution_state": "Failed", "eligible_close": True, "status": "retry_ready"}
    port = _Port(_terminal(
        OwnerTaskState.FAILED, evidence,
        operation_state="failed", recovery_assessment=assessment,
    ))
    adapter = _adapter(port)
    assert _tick(adapter, 2) is py_trees.common.Status.FAILURE
    assert adapter.failure_reason_code == "command_failed"
    assert port.closed == ["op.1"] and port.starts == ["attempt:set-valve"]


@pytest.mark.parametrize(
    ("operation_state", "execution_state"),
    [("intervention_required", "InterventionRequired"), ("failed", "Failed")],
)
def test_unknown_acceptance_is_an_uncertain_effect_never_resent(
    operation_state: str, execution_state: str,
) -> None:
    # Outer operation state is lowercase; the participant assessment carries
    # Core's TitleCase ExecutionState spelling.
    assessment = {"execution_state": execution_state, "eligible_close": False}
    port = _Port(_terminal(
        OwnerTaskState.FAILED, "acceptance_unknown",
        operation_state=operation_state, recovery_assessment=assessment,
    ))
    adapter = _adapter(port)
    assert _tick(adapter, 4) is py_trees.common.Status.FAILURE
    # No close (uncertain effect stays retained for intervention), no resend.
    assert port.closed == [] and port.acknowledged == []
    assert port.starts == ["attempt:set-valve"]


def test_held_node_sends_nothing_until_released() -> None:
    state = OperatorMotionStopState({"set-valve": ()})
    state.publish(OperatorMotionStopSnapshot(1, frozenset({"set-valve"})))
    port = _Port(_terminal(OwnerTaskState.SUCCEEDED, "source_accepted"), held=1)
    adapter = _adapter(port, state)
    assert _tick(adapter, 4) is py_trees.common.Status.RUNNING
    assert port.starts == [] and adapter.presented_held
    state.publish(OperatorMotionStopSnapshot(2, frozenset()))
    assert _tick(adapter, 2) is py_trees.common.Status.SUCCESS
    assert len(port.starts) == 1 and port.acknowledged == ["op.1"]
