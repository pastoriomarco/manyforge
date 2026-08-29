"""Thin addressed Kernel client and behavior-owned action spans."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelClientResponseError,
    KernelOperationObservationUnavailableError,
    LegacyKernelMockUnavailableError,
    MockKernelClient,
)


class FakeTransport:
    def __init__(self) -> None:
        self.submitted: Mapping[str, Any] | None = None
        self.controls: list[tuple[str, str, Mapping[str, Any]]] = []

    def execution_identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {"owner_epoch": "epoch-1"}}

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {"ok": True, "accepted": {"operation_id": "operation-1"}}

    def plan_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "planning_result": {"kind": "pick", "sequence_id": "pick-1"},
        }

    def plan_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "planning_result": {"kind": "place", "sequence_id": "place-1"},
        }

    def start_pick(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "sequence_status": {"kind": "pick", "sequence_id": "pick-1"},
        }

    def start_place(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {
            "ok": True,
            "sequence_status": {"kind": "place", "sequence_id": "place-1"},
        }

    def pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "sequence_status": {"sequence_id": sequence_id, "phase": "completed"},
        }

    def operation(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
        }

    def _control(
        self, kind: str, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.controls.append((kind, operation_id, body))
        return {"ok": True, "accepted": {"operation_id": operation_id}}

    def pause_operation(self, operation_id, body):
        return self._control("pause", operation_id, body)

    def resume_operation(self, operation_id, body):
        return self._control("resume", operation_id, body)

    def reset_operation(self, operation_id, body):
        return self._control("reset", operation_id, body)

    def acknowledge_completed_operation(self, operation_id, body):
        return self._control("acknowledge_completed", operation_id, body)

    def stop_operations(self, body):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)

    def set_operation_runtime_scale(self, operation_id, body):
        return self._control("runtime_scale", operation_id, body)

    def operation_runtime_scale(self, operation_id):
        return {
            "ok": True,
            "runtime_scale": {"operation_id": operation_id, "effective_scale": 1.0},
        }


def test_execution_state_vocabulary_is_the_native_owner_vocabulary() -> None:
    assert {state.value for state in ExecutionState} == {
        "queued",
        "resolving",
        "planning",
        "validating",
        "prepared",
        "dispatching",
        "executing",
        "paused",
        "stopping",
        "completed",
        "stopped",
        "failed",
        "interrupted",
        "intervention_required",
        "cancelled_pre_dispatch",
    }


def test_program_action_span_requires_exact_attempt_identity() -> None:
    span = ActionSpan(
        action_source="program_node",
        action_span_id="run/node/attempt-1",
        program_run_id="run-1",
        node_instance_id="root/node[0]",
        node_attempt_id="run/node/attempt-1",
    )
    assert span.as_request()["action_span_id"] == span.node_attempt_id
    with pytest.raises(ValueError, match="must equal"):
        ActionSpan(
            action_source="program_node",
            action_span_id="span-1",
            node_instance_id="root/node[0]",
            node_attempt_id="attempt-2",
        )


def test_kernel_client_only_forwards_bounded_headless_operations() -> None:
    transport = FakeTransport()
    client = KernelClient(transport)

    assert client.identity()["identity"]["owner_epoch"] == "epoch-1"
    assert client.execute({"program_id": "program-1"})["accepted"][
        "operation_id"
    ] == "operation-1"
    assert transport.submitted == {"program_id": "program-1"}
    assert client.plan_pick({"sequence_id": "pick-1"})["planning_result"][
        "kind"
    ] == "pick"
    assert client.plan_place({"sequence_id": "place-1"})["planning_result"][
        "kind"
    ] == "place"
    assert client.start_pick({"sequence_id": "pick-1"})["sequence_status"][
        "kind"
    ] == "pick"
    assert client.start_place({"sequence_id": "place-1"})["sequence_status"][
        "kind"
    ] == "place"
    assert client.pick_place_sequence("pick-1")["sequence_status"][
        "phase"
    ] == "completed"
    assert client.operation("operation-1")["operation"]["state"] == "completed"
    request = {"expected_operation_revision": 1}
    stop_request = {
        "operations": [{"operation_id": "operation-1"}],
    }
    assert client.stop(stop_request)["accepted"]["operation_id"] == "operation-1"
    assert client.set_runtime_scale("operation-1", request)["accepted"][
        "operation_id"
    ] == "operation-1"
    assert client.runtime_scale("operation-1")["runtime_scale"][
        "effective_scale"
    ] == 1.0
    assert transport.controls == [
        ("stop", "operation-1", stop_request),
        ("runtime_scale", "operation-1", request),
    ]


def test_malformed_success_and_legacy_stateful_mock_fail_closed() -> None:
    class BadTransport(FakeTransport):
        def execution_identity(self) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(BadTransport()).identity()
    with pytest.raises(LegacyKernelMockUnavailableError):
        MockKernelClient()


def test_only_exact_operation_owner_unavailability_is_typed_for_retry() -> None:
    class OwnerUnavailableError(RuntimeError):
        code = "execution_owner_unavailable"

    class UnavailableObservationTransport(FakeTransport):
        def operation(self, operation_id: str) -> dict[str, Any]:
            raise OwnerUnavailableError("HTTP 503")

    client = KernelClient(UnavailableObservationTransport())
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.operation("operation-1")

    class MalformedObservationTransport(FakeTransport):
        def operation(self, operation_id: str) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(MalformedObservationTransport()).operation("operation-1")
