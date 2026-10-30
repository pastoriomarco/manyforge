"""Item 37: Force close, the operator's explicit action after a refused Close.

Headless runs each step: `stop` sends the cell Full Stop and waits briefly
until the Kernel confirms it (fail closed with a typed reason otherwise);
`close_operations` runs the Kernel step and records one History row.
"""

from __future__ import annotations

from typing import Any

import pytest
from fastapi.testclient import TestClient

import manyforge_headless_api.app as app_module
from manyforge_headless_api.app import create_app
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_app import (
    FakeCore,
    FakeExecution,
    FakeManager,
    FakeProgramRunner,
    _service,
)

_CONTEXT = {
    "request_id": "force-close.1",
    "request_owner_epoch": "kernel.epoch.1",
    "source_label": "composer_ui",
    "correlation_id": None,
}
_CLOSED = {
    "containment_uncertain": False,
    "operation_id": "op.io",
    "operation_revision": 2,
    "state": "failed",
    "targets": [
        {"command_target_id": "valve", "controllable_component_id": "io", "output_unknown": True}
    ],
}


def _armed() -> dict[str, Any]:
    return {
        "ok": True,
        "command": "force_close",
        "result": {"operations": [], "released_orphan_resources": [], "step": "arm", "stop_generation": 5},
    }


def _unconfirmed(reason: str) -> dict[str, Any]:
    return {
        "ok": False,
        "error": {
            "code": "scenario_force_close_stop_unconfirmed",
            "message": "Force close requires the operator's confirmed Full Stop",
            "details": {"reason": reason, "stop_generation": "5"},
        },
    }


class _ForceCloseExecution(FakeExecution):
    def __init__(self, arm_replies: list[dict[str, Any]] | None = None,
                 operations_reply: dict[str, Any] | None = None) -> None:
        super().__init__()
        self.arm_replies = list(arm_replies or [])
        self.operations_reply = operations_reply
        self.steps: list[tuple[str, int | None]] = []

    def full_stop(self, request: Any, *, trusted_principal_id: str | None) -> dict[str, Any]:
        self.requests.append(("full_stop", request))
        return {
            "ok": True,
            "command": "full_stop",
            "accepted": {
                "request_id": request.request_context.request_id,
                "resolution_reason": "explicit_full_stop",
                "stop_generation": 5,
            },
        }

    def force_close(self, *, step: str, stop_generation: int | None = None) -> dict[str, Any]:
        self.steps.append((step, stop_generation))
        if step == "arm":
            return self.arm_replies.pop(0) if len(self.arm_replies) > 1 else self.arm_replies[0]
        return self.operations_reply or {
            "ok": True,
            "command": "force_close",
            "result": {
                "motion_block_cleared": True,
                "operations": [_CLOSED],
                "released_orphan_resources": [],
                "step": "close_operations",
                "stop_generation": 5,
            },
        }


class _HistoryCore(FakeCore):
    def __init__(self) -> None:
        super().__init__()
        self.force_close_records: list[dict[str, Any]] = []

    def record_scenario_force_close(self, record: dict[str, Any]) -> dict[str, Any]:
        self.force_close_records.append(dict(record))
        return {"event_id": "event-force-close", "recorded": True,
                "schema_version": "manyforge.scenario_force_close_record.v1"}


def _client(execution: _ForceCloseExecution, core: FakeCore | None = None) -> TestClient:
    return TestClient(create_app(service=_service(
        core or _HistoryCore(), FakeManager(), execution,
        program_runner=FakeProgramRunner(active=False))))


def _stop() -> dict[str, Any]:
    return {"request_context": _CONTEXT, "step": "stop", "expected_stop_generation": 4}


def test_force_close_stop_waits_until_the_kernel_confirms_the_full_stop(monkeypatch) -> None:
    monkeypatch.setattr(app_module, "FORCE_CLOSE_STOP_POLL_SECONDS", 0.0)
    execution = _ForceCloseExecution([_unconfirmed("stop_pending"), _armed()])
    with _client(execution) as client:
        response = client.post("/api/execution/force-close", json=_stop())

    assert response.status_code == 200, response.text
    assert response.json()["result"]["stop_generation"] == 5
    full_stop = [request for kind, request in execution.requests if kind == "full_stop"]
    assert len(full_stop) == 1
    assert full_stop[0].expected_stop_generation == 4
    # The Kernel arm names the accepted Full Stop; it is asked again only while pending.
    assert execution.steps == [("arm", 5), ("arm", 5)]


