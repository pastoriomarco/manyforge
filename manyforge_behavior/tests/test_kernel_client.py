"""Thin addressed Kernel client and behavior-owned action spans."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelClientResponseError,
    KernelOperationAdmissionRejectedError,
    KernelOperationObservationUnavailableError,
    KernelOperationSettlementPendingError,
    LegacyKernelMockUnavailableError,
    MockKernelClient,
)


@pytest.mark.parametrize(
    "field,value",
    [
        ("request_id", "another.request"),
        ("action", "reset_cycle"),
        ("accepted", 1),
        ("accepted", "true"),
        ("resulting_assessment", None),
    ],
)
def test_recovery_result_must_match_the_exact_request(field, value) -> None:
    request = {"request_id": "recovery.1", "action": "acknowledge_and_continue"}
    result = {**request, "accepted": True, "resulting_assessment": {}, field: value}

    class Transport(FakeTransport):
        def apply_operation_recovery(self, operation_id, revision, body):
            assert (operation_id, revision, body) == ("operation.1", 3, request)
            return {"ok": True, "result": result}

    with pytest.raises(KernelClientResponseError, match="exact owner result"):
        KernelClient(Transport()).recover("operation.1", 3, request)


class FakeTransport:
    def __init__(self) -> None:
        self.submitted: Mapping[str, Any] | None = None
        self.controls: list[tuple[str, str, Mapping[str, Any]]] = []

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "identity": {"owner_epoch": "epoch-1"}}

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
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

    def read(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
        }

    def _control(
        self, kind: str, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]:
        self.controls.append((kind, operation_id, body))
        return {"ok": True, "accepted": {"operation_id": operation_id}}

    def reset(self, operation_id, body):
        return self._control("reset", operation_id, body)

    def acknowledge_completed(self, operation_id, body):
        return self._control("acknowledge_completed", operation_id, body)

    def stop(self, body):
        operation_id = body["operations"][0]["operation_id"]
        return self._control("stop", operation_id, body)


def test_execution_state_vocabulary_is_the_native_owner_vocabulary() -> None:
    assert {state.value for state in ExecutionState} == {
        "queued",
        "resolving",
        "planning",
        "validating",
        "prepared",
        "dispatching",
        "executing",
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
    assert client.operation("operation-1")["operation"]["state"] == "completed"
    request = {"expected_operation_revision": 1}
    stop_request = {
        "operations": [{"operation_id": "operation-1"}],
    }
    assert client.stop(stop_request)["accepted"]["operation_id"] == "operation-1"
    assert transport.controls == [("stop", "operation-1", stop_request)]


def test_malformed_success_and_legacy_stateful_mock_fail_closed() -> None:
    class BadTransport(FakeTransport):
        def identity(self) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(BadTransport()).identity()
    with pytest.raises(LegacyKernelMockUnavailableError):
        MockKernelClient()


def test_execute_preserves_explicit_rejection_and_unknown_transport_failure() -> None:
    class RejectedTransport(FakeTransport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            return {
                "ok": False,
                "error": {
                    "code": "invalid_state_transition",
                    "message": "Program run is held",
                    "details": {"reason": "program_run_stop_fence_active"},
                },
            }

    with pytest.raises(KernelOperationAdmissionRejectedError) as rejected:
        KernelClient(RejectedTransport()).execute({"program_id": "program-1"})
    assert rejected.value.code == "invalid_state_transition"
    assert rejected.value.details == {"reason": "program_run_stop_fence_active"}

    class LostTransport(FakeTransport):
        def submit(self, body: Mapping[str, Any]) -> dict[str, Any]:
            raise TimeoutError("acceptance reply lost")

    with pytest.raises(TimeoutError, match="acceptance reply lost"):
        KernelClient(LostTransport()).execute({"program_id": "program-1"})


def test_only_exact_operation_owner_unavailability_is_typed_for_retry() -> None:
    class OwnerUnavailableError(RuntimeError):
        code = "execution_owner_unavailable"

    class UnavailableObservationTransport(FakeTransport):
        def read(self, operation_id: str) -> dict[str, Any]:
            raise OwnerUnavailableError("HTTP 503")

    client = KernelClient(UnavailableObservationTransport())
    with pytest.raises(KernelOperationObservationUnavailableError):
        client.operation("operation-1")

    class MalformedObservationTransport(FakeTransport):
        def read(self, operation_id: str) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(MalformedObservationTransport()).operation("operation-1")


def test_only_exact_in_flight_effect_is_typed_as_settlement_pending() -> None:
    class EffectInFlightError(RuntimeError):
        code = "invalid_state_transition"
        details = {"reason": "operation_effect_in_flight"}

    class SettlingTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise EffectInFlightError("subordinate effect is still settling")

    client = KernelClient(SettlingTransport())
    with pytest.raises(KernelOperationSettlementPendingError):
        client.acknowledge_completed(
            "operation-1", {"expected_operation_revision": 2}
        )

    class RejectedTransport(FakeTransport):
        def acknowledge_completed(self, operation_id, body):
            raise RuntimeError("permanent rejection")

    with pytest.raises(RuntimeError, match="permanent rejection"):
        KernelClient(RejectedTransport()).acknowledge_completed(
            "operation-1", {"expected_operation_revision": 2}
        )
