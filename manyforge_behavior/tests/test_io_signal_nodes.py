"""Boolean input reads and level waits (I/O slice 1) over the native owner.

Fakes only: the native transport, the watch port and the Program clock.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Mapping

import py_trees
import pytest

from manyforge_behavior.canonical_artifact import (
    compute_content_identity,
    load_strict_json_yaml,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.kernel_client import KernelClient
from manyforge_behavior.nodes.v2_owner import IoInputWaitAdapter
from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)
from manyforge_behavior.production_ports import (
    IoSignalRefusedError,
    NativeKernelProgramPort,
)
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_behavior.program_clock import ProgramClock


_ROOT = Path(__file__).resolve().parents[2]
_A = "sha256:" + "a" * 64
_B = "sha256:" + "b" * 64
_C = "sha256:" + "c" * 64
_D = "sha256:" + "d" * 64
_E = "sha256:" + "e" * 64


class _OwnerError(RuntimeError):
    def __init__(self, code: str, details: Mapping[str, Any] | None = None) -> None:
        super().__init__(code)
        self.code = code
        self.details = dict(details or {})


def _signal_row(**overrides: str) -> dict[str, str]:
    return {
        "resource_role_id": "part_present",
        "signal_name": "di_0",
        "value_type": "bool",
        "direction": "input",
        "transition_capability": "sampled_only",
        "signal_descriptor_digest": _E,
        **overrides,
    }


def _context(signal_rows=None, *, coordinate_roles=(), tree=None) -> ProgramRunPortContext:
    target: dict[str, Any] = {
        "target_role_id": "cell_io",
        "component_role_id": "io_module",
        "controllable_component_id": "io.1",
        "command_target_id": "io.command",
        "capability_bindings": [
            {"capability_id": "io.gpio.get_input", "command_schema": None},
        ],
        "resource_role_bindings": [
            {"resource_role_id": role, "resource_ids": [], "compiled_resource_names": []}
            for role in coordinate_roles
        ],
    }
    if signal_rows is not None:
        target["signal_role_bindings"] = signal_rows
    program = {}
    if tree is not None:
        program = {"program_id": "io_program", "program_revision": _A, "tree": tree}
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
        program=program,
    )


def _value(quality: str = "fresh", value: bool | None = True) -> dict[str, Any]:
    fresh = quality == "fresh"
    return {
        "liveness": {"mode": "in_process", "warning": None},
        "signal_name": "di_0",
        "value_type": "bool",
        "value": value,
        "quality": quality,
        "source_id": "mock.io",
        "source_epoch": "1" if fresh else None,
        "source_sequence": "9" if fresh else None,
        "receipt_age_ns": 1000 if fresh else None,
        "continuity_loss_generation": "0",
        "error": None,
    }


class _Transport:
    """The native owner's command socket, as the Runner client sees it."""

    def __init__(self) -> None:
        self.value = _value()
        self.query_error: Exception | None = None
        self.queries: list[Mapping[str, Any]] = []
        self.registrations: list[Mapping[str, Any]] = []
        self.reads: list[Mapping[str, Any]] = []
        self.releases: list[Mapping[str, Any]] = []
        self.watch_state = "pending"
        self.watch_quality = "fresh"
        self.read_error: Exception | None = None
        self.release_error: Exception | None = None
        self.register_error: Exception | None = None
        self.scope_releases: list[Mapping[str, Any]] = []
        self.scope_release_error: Exception | None = None

    def identity(self) -> dict[str, Any]:
        return {
            "ok": True,
            "identity": {
                "owner_epoch": "kernel.epoch.7",
                "graph_revision": _C,
                "retained_bundle_id": _B,
                "scene_revision": {"scene_id": _D, "revision_id": 1, "content_hash": _E},
            },
        }

    def submit(self, body):  # pragma: no cover - never used by I/O reads
        raise AssertionError("an I/O read or watch never submits an operation")

    read = reset = acknowledge_completed = stop = submit

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "scope": {
                "schema_version": "manyforge.program_execution_scope.v2",
                "program_run_id": program_run_id,
                "request_owner_epoch": "kernel.epoch.7",
                "program_runner_owner_epoch": "runner.epoch.3",
                "claim_generation": 5,
                "selection_revision": 0,
            },
        }

    def query_target(self, query: Mapping[str, Any]) -> dict[str, Any]:
        self.queries.append(deepcopy(dict(query)))
        if self.query_error is not None:
            raise self.query_error
        return {
            "ok": True,
            "target_observation": {
                **query,
                "schema_version": "manyforge.target_observation.v1",
                "value": deepcopy(self.value),
            },
        }

    def _result(self) -> dict[str, Any]:
        if self.watch_state == "released":
            # Core erased the watch: the actual reply carries neither.
            return {"state": "released", "observation": None, "matched_sample": None}
        matched = self.watch_state == "matched"
        return {
            "state": self.watch_state,
            "observation": _value(self.watch_quality, True),
            "matched_sample": {
                "controllable_component_id": "io.1",
                "signal_name": "di_0",
                "source_id": "mock.io",
                "source_epoch": "1",
                "source_sequence": "10",
                "value": True,
                "received_at_ns": 55,
            } if matched else None,
        }

    def register_io_watch(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.registrations.append(deepcopy(dict(body)))
        if self.register_error is not None:
            raise self.register_error
        return {
            "registration": {
                "owner_epoch": body["owner_epoch"],
                "catalog_generation": "1",
                "registration_id": str(len(self.registrations)),
                "scope": {
                    "program_runner_owner_epoch": body["program_runner_owner_epoch"],
                    "program_run_id": body["program_run_id"],
                    "node": {
                        "node_instance_id": body["node_instance_id"],
                        "node_attempt_id": body["node_attempt_id"],
                    },
                },
            },
            "result": self._result(),
        }

    def read_io_watch(self, registration: Mapping[str, Any]) -> dict[str, Any]:
        self.reads.append(deepcopy(dict(registration)))
        if self.read_error is not None:
            raise self.read_error
        return self._result()

    def release_io_watch(self, registration: Mapping[str, Any]) -> None:
        if self.release_error is not None:
            raise self.release_error
        self.releases.append(deepcopy(dict(registration)))

    def release_io_watch_scope(self, scope_request: Mapping[str, Any]) -> None:
        self.scope_releases.append(deepcopy(dict(scope_request)))
        if self.scope_release_error is not None:
            raise self.scope_release_error


def _bound(context: ProgramRunPortContext | None = None):
    transport = _Transport()
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(context or _context([_signal_row()]))
    return transport, port


def _identity(instance: str = "io_node", attempt: str = "attempt.1") -> NodeAttemptIdentity:
    return NodeAttemptIdentity("run.1", instance, attempt)


def _read(port, node_id: str, **parameters):
    return port.queries.invoke(
        node_id,
        _identity(),
        OwnerNodeInputs(
            parameters={"target_role": "cell_io", "resource_role": "part_present", **parameters}
        ),
    )


# --- Catalog and resolver ---------------------------------------------------


def test_reads_waits_and_outputs_are_executable() -> None:
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    assert catalog.content_version == "2.7.0"
    timeout = next(
        row for row in catalog.get("io.wait_for_input").parameters if row.id == "timeout_s"
    )
    assert timeout.required is False and timeout.has_default is False


# --- get_input / check_input ------------------------------------------------


@pytest.mark.parametrize("value", [True, False])
def test_fresh_get_input_returns_the_exact_boolean(value: bool) -> None:
    transport, port = _bound()
    transport.value = _value("fresh", value)
    result = _read(port, "io.get_input", output_key="part")
    assert result.succeeded is True
    assert result.output is value
    assert result.metadata["quality"] == "fresh"
    assert transport.queries == [{
        "schema_version": "manyforge.target_observation_query.v1",
        "owner_epoch": "kernel.epoch.7",
        "program_binding_id": _A,
        "target_role_id": "cell_io",
        "kind": "io_signal",
        "resource_role_id": "part_present",
    }]


@pytest.mark.parametrize(
    ("observed", "expected", "succeeded"),
    [(True, True, True), (False, False, True), (True, False, False), (False, True, False)],
)
def test_fresh_check_input_compares_once(observed, expected, succeeded) -> None:
    transport, port = _bound()
    transport.value = _value("fresh", observed)
    result = _read(port, "io.check_input", expected_value=expected)
    assert result.succeeded is succeeded
    assert result.reason_code == (None if succeeded else "io_input_not_matched")


@pytest.mark.parametrize("node_id", ["io.get_input", "io.check_input"])
@pytest.mark.parametrize("quality", ["missing", "stale", "invalid", "unavailable"])
def test_non_fresh_quality_fails_and_is_never_false(node_id: str, quality: str) -> None:
    transport, port = _bound()
    # A stale reading keeps its last value for diagnosis; it is never a read.
    transport.value = _value(quality, False if quality == "stale" else None)
    result = _read(port, node_id, output_key="part", expected_value=False)
    assert result.succeeded is False
    assert result.output is None
    assert result.reason_code == "io_input_unavailable"
    assert result.details["reason"] == "io_input_unavailable"
    assert result.details["reference_id"] == "part_present"
    assert quality in result.message


def test_uninstalled_signal_service_fails_with_its_reason() -> None:
    transport, port = _bound()
    transport.query_error = _OwnerError(
        "unsupported_capability", {"reason": "io_signal_service_unavailable"}
    )
    result = _read(port, "io.get_input", output_key="part")
    assert result.succeeded is False
    assert result.reason_code == "io_signal_service_unavailable"


def test_a_transport_failure_is_not_turned_into_an_input_value() -> None:
    transport, port = _bound()
    transport.query_error = _OwnerError("execution_owner_unavailable")
    with pytest.raises(_OwnerError):
        _read(port, "io.get_input", output_key="part")


# --- Binding refusals -------------------------------------------------------


@pytest.mark.parametrize(
    ("rows", "coordinate_roles", "code"),
    [
        (None, (), "io_signal_unbound"),
        ([], (), "io_signal_unbound"),
        ([_signal_row(resource_role_id="other")], (), "io_signal_unbound"),
        ([_signal_row(), _signal_row(signal_name="di_1")], (), "io_signal_unbound"),
        ([_signal_row()], ("part_present",), "io_signal_unbound"),
        ([_signal_row(direction="output")], (), "io_signal_direction_mismatch"),
        ([_signal_row(value_type="int")], (), "io_signal_type_mismatch"),
        ([_signal_row(transition_capability="edge_only")], (), "io_signal_transition_unsupported"),
    ],
)
def test_signal_references_refuse_at_run_preparation(rows, coordinate_roles, code) -> None:
    tree = {
        "node_id": "sequence",
        "instance_id": "main",
        "parameters": {},
        "children": [{
            "node_id": "io.check_input",
            "instance_id": "check",
            "parameters": {
                "target_role": "cell_io",
                "resource_role": "part_present",
                "expected_value": True,
            },
            "children": [],
        }],
    }
    port = NativeKernelProgramPort(KernelClient(_Transport()))
    with pytest.raises(IoSignalRefusedError) as refused:
        port.bind(_context(rows, coordinate_roles=coordinate_roles, tree=tree))
    assert refused.value.code == code
    assert str(refused.value).startswith(code)
    # Without the literal node the binding binds; the read refuses the same way.
    transport, port = _bound(_context(rows, coordinate_roles=coordinate_roles))
    result = _read(port, "io.check_input", expected_value=True)
    assert result.reason_code == code
    assert transport.queries == []


def test_an_edge_wait_is_refused_at_run_preparation() -> None:
    tree = {
        "node_id": "io.wait_for_input",
        "instance_id": "wait",
        "parameters": {
            "target_role": "cell_io",
            "resource_role": "part_present",
            "expected_value": True,
            "edge": "rising",
        },
        "children": [],
    }
    port = NativeKernelProgramPort(KernelClient(_Transport()))
    with pytest.raises(IoSignalRefusedError) as refused:
        port.bind(_context([_signal_row()], tree=tree))
    assert refused.value.code == "io_watch_condition_unsupported"


def _wait_program(edge: str | None, *, timeout_s: float | None = None) -> dict[str, Any]:
    document = load_strict_json_yaml(_ROOT / "examples/pick_and_place_minimal.program.yaml")
    arm = document["target_roles"]["arm"]
    arm["required_capabilities"] = sorted(
        [
            *arm["required_capabilities"],
            {"capability_id": "io.gpio.wait_for_input", "parameters": {}, "disposition": "required"},
        ],
        key=lambda row: row["capability_id"],
    )
    parameters: dict[str, Any] = {
        "target_role": "arm",
        "resource_role": "arm_joints",
        "expected_value": True,
    }
    if edge is not None:
        parameters["edge"] = edge
    if timeout_s is not None:
        parameters["timeout_s"] = timeout_s
    document["tree"] = {
        "node_id": "io.wait_for_input",
        "instance_id": "wait_part",
        "parameters": parameters,
        "children": [],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return document


@pytest.mark.parametrize("edge", ["rising", "falling"])
def test_program_validation_refuses_edge_waits(edge: str) -> None:
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    with pytest.raises(ProgramValidationError, match="io_watch_condition_unsupported"):
        Program.from_dict(_wait_program(edge), catalog)


@pytest.mark.parametrize(("edge", "timeout_s"), [(None, None), ("level", 2.5)])
def test_program_validation_accepts_level_waits(edge, timeout_s) -> None:
    catalog = NodeCatalog(_ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    Program.from_dict(_wait_program(edge, timeout_s=timeout_s), catalog)


# --- Native watch port ------------------------------------------------------


def _wait_inputs(**parameters) -> OwnerNodeInputs:
    return OwnerNodeInputs(parameters={
        "target_role": "cell_io",
        "resource_role": "part_present",
        "expected_value": True,
        "edge": "level",
        **parameters,
    })


def test_watch_registers_exact_attempt_reads_and_releases_the_exact_token() -> None:
    transport, port = _bound()
    identity = _identity()
    task = port.io_watches.start("io.wait_for_input", identity, _wait_inputs())
    assert task.state is OwnerTaskState.ACTIVE
    assert transport.registrations == [{
        "schema_version": "manyforge.io_watch_request.v1",
        "owner_epoch": "kernel.epoch.7",
        "program_binding_id": _A,
        "program_runner_owner_epoch": "runner.epoch.3",
        "claim_generation": 5,
        "program_run_id": "run.1",
        "node_instance_id": "io_node",
        "node_attempt_id": "attempt.1",
        "target_role_id": "cell_io",
        "resource_role_id": "part_present",
        "condition": {"kind": "level", "expected_value": True},
    }]
    token = transport.register_io_watch(transport.registrations[0])["registration"]
    transport.watch_state = "matched"
    matched = port.io_watches.observe(task.task_id)
    assert matched.state is OwnerTaskState.SUCCEEDED
    assert matched.metadata["matched_sample"]["source_sequence"] == "10"
    assert transport.reads[-1]["scope"] == token["scope"]
    port.io_watches.acknowledge(task.task_id, identity)
    port.io_watches.acknowledge(task.task_id, identity)  # Idempotent.
    assert len(transport.releases) == 1
    assert transport.releases[0]["registration_id"] == "1"


def test_watch_read_transport_failure_keeps_the_watch() -> None:
    transport, port = _bound()
    task = port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs())
    transport.read_error = _OwnerError("execution_owner_unavailable")
    with pytest.raises(OwnerTaskObservationUnavailableError):
        port.io_watches.observe(task.task_id)
    assert transport.releases == []
    transport.read_error = None
    assert port.io_watches.observe(task.task_id).state is OwnerTaskState.ACTIVE


def test_host_released_watch_fails_the_attempt() -> None:
    transport, port = _bound()
    task = port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs())
    transport.watch_state = "released"
    released = port.io_watches.observe(task.task_id)
    assert released.state is OwnerTaskState.FAILED
    assert released.reason_code == "io_watch_released"


def test_watch_port_refuses_an_edge_condition() -> None:
    transport, port = _bound()
    with pytest.raises(IoSignalRefusedError) as refused:
        port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs(edge="falling"))
    assert refused.value.code == "io_watch_condition_unsupported"
    assert transport.registrations == []


