"""io.set_output: one write through the one write path.

The node makes the same host call as a manual write (`set_io_output` by
signal id), once: `succeeded` → success; `failed`/`unknown` → failure with
the host's code; an admission refusal → failure with its code. Never resent.
Fakes only: the native transport and the owner-invocation port.
"""

from __future__ import annotations

from types import SimpleNamespace
from typing import Any, Mapping

import py_trees
import pytest

from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.kernel_client import KernelClient, KernelClientResponseError
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.nodes.v2_owner import OwnerInvocationAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerInvocationResult,
    OwnerNodeInputs,
    ProgramRunPortContext,
)
from manyforge_behavior.production_ports import (
    IoSignalRefusedError,
    NativeKernelProgramPort,
    ProductionPortError,
)

from .v3_fixtures import signal, target, target_binding

_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64
_E = "sha256:" + "e" * 64


def _signal_row(name: str, direction: str) -> dict[str, Any]:
    return {
        "reference": signal("io.1", name),
        "target": target("io.1", "io.command"),
        "value_type": "bool",
        "direction": direction,
        "transition_capability": "sampled_only",
        "signal_descriptor_digest": _E,
    }


def _context(*, tree: Mapping[str, Any] | None = None):
    io_target = target_binding("io.1", "io.command", [
        {"capability_id": "io.gpio.get_input", "command_schema": None},
        {"capability_id": "io.gpio.set_output", "command_schema": None},
    ])
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
            "target_bindings": [io_target],
            "signal_bindings": [
                _signal_row("di_0", "input"),
                _signal_row("do_0", "output"),
            ],
        },
        program=(
            {} if tree is None
            else {"program_id": "io_program", "program_revision": _A, "tree": tree}
        ),
    )


class _Refused(RuntimeError):
    """The transport's typed admission refusal (`ok: false`)."""

    def __init__(self, code: str) -> None:
        super().__init__(f"refused: {code}")
        self.code = code


class _Transport:
    def __init__(self, reply: Mapping[str, Any] | Exception | None = None) -> None:
        self.writes: list[tuple[str, Any]] = []
        self.submissions: list[Mapping[str, Any]] = []
        self.reply = reply or {"outcome": "succeeded"}

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {
            "owner_epoch": "kernel.epoch.7", "graph_revision": _C, "retained_bundle_id": _B,
            "scene_revision": {"scene_id": _D, "revision_id": 4, "content_hash": _E},
        }}

    def set_io_output(self, signal_id: str, value: Any, *, origin: str) -> dict[str, Any]:
        assert origin == "program"
        self.writes.append((signal_id, value))
        if isinstance(self.reply, Exception):
            raise self.reply
        return {"ok": True, "command": "set_io_output", "result": dict(self.reply)}

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submissions.append(body)
        raise AssertionError("an output write is never a Kernel operation")

    read = reset = acknowledge_completed = stop = submit


def _identity(instance: str = "set-valve") -> NodeAttemptIdentity:
    return NodeAttemptIdentity("run.1", instance, f"attempt:{instance}")


def _inputs(value: Any = True, signal_name: str = "do_0") -> OwnerNodeInputs:
    return OwnerNodeInputs({"signal": signal("io.1", signal_name), "value": value})


def _port(transport: _Transport | None = None, context: ProgramRunPortContext | None = None):
    transport = transport or _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(context or _context())
    return port, transport


# --- Port: one host call by signal id ---------------------------------------


@pytest.mark.parametrize("value", [True, False])
def test_success_writes_the_bound_signal_once(value: bool) -> None:
    port, transport = _port()
    result = port.queries.invoke("io.set_output", _identity(), _inputs(value))
    assert result.succeeded
    assert transport.writes == [("io.1/do_0", value)]
    assert transport.submissions == []


@pytest.mark.parametrize("outcome,code", [
    ("failed", "io_output_write_failed"),
    ("unknown", "io_output_acceptance_unknown"),
])
def test_failed_or_unknown_result_fails_with_the_host_code_never_resent(outcome, code) -> None:
    port, transport = _port(_Transport(
        {"outcome": outcome, "error": {"code": code, "message": "the device said so"}}
    ))
    result = port.queries.invoke("io.set_output", _identity(), _inputs())
    assert not result.succeeded
    assert result.reason_code == code
    assert "the device said so" in result.message
    assert transport.writes == [("io.1/do_0", True)]


