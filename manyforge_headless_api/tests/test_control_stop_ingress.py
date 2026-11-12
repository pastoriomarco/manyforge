"""A recognized Stop on the control WebSocket or /api/control/commands whose
payload is invalid is a Full Stop, as on the Stop HTTP routes; other commands
keep their refusal. Both transports share one dispatch; the stamp names it."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ExecutionCommandRequestDTO,
    ExecutionIngressStamp,
)
from manyforge_headless_api.native import HeadlessApiError


def _command(kind: str, payload: dict[str, Any]) -> ExecutionCommandRequestDTO:
    authority = ControlFreshnessAuthority(
        clock_ns=lambda: 1_000_000_000,
        receiver_incarnation="receiver.1",
        secret=b"x" * 32,
    )
    return ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "correlation_id": "request.invalid",
                "source_label": "test",
                "control_session_id": None,
                "client_sequence": None,
                "freshness": authority.issue(
                    semantic_owner_epoch="runner.1", execution_control_revision=7
                ).model_dump(mode="json"),
                "expected_semantic_owner_epoch": "runner.1",
                "expected_execution_control_revision": 7,
            },
            "payload": payload,
        }
    )


class _MalformedStopService:
    """Records the recognized-malformed Stop call; any other owner call fails."""

    def __init__(self) -> None:
        self.malformed: list[dict[str, Any]] = []

    @staticmethod
    def program_cycle_status() -> dict[str, Any]:
        raise AssertionError("recognized Stop synchronously read Program state")

    def execution_recognized_malformed_stop(self, **call: Any) -> dict[str, Any]:
        self.malformed.append(call)
        return {
            "ok": True,
            "command": "stop",
            "accepted": {
                "request_id": call["request_id"],
                "resolution_disposition": "promoted_to_full",
                "resolution_reason": "malformed_ordinary_intent",
            },
        }

    def _unexpected(self, *args: Any) -> Any:
        raise AssertionError("an invalid payload reached its owner call")

    execution_stop = execution_full_stop = _unexpected
    program_run_stop = program_run_full_stop = program_run_motion_stop = _unexpected
    execution_reset = _unexpected


_OWNER_FENCE = {"expected_execution_owner_epoch": "kernel.epoch.1"}
_CONTEXT = {
    "request_id": "request.invalid",
    "request_owner_epoch": "kernel.epoch.1",
    "source_label": "test",
    "correlation_id": None,
}


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
@pytest.mark.parametrize(
    "kind,payload,route",
    [
        (
            "operation_stop",
            {
                **_OWNER_FENCE,
                "request_context": _CONTEXT,
                "operations": [
                    {"operation_id": "b", "expected_operation_revision": 1,
                     "expected_stop_generation": 0},
                    {"operation_id": "a", "expected_operation_revision": 1,
                     "expected_stop_generation": 0},
                ],
            },
            "/api/execution/stops",
        ),
        (
            "cell_full_stop",
            {**_OWNER_FENCE, "request_context": _CONTEXT, "expected_stop_generation": -1},
            "/api/execution/full-stop",
        ),
        ("program_stop", {"request_id": "request.invalid"}, "/api/execution/stops"),
        (
            "program_full_stop",
            {"request_id": "request.invalid"},
            "/api/execution/full-stop",
        ),
        (
            "program_motion_stop",
            {"request_id": "request.invalid", "program_run_id": "program-run.1"},
            "/api/execution/stops",
        ),
    ],
)
def test_control_stop_with_invalid_payload_is_a_full_stop(
    transport_kind: str, kind: str, payload: dict[str, Any], route: str
) -> None:
    service = _MalformedStopService()
    stamp = ExecutionIngressStamp(
        transport_kind=transport_kind,
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    first, result = dispatch(_command(kind, payload), stamp)
    assert first.disposition == "received"
    assert result is not None
    assert result["accepted"]["resolution_disposition"] == "promoted_to_full"
    assert result["accepted"]["resolution_reason"] == "malformed_ordinary_intent"
    # A repeat is its own request: the Kernel joins it to the Full Stop that
    # is still running (Kernel test StopWithUnusableIdentityPromotes...).
    repeat, _ = dispatch(_command(kind, payload), stamp)
    assert repeat.disposition == "received"
    assert [call["recognized_route"] for call in service.malformed] == [route, route]
    assert {call["reason"] for call in service.malformed} == {
        "recognized_stop_payload_invalid"
    }
    first_id, second_id = (call["request_id"] for call in service.malformed)
    assert first_id.startswith("malformed-stop.") and first_id != second_id


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
def test_control_non_stop_with_invalid_payload_is_still_refused(transport_kind: str) -> None:
    stamp = ExecutionIngressStamp(
        transport_kind=transport_kind,
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )
    invalid_reset = _command(
        "operation_reset",
        {**_OWNER_FENCE, "request_context": _CONTEXT, "operation_id": "operation-1",
         "expected_operation_revision": 0},
    )

    class Available(_MalformedStopService):
        @staticmethod
        def program_cycle_status() -> dict[str, Any]:
            return {}

    service = Available()
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    dispatch._availability = lambda cycle, kind: (True, None)  # type: ignore[method-assign]
    dispatch._manual_scope_conflict = lambda request, cycle: None  # type: ignore[method-assign]
    with pytest.raises(HeadlessApiError) as refused:
        dispatch(invalid_reset, stamp)
    assert refused.value.code == "execution_control_payload_invalid"
    assert service.malformed == []


class _OrdinaryStopService(_MalformedStopService):
    """Records the ordinary Stop calls an owner-fence-free Stop makes."""

    def __init__(self) -> None:
        super().__init__()
        self.ordinary: list[tuple[str, Any]] = []

    def execution_stop(self, request: Any, attribution: Any) -> dict[str, Any]:
        del attribution
        self.ordinary.append(("stop", request))
        return {"ok": True, "command": "stop", "accepted": {}}

    def execution_full_stop(self, request: Any, attribution: Any) -> dict[str, Any]:
        del attribution
        self.ordinary.append(("full_stop", request))
        return {"ok": True, "command": "full_stop", "accepted": {}}


_STOP_PAYLOADS = {
    "operation_stop": {
        "request_context": _CONTEXT,
        "operations": [
            {"operation_id": "a", "expected_operation_revision": 1,
             "expected_stop_generation": 0},
        ],
    },
    "cell_full_stop": {"request_context": _CONTEXT, "expected_stop_generation": 0},
}


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
@pytest.mark.parametrize("kind", ["operation_stop", "cell_full_stop"])
def test_control_stop_without_owner_fence_is_an_ordinary_stop(
    transport_kind: str, kind: str
) -> None:
    service = _OrdinaryStopService()
    stamp = ExecutionIngressStamp(
        transport_kind=transport_kind,
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )
    disposition, _ = HeadlessExecutionControlDispatch(service)(  # type: ignore[arg-type]
        _command(kind, dict(_STOP_PAYLOADS[kind])), stamp
    )
    assert disposition.disposition == "received"
    assert service.malformed == []
    ((called, request),) = service.ordinary
    assert called == ("stop" if kind == "operation_stop" else "full_stop")
    # The caller's own epoch stays; the Kernel resolves the Stop on its own.
    assert request.request_context.request_owner_epoch == (
        _command(kind, {}).command_envelope.freshness.receiver_incarnation
    )


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
@pytest.mark.parametrize("kind", ["operation_stop", "cell_full_stop"])
@pytest.mark.parametrize("owner", ["", "o" * 300, 7, None])
def test_control_stop_with_invalid_owner_fence_is_a_full_stop(
    transport_kind: str, kind: str, owner: Any
) -> None:
    service = _MalformedStopService()
    stamp = ExecutionIngressStamp(
        transport_kind=transport_kind,
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )
    payload = {**_STOP_PAYLOADS[kind], "expected_execution_owner_epoch": owner}
    disposition, result = HeadlessExecutionControlDispatch(service)(  # type: ignore[arg-type]
        _command(kind, payload), stamp
    )
    assert disposition.disposition == "received"
    assert result is not None
    assert result["accepted"]["resolution_disposition"] == "promoted_to_full"
    assert len(service.malformed) == 1


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
def test_control_non_stop_without_owner_fence_is_still_refused(transport_kind: str) -> None:
    stamp = ExecutionIngressStamp(
        transport_kind=transport_kind,
        receiver_incarnation="receiver.1",
        received_server_sequence=1,
        received_monotonic_ns=1,
    )

    class Available(_MalformedStopService):
        @staticmethod
        def program_cycle_status() -> dict[str, Any]:
            return {}

    service = Available()
    dispatch = HeadlessExecutionControlDispatch(service)  # type: ignore[arg-type]
    dispatch._availability = lambda cycle, kind: (True, None)  # type: ignore[method-assign]
    dispatch._manual_scope_conflict = lambda request, cycle: None  # type: ignore[method-assign]
    with pytest.raises(HeadlessApiError) as refused:
        dispatch(
            _command(
                "operation_reset",
                {"request_context": _CONTEXT, "operation_id": "operation-1",
                 "expected_operation_revision": 1},
            ),
            stamp,
        )
    assert refused.value.code == "execution_control_payload_invalid"
    assert service.malformed == []