def test_run_teardown_releases_remaining_watches_even_after_a_failed_release() -> None:
    transport, port = _bound()
    first = _identity("wait_a", "attempt.a")
    port.io_watches.start("io.wait_for_input", first, _wait_inputs())
    port.io_watches.start("io.wait_for_input", _identity("wait_b", "attempt.b"), _wait_inputs())
    transport.release_error = _OwnerError("execution_owner_unavailable")
    with pytest.raises(_OwnerError):
        port.io_watches.cancel(
            next(iter(port._io_watches)), first
        )
    assert len(port._io_watches) == 2  # An unconfirmed release keeps the token.
    transport.release_error = None
    port.unbind("run.1")
    assert sorted(row["registration_id"] for row in transport.releases) == ["1", "2"]
    assert port._io_watches == {}


_SCOPE_RELEASE = {
    "schema_version": "manyforge.io_watch_scope_release.v1",
    "owner_epoch": "kernel.epoch.7",
    "program_binding_id": _A,
    "program_runner_owner_epoch": "runner.epoch.3",
    "claim_generation": 5,
    "program_run_id": "run.1",
    "node_instance_id": "io_node",
    "node_attempt_id": "attempt.1",
}


@pytest.mark.parametrize(
    "unknown", ["execution_owner_unavailable", "execution_command_response_invalid"]
)
def test_unknown_registration_releases_the_exact_attempt_scope(unknown: str) -> None:
    transport, port = _bound()
    transport.register_error = _OwnerError(unknown)
    with pytest.raises(_OwnerError):
        port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs())
    assert transport.scope_releases == [_SCOPE_RELEASE]
    assert set(_SCOPE_RELEASE) <= set(transport.registrations[0])
    assert transport.releases == [] and port._io_watches == {}