@pytest.mark.parametrize("code", ["cell_latched", "rule_holds"])
def test_admission_refusal_fails_with_its_code(code: str) -> None:
    port, transport = _port(_Transport(_Refused(code)))
    result = port.queries.invoke("io.set_output", _identity(), _inputs())
    assert not result.succeeded
    assert result.reason_code == code
    assert len(transport.writes) == 1


def test_unknown_result_without_a_typed_error_is_an_invalid_reply() -> None:
    port, transport = _port(_Transport({"outcome": "unknown"}))
    with pytest.raises(KernelClientResponseError):
        port.queries.invoke("io.set_output", _identity(), _inputs())
    assert len(transport.writes) == 1


@pytest.mark.parametrize("inputs,error", [
    (_inputs(1), "must be a boolean"),
    (_inputs("true"), "must be a boolean"),
])
def test_non_boolean_value_is_refused_before_any_write(inputs, error) -> None:
    port, transport = _port()
    with pytest.raises(ProductionPortError, match=error):
        port.queries.invoke("io.set_output", _identity(), inputs)
    assert transport.writes == []


@pytest.mark.parametrize("signal_name,code", [
    ("di_0", "io_signal_direction_mismatch"),
    ("unknown", "io_signal_unbound"),
])
def test_unwritable_signal_fails_before_any_write(signal_name: str, code: str) -> None:
    port, transport = _port()
    result = port.queries.invoke("io.set_output", _identity(), _inputs(signal_name=signal_name))
    assert not result.succeeded and result.reason_code == code
    assert transport.writes == []


def test_run_preparation_refuses_a_literal_input_signal_for_an_output() -> None:
    tree = {
        "node_id": "io.set_output", "instance_id": "out", "children": [],
        "parameters": {"signal": signal("io.1", "di_0"), "value": True},
    }
    with pytest.raises(IoSignalRefusedError, match="not an output") as refused:
        _port(context=_context(tree=tree))
    assert refused.value.code == "io_signal_direction_mismatch"


def test_resolver_qualifies_the_output_node_as_an_immediate_invocation() -> None:
    resolver = build_executable_node_resolver(build_node_environment())
    (row,) = [item for item in resolver.dispositions if item.row.node_id == "io.set_output"]
    assert row.adapter_build_qualified is True
    assert row.owner_port_name == "io_signals"
    assert row.unavailable_reason_code is None


# --- Adapter: one call per attempt ------------------------------------------


class _Port:
    def __init__(self, result: OwnerInvocationResult | Exception) -> None:
        self.result = result
        self.calls: list[str] = []

    def invoke(self, node_id, identity, inputs):
        self.calls.append(node_id)
        if isinstance(self.result, Exception):
            raise self.result
        return self.result


def _adapter(port: _Port) -> OwnerInvocationAdapter:
    adapter = OwnerInvocationAdapter(
        name="set-valve", node_id="io.set_output", node_instance_id="set-valve",
        port=port, parameters=dict(_inputs().parameters), value_sources={},
        attempt_source=SimpleNamespace(begin=lambda node: _identity(node)),
        input_resolver=SimpleNamespace(snapshot=lambda **kwargs: SimpleNamespace(
            parameters=kwargs["parameters"], blackboard={},
        )),
    )
    adapter.setup_with_descendants()
    return adapter


@pytest.mark.parametrize("result,status,code", [
    (OwnerInvocationResult(True), py_trees.common.Status.SUCCESS, None),
    (OwnerInvocationResult(False, reason_code="io_output_acceptance_unknown", message="unknown"),
     py_trees.common.Status.FAILURE, "io_output_acceptance_unknown"),
    (RuntimeError("socket closed"), py_trees.common.Status.FAILURE, "owner_request_failed"),
])
def test_adapter_settles_on_the_one_call(result, status, code) -> None:
    port = _Port(result)
    adapter = _adapter(port)
    adapter.tick_once()
    assert adapter.status is status
    assert port.calls == ["io.set_output"]
    if code is not None:
        assert adapter.failure_reason_code == code
