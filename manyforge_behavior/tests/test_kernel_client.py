"""Thin addressed Kernel client and behavior-owned action spans."""

from __future__ import annotations

from typing import Any, Mapping

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecutionState,
    KernelClient,
    KernelClientResponseError,
    LegacyKernelMockUnavailableError,
    MockKernelClient,
)


class FakeTransport:
    def __init__(self) -> None:
        self.submitted: Mapping[str, Any] | None = None

    def execution_status(self) -> dict[str, Any]:
        return {"ok": True, "status": {"owner_epoch": "epoch-1"}}

    def execute(self, body: Mapping[str, Any]) -> dict[str, Any]:
        self.submitted = body
        return {"ok": True, "accepted": {"operation_id": "operation-1"}}

    def operation(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "operation": {"operation_id": operation_id, "state": "completed"},
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

    assert client.status()["status"]["owner_epoch"] == "epoch-1"
    assert client.execute({"program_id": "program-1"})["accepted"][
        "operation_id"
    ] == "operation-1"
    assert transport.submitted == {"program_id": "program-1"}
    assert client.operation("operation-1")["operation"]["state"] == "completed"


def test_malformed_success_and_legacy_stateful_mock_fail_closed() -> None:
    class BadTransport(FakeTransport):
        def execution_status(self) -> dict[str, Any]:
            return {"ok": False}

    with pytest.raises(KernelClientResponseError):
        KernelClient(BadTransport()).status()
    with pytest.raises(LegacyKernelMockUnavailableError):
        MockKernelClient()