def test_unknown_scope_release_is_left_to_run_teardown() -> None:
    transport, port = _bound()
    transport.register_error = _OwnerError("execution_owner_unavailable")
    transport.scope_release_error = _OwnerError("execution_owner_unavailable")
    with pytest.raises(_OwnerError) as failed:
        port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs())
    assert failed.value is transport.register_error  # The original failure.
    assert len(transport.scope_releases) == 1  # Sent once, never retried.
    port.unbind("run.1")
    assert len(transport.scope_releases) == 1 and transport.releases == []


def test_a_host_refused_registration_sends_no_scope_release() -> None:
    transport, port = _bound()
    transport.register_error = _OwnerError(
        "stale_generation", {"reason": "io_watch_generation_stale"}
    )
    with pytest.raises(_OwnerError):
        port.io_watches.start("io.wait_for_input", _identity(), _wait_inputs())
    assert transport.scope_releases == []


# --- Wait adapter -----------------------------------------------------------


class _WatchPort:
    def __init__(self) -> None:
        self.state = OwnerTaskState.ACTIVE
        self.quality = "fresh"
        self.calls: list[tuple[str, ...]] = []
        self.observe_error: Exception | None = None
        self.start_error: Exception | None = None
        self.started = 0
        self.during_call: Any = None  # Runs inside start/observe (e.g. time passes).

    def _snapshot(self, task_id: str) -> OwnerTaskSnapshot:
        failed = self.state is OwnerTaskState.FAILED
        return OwnerTaskSnapshot(
            task_id,
            self.state,
            metadata={"quality": self.quality},
            reason_code="io_watch_released" if failed else None,
        )

    def start(self, node_id, identity, inputs):
        self.calls.append(("start", identity.node_attempt_id))
        if self.during_call is not None:
            self.during_call()
        if self.start_error is not None:
            raise self.start_error
        self.started += 1
        return self._snapshot(f"watch.{self.started}")

    def observe(self, task_id):
        self.calls.append(("observe", task_id))
        if self.during_call is not None:
            self.during_call()
        if self.observe_error is not None:
            raise self.observe_error
        return self._snapshot(task_id)

    def cancel(self, task_id, identity):
        self.calls.append(("cancel", task_id))

    def acknowledge(self, task_id, identity):
        self.calls.append(("acknowledge", task_id))

    def releases(self) -> list[tuple[str, ...]]:
        return [call for call in self.calls if call[0] in {"cancel", "acknowledge"}]