@pytest.mark.parametrize("reason", ["stop_not_contained", "stop_superseded"])
def test_force_close_refuses_with_a_typed_reason_when_the_full_stop_is_unconfirmed(reason: str) -> None:
    execution = _ForceCloseExecution([_unconfirmed(reason)])
    with _client(execution) as client:
        response = client.post("/api/execution/force-close", json=_stop())

    assert response.status_code == 409, response.text
    error = response.json()["error"]
    assert error["code"] == "scenario_force_close_stop_unconfirmed"
    assert error["details"]["reason"] == reason
    # Fail closed: no second arm, no Kernel step.
    assert execution.steps == [("arm", 5)]


def test_force_close_refuses_when_the_full_stop_stays_pending(monkeypatch) -> None:
    monkeypatch.setattr(app_module, "FORCE_CLOSE_STOP_WAIT_SECONDS", 0.0)
    execution = _ForceCloseExecution([_unconfirmed("stop_pending")])
    with _client(execution) as client:
        response = client.post("/api/execution/force-close", json=_stop())

    assert response.status_code == 409, response.text
    assert response.json()["error"]["details"]["reason"] == "stop_pending"
    assert all(step == "arm" for step, _generation in execution.steps)


def test_force_close_operations_records_one_history_row_of_what_stays_unknown() -> None:
    core = _HistoryCore()
    execution = _ForceCloseExecution()
    with _client(execution, core) as client:
        response = client.post("/api/execution/force-close",
                               json={"request_context": _CONTEXT, "step": "close_operations"})

    assert response.status_code == 200, response.text
    body = response.json()
    assert body["result"]["operations"] == [_CLOSED]
    assert body["history"] == {"event_id": "event-force-close", "recorded": True,
                               "schema_version": "manyforge.scenario_force_close_record.v1"}
    assert execution.steps == [("close_operations", None)]
    assert core.force_close_records == [{
        "schema_version": "manyforge.scenario_force_close.v1",
        "reason_code": "cell_force_closed_by_operator",
        "request_id": "force-close.1",
        "source_label": "composer_ui",
        "stop_generation": 5,
        "operations": [_CLOSED],
        "released_orphan_resources": [],
        "motion_block_cleared": True,
    }]


@pytest.mark.parametrize("code", ["scenario_force_close_not_closing", "scenario_force_close_operation_busy"])
def test_a_refused_kernel_step_records_no_history_row(code: str) -> None:
    core = _HistoryCore()
    execution = _ForceCloseExecution(operations_reply={
        "ok": False, "error": {"code": code, "message": "refused", "details": {}}})
    with _client(execution, core) as client:
        response = client.post("/api/execution/force-close",
                               json={"request_context": _CONTEXT, "step": "close_operations"})

    assert response.status_code == 409, response.text
    assert response.json()["error"]["code"] == code
    assert core.force_close_records == []


def test_force_close_end_step_ends_the_attempt_without_a_kernel_call() -> None:
    # SX-616: the caller ends every attempt; nothing reaches the Kernel.
    execution = _ForceCloseExecution()
    with _client(execution) as client:
        response = client.post("/api/execution/force-close",
                               json={"request_context": _CONTEXT, "step": "end"})

    assert response.status_code == 200, response.text
    assert response.json()["result"] == {"step": "end", "operations": []}
    assert execution.steps == []


@pytest.mark.parametrize("body", [
    {"request_context": _CONTEXT, "step": "end", "expected_stop_generation": 4},
    {"request_context": _CONTEXT, "step": "stop"},
    {"request_context": _CONTEXT, "step": "close_operations", "expected_stop_generation": 4},
    {"request_context": _CONTEXT, "step": "recover"},
])
def test_force_close_request_shape_is_closed(body: dict[str, Any]) -> None:
    execution = _ForceCloseExecution([_armed()])
    with _client(execution) as client:
        response = client.post("/api/execution/force-close", json=body)

    assert response.status_code == 422, response.text
    assert execution.steps == []


def test_execution_client_checks_the_force_close_reply_against_its_step() -> None:
    client = object.__new__(ExecutionCommandClient)
    sent: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        sent.append(command)
        return {"ok": True, "command": "force_close",
                "result": {"operations": [], "released_orphan_resources": [], "step": "arm", "stop_generation": 6}}

    client._exchange = exchange  # type: ignore[method-assign]
    with pytest.raises(HeadlessApiError) as mismatch:
        client.force_close(step="arm", stop_generation=5)
    assert mismatch.value.code == "execution_command_response_identity_mismatch"
    assert sent[0]["command"] == "force_close" and sent[0]["stop_generation"] == 5
    with pytest.raises(HeadlessApiError):
        client.force_close(step="close_operations")
    assert "stop_generation" not in sent[1]