def _waiter(timeout_s: float | None = None, *, port: Any = None):
    clock = [100.0]
    program_clock = ProgramClock(lambda: clock[0])
    parameters: dict[str, Any] = {
        "target_role": "cell_io",
        "resource_role": "part_present",
        "expected_value": True,
        "edge": "level",
    }
    if timeout_s is not None:
        parameters["timeout_s"] = timeout_s
    attempts = iter(f"attempt.{index}" for index in range(1, 100))
    events: list[dict[str, Any]] = []
    port = _WatchPort() if port is None else port
    node = IoInputWaitAdapter(
        name="wait",
        node_id="io.wait_for_input",
        node_instance_id="wait",
        attempt_source=SimpleNamespace(
            begin=lambda _: NodeAttemptIdentity("run.1", "wait", next(attempts))
        ),
        parameters=parameters,
        value_sources={},
        input_resolver=SimpleNamespace(
            snapshot=lambda **_: SimpleNamespace(parameters=parameters, blackboard={})
        ),
        port=port,
        program_clock=program_clock,
        audit_sink=events.append,
        cancellable=True,
    )
    return node, port, clock, program_clock, events


def test_wait_pending_then_matched_succeeds_and_releases_once() -> None:
    node, port, _clock, _program_clock, _events = _waiter()
    for _ in range(5):
        node.tick_once()
        assert node.status is py_trees.common.Status.RUNNING
    port.state = OwnerTaskState.SUCCEEDED
    node.tick_once()
    assert node.status is py_trees.common.Status.SUCCESS
    # One watch, read on every tick (the tree's own cadence), released once.
    assert [call[0] for call in port.calls].count("start") == 1
    assert [call[0] for call in port.calls].count("observe") == 5
    assert port.releases() == [("acknowledge", "watch.1")]


def test_unavailable_source_stays_running_and_visible() -> None:
    node, port, _clock, _program_clock, events = _waiter()
    port.quality = "stale"
    for _ in range(3):
        node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert "input unavailable (stale)" in node.feedback_message
    port.quality = "fresh"
    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert [event["kind"] for event in events] == [
        "program_node_io_source_unavailable",
        "program_node_io_source_available",
    ]
    assert events[0]["quality"] == "stale"
    assert port.releases() == []


def test_retry_after_an_unavailable_attempt_records_the_source_available() -> None:
    node, port, _clock, _program_clock, events = _waiter()
    port.quality = "stale"
    node.tick_once()
    assert [event["kind"] for event in events] == ["program_node_io_source_unavailable"]
    node.stop(py_trees.common.Status.INVALID)  # The attempt ends while the source is unavailable.
    assert port.releases() == [("cancel", "watch.1")]
    port.quality = "fresh"
    node.tick_once()  # The retry starts; its first read is fresh.
    assert node.status is py_trees.common.Status.RUNNING
    assert [event["kind"] for event in events] == [
        "program_node_io_source_unavailable",
        "program_node_io_source_available",
    ]
    assert events[1]["node_instance_id"] == "wait"
    assert events[1]["node_attempt_id"] == "attempt.2"
    assert events[1]["quality"] is None
    assert port.releases() == [("cancel", "watch.1")]


def test_wait_without_timeout_waits_indefinitely() -> None:
    node, port, clock, _program_clock, _events = _waiter()
    for _ in range(50):
        clock[0] += 3600.0
        node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert port.releases() == []


def test_authored_timeout_fails_and_releases_the_exact_watch() -> None:
    node, port, clock, program_clock, _events = _waiter(timeout_s=2.0)
    node.tick_once()
    program_clock.freeze()  # A Halt: Program time stands still.
    clock[0] += 60.0
    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    program_clock.resume()
    clock[0] += 2.0
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_wait_timeout"
    assert port.releases() == [("cancel", "watch.1")]


@pytest.mark.parametrize("failure", ["refused_start", "host_released"])
def test_owner_failures_fail_the_node_and_release(failure: str) -> None:
    node, port, _clock, _program_clock, _events = _waiter()
    if failure == "refused_start":
        port.start_error = _OwnerError("unsupported_capability", {"reason": "io_signal_unbound"})
        node.tick_once()
        assert node.failure_reason_code == "io_signal_unbound"
        assert port.releases() == []
    else:
        node.tick_once()
        port.state = OwnerTaskState.FAILED
        node.tick_once()
        assert node.failure_reason_code == "io_watch_released"
        assert port.releases() == [("cancel", "watch.1")]
    assert node.status is py_trees.common.Status.FAILURE


def test_stop_reset_or_preemption_releases_and_reports_interrupted() -> None:
    node, port, _clock, _program_clock, _events = _waiter()
    node.tick_once()
    node.stop(py_trees.common.Status.INVALID)
    assert port.releases() == [("cancel", "watch.1")]
    assert node.presented_interrupted is True
    # The next attempt registers a new watch.
    node.tick_once()
    assert ("start", "attempt.2") in port.calls


def test_transient_read_failure_is_not_teardown() -> None:
    node, port, _clock, _program_clock, _events = _waiter()
    node.tick_once()
    port.observe_error = OwnerTaskObservationUnavailableError("socket gone")
    for _ in range(3):
        node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    assert port.releases() == []
    port.observe_error = None
    port.state = OwnerTaskState.SUCCEEDED
    node.tick_once()
    assert node.status is py_trees.common.Status.SUCCESS


def test_a_watch_is_never_owner_work_to_settle_and_a_hold_keeps_it() -> None:
    node, port, _clock, _program_clock, _events = _waiter()
    node.tick_once()
    observed = len(port.calls)
    port.state = OwnerTaskState.SUCCEEDED
    node.observe_while_operator_stop_held()
    assert len(port.calls) == observed  # A match is reported when ticking resumes.
    assert node.has_active_task is False
    assert node.observe_control_state() == "inactive"
    assert node.reconcile_full_stop_task(selected_resume=True) == "unaffected"
    assert port.releases() == []


# --- Review regressions (snapshot-b) ---------------------------------------


@pytest.mark.parametrize("call", ["start", "observe"])
def test_a_match_returned_after_the_deadline_times_out_and_releases(call: str) -> None:
    node, port, clock, _program_clock, _events = _waiter(timeout_s=1.0)
    if call == "observe":
        node.tick_once()
        assert node.status is py_trees.common.Status.RUNNING
    port.state = OwnerTaskState.SUCCEEDED

    def slow_call() -> None:  # The synchronous exchange crosses timeout_s.
        clock[0] += 1.5
    port.during_call = slow_call
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_wait_timeout"
    assert port.releases() == [("acknowledge", "watch.1")]


def test_a_halt_during_the_call_does_not_count_toward_the_deadline() -> None:
    node, port, clock, program_clock, _events = _waiter(timeout_s=1.0)
    node.tick_once()
    port.state = OwnerTaskState.SUCCEEDED

    def halted_call() -> None:
        program_clock.freeze()
        clock[0] += 60.0
    port.during_call = halted_call
    node.tick_once()
    assert node.status is py_trees.common.Status.SUCCESS


def _wire_port(monkeypatch, tmp_path, replies: dict[str, dict[str, Any]]):
    """Actual host envelopes through the Runner transport, client and port."""

    from manyforge_headless_api.execution import ExecutionCommandClient
    from manyforge_program_runner.execution_transport import (
        ProgramRunnerExecutionTransport,
    )

    wire = ProgramRunnerExecutionTransport(tmp_path, timeout_seconds=1.0)
    sent: list[dict[str, Any]] = []

    def exchange_on(_self, command, _socket_path):
        sent.append(deepcopy(command))
        return deepcopy(replies[command["command"]])

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange_on)
    transport = _Transport()
    transport.register_io_watch = wire.register_io_watch
    transport.read_io_watch = wire.read_io_watch
    transport.release_io_watch = wire.release_io_watch
    transport.release_io_watch_scope = wire.release_io_watch_scope
    port = NativeKernelProgramPort(KernelClient(transport))
    port.bind(_context([_signal_row()]))
    return port, sent


def _wire_registration(attempt: str) -> dict[str, Any]:
    return {
        "owner_epoch": "kernel.epoch.7",
        "catalog_generation": "1",
        "registration_id": "1",
        "scope": {
            "program_runner_owner_epoch": "runner.epoch.3",
            "program_run_id": "run.1",
            "node": {"node_instance_id": "wait", "node_attempt_id": attempt},
        },
    }


def _wire_replies(read: dict[str, Any]) -> dict[str, dict[str, Any]]:
    api = {"api_version": "manyforge.execution_command_response.v1"}
    observation = _value("fresh", False)
    observation["receipt_age_ns"] = "1000"
    return {
        "register_io_watch": {
            **api, "ok": True, "command": "register_io_watch",
            "registration": _wire_registration("attempt.1"),
            "result": {"state": "pending", "observation": observation, "matched_sample": None},
        },
        "read_io_watch": {**api, **read},
        "release_io_watch": {**api, "ok": True, "command": "release_io_watch"},
    }


def test_actual_released_reply_fails_the_wait_with_io_watch_released(
    monkeypatch, tmp_path
) -> None:
    released = {
        "ok": True, "command": "read_io_watch",
        "result": {"state": "released", "observation": None, "matched_sample": None},
    }
    port, sent = _wire_port(monkeypatch, tmp_path, _wire_replies(released))
    node, _fake, _clock, _program_clock, _events = _waiter(port=port.io_watches)
    node.tick_once()
    assert node.status is py_trees.common.Status.RUNNING
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_watch_released"
    assert [command["command"] for command in sent] == [
        "register_io_watch", "read_io_watch", "release_io_watch"
    ]
    assert sent[2]["registration"] == _wire_registration("attempt.1")


def test_actual_stale_generation_refusal_fails_the_wait_with_its_reason(
    monkeypatch, tmp_path
) -> None:
    refusal = {
        "ok": False,
        "error": {
            "code": "stale_generation",
            "message": "I/O signal identity is no longer current",
            "details": {"reason": "io_watch_generation_stale"},
        },
    }
    port, sent = _wire_port(monkeypatch, tmp_path, _wire_replies(refusal))
    node, _fake, _clock, _program_clock, _events = _waiter(port=port.io_watches)
    node.tick_once()
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert node.failure_reason_code == "io_watch_generation_stale"
    assert [command["command"] for command in sent] == [
        "register_io_watch", "read_io_watch", "release_io_watch"
    ]


def test_actual_unknown_registration_fails_and_releases_the_attempt_scope(
    monkeypatch, tmp_path
) -> None:
    from manyforge_headless_api.native import HeadlessApiError

    replies = _wire_replies({"ok": True, "command": "read_io_watch"})
    port, sent = _wire_port(monkeypatch, tmp_path, replies)
    from manyforge_headless_api.execution import ExecutionCommandClient

    def exchange_on(_self, command, _socket_path):
        sent.append(deepcopy(command))
        if command["command"] == "register_io_watch":
            # The request may have reached the host; its reply never arrived.
            raise HeadlessApiError("execution_owner_unavailable", "timed out")
        return deepcopy(replies[command["command"]])

    monkeypatch.setattr(ExecutionCommandClient, "_exchange_on", exchange_on)
    node, _fake, _clock, _program_clock, _events = _waiter(port=port.io_watches)
    node.tick_once()
    assert node.status is py_trees.common.Status.FAILURE
    assert [command["command"] for command in sent] == [
        "register_io_watch", "release_io_watch"
    ]
    release = sent[1]
    assert set(release) == {"api_version", "command", "attribution", "scope_request"}
    assert release["attribution"]["transport"] == "internal"
    assert release["scope_request"] == {
        **_SCOPE_RELEASE, "node_instance_id": "wait", "node_attempt_id": "attempt.1"
    }
    # The next attempt registers again; nothing else is released for attempt.1.
    node.tick_once()
    assert [command["command"] for command in sent][2:] == [
        "register_io_watch", "release_io_watch"
    ]
    assert sent[3]["scope_request"]["node_attempt_id"] == "attempt.2"
