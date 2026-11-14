"""A recognized Stop on the control WebSocket or /api/control/commands whose
payload is invalid is still a Stop, as on the Stop HTTP routes: the Kernel
widens it from the route it names (PD-1: Halt stays Halt, the others become
Stop all); other commands keep their refusal. Both transports share one
dispatch; the stamp names it."""

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
from manyforge_headless_api.identity import RequestAdmission
from manyforge_headless_api.native import HeadlessApiError

APPLICATION_START_ID = "3f9a0000000000a1"
# A Headless key: class 1 (0x40), part 0, minter 5 (contract 1.4).
HEADLESS_KEY_PREFIX = "40" + "00000000000005"


def _command(
    kind: str, payload: dict[str, Any], transport_kind: str
) -> ExecutionCommandRequestDTO:
    """A WebSocket command of session 1, or the session-less REST form,
    which carries this start's ``A`` (contract 1.6)."""

    authority = ControlFreshnessAuthority(
        clock_ns=lambda: 1_000_000_000,
        secret=b"x" * 32,
    )
    websocket = transport_kind == "websocket"
    return ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "source_label": "test",
                "control_session_number": 1 if websocket else None,
                "client_sequence": "1" if websocket else None,
                "freshness": authority.issue(
                    execution_control_revision=7
                ).model_dump(mode="json"),
                "expected_execution_control_revision": 7,
            },
            "payload": (
                payload
                if websocket
                else {**payload, "application_start_id": APPLICATION_START_ID}
            ),
        }
    )


def _dispatch(service: Any) -> HeadlessExecutionControlDispatch:
    return HeadlessExecutionControlDispatch(
        service, RequestAdmission(APPLICATION_START_ID)
    )


def _stamp(transport_kind: str) -> ExecutionIngressStamp:
    return ExecutionIngressStamp(
        transport_kind=transport_kind,  # type: ignore[arg-type]
        received_server_sequence=1,
        received_monotonic_ns=1,
    )


class _MalformedStopService:
    """Records the recognized-malformed Stop call; any other owner call fails."""

    def __init__(self) -> None:
        self.malformed: list[dict[str, Any]] = []

    @staticmethod
    def program_cycle_status() -> dict[str, Any]:
        raise AssertionError("recognized Stop synchronously read Program state")

    def execution_recognized_stop(self, **call: Any) -> dict[str, Any]:
        self.malformed.append(call)
        route = call["recognized_route"]
        applied = "halt" if route == "/api/execution/halt" else "stop_all"
        # The Kernel records the kind the route names (contract §5.1).
        requested = {"/api/execution/stops": "operation"}.get(route, applied)
        return {
            "ok": True,
            "command": "recognized_stop",
            "accepted": {
                "request_key": call["request_key"],
                "requested": requested,
                "applied": applied,
                "widen_reason": "malformed",
            },
        }

    def _unexpected(self, *args: Any) -> Any:
        raise AssertionError("an invalid payload reached its owner call")

    execution_stop = execution_stop_all = execution_halt = _unexpected
    program_run_motion_stop = _unexpected
    execution_reset = _unexpected


_CONTEXT = {"source_label": "test"}


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
@pytest.mark.parametrize(
    "kind,payload,route,applied",
    [
        (
            "operation_stop",
            {
                "request_context": _CONTEXT,
                "operations": [
                    {"operation_id": 2, "expected_operation_revision": 1},
                    {"operation_id": 1, "expected_operation_revision": 1},
                ],
            },
            "/api/execution/stops",
            "stop_all",
        ),
        (
            "stop_all",
            {"request_context": _CONTEXT, "expected_stop_generation": 0},
            "/api/execution/stop-all",
            "stop_all",
        ),
        (
            "halt",
            {"request_context": _CONTEXT, "program_run_id": 1},
            "/api/execution/halt",
            "halt",
        ),
        (
            "program_motion_stop",
            {"program_run_id": "program-run.1"},
            "/api/execution/stops",
            "stop_all",
        ),
    ],
)
def test_control_stop_with_invalid_payload_is_widened_by_its_route(
    transport_kind: str, kind: str, payload: dict[str, Any], route: str, applied: str
) -> None:
    service = _MalformedStopService()
    stamp = _stamp(transport_kind)
    dispatch = _dispatch(service)
    first, result = dispatch(_command(kind, payload, transport_kind), stamp)
    assert first.disposition == "received"
    assert result is not None
    assert result["accepted"]["applied"] == applied
    assert result["accepted"]["widen_reason"] == "malformed"
    # A repeat is its own request with its own key: a new Stop event.
    repeat, _ = dispatch(_command(kind, payload, transport_kind), stamp)
    assert repeat.disposition == "received"
    assert [call["recognized_route"] for call in service.malformed] == [route, route]
    assert {call["reason"] for call in service.malformed} == {
        "recognized_stop_payload_invalid"
    }
    # The envelope's validated label is the caller's label.
    assert {call["source_label"] for call in service.malformed} == {"test"}
    # Each acts with its own fresh Headless key (class 1, minter 5).
    first_key, second_key = (call["request_key"] for call in service.malformed)
    assert first_key.startswith(HEADLESS_KEY_PREFIX)
    assert second_key.startswith(HEADLESS_KEY_PREFIX) and first_key != second_key


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
def test_control_non_stop_with_invalid_payload_is_still_refused(transport_kind: str) -> None:
    stamp = _stamp(transport_kind)
    invalid_reset = _command(
        "operation_reset",
        {"request_context": {**_CONTEXT, "request_key": "ab" * 16},
         "operation_id": 1, "expected_operation_revision": 0},
        transport_kind,
    )

    class Available(_MalformedStopService):
        @staticmethod
        def program_cycle_status(*, before_command: bool = False) -> dict[str, Any]:
            del before_command
            return {}

    service = Available()
    dispatch = _dispatch(service)
    dispatch._availability = lambda cycle, kind: (True, None)  # type: ignore[method-assign]
    dispatch._manual_scope_conflict = lambda request, cycle: None  # type: ignore[method-assign]
    with pytest.raises(HeadlessApiError) as refused:
        dispatch(invalid_reset, stamp)
    assert refused.value.code == "execution_control_payload_invalid"
    assert service.malformed == []


class _OrdinaryStopService(_MalformedStopService):
    """Records the ordinary Stop calls a valid Stop makes."""

    def __init__(self) -> None:
        super().__init__()
        self.ordinary: list[tuple[str, Any]] = []

    def execution_stop(self, request: Any, attribution: Any) -> dict[str, Any]:
        del attribution
        self.ordinary.append(("stop", request))
        return {"ok": True, "command": "stop", "accepted": {}}

    def execution_stop_all(self, request: Any, attribution: Any) -> dict[str, Any]:
        del attribution
        self.ordinary.append(("stop_all", request))
        return {"ok": True, "command": "stop_all", "accepted": {}}

    def execution_halt(self, request: Any, attribution: Any) -> dict[str, Any]:
        del attribution
        self.ordinary.append(("halt", request))
        return {"ok": True, "command": "halt", "accepted": {}}


_STOP_PAYLOADS = {
    "operation_stop": {
        "request_context": _CONTEXT,
        "operations": [{"operation_id": 1, "expected_operation_revision": 1}],
    },
    "stop_all": {"request_context": _CONTEXT},
    "halt": {"request_context": _CONTEXT},
}


@pytest.mark.parametrize("transport_kind", ["websocket", "rest"])
@pytest.mark.parametrize("kind", ["operation_stop", "stop_all", "halt"])
def test_control_stop_without_key_is_an_ordinary_stop(
    transport_kind: str, kind: str
) -> None:
    service = _OrdinaryStopService()
    disposition, _ = _dispatch(service)(
        _command(kind, dict(_STOP_PAYLOADS[kind]), transport_kind),
        _stamp(transport_kind),
    )
    assert disposition.disposition == "received"
    assert service.malformed == []
    ((called, request),) = service.ordinary
    assert called == ("stop" if kind == "operation_stop" else kind)
    if transport_kind == "websocket":
        # Class 0: the session and its client sequence are the key.
        assert request.request_context.request_key == (
            "00" + "00000000000001" + "0000000000000001"
        )
        assert disposition.request_key == request.request_context.request_key
    else:
        # A Stop without a key acts as a fresh Stop with a Headless key (Q8).
        assert request.request_context.request_key.startswith(HEADLESS_KEY_PREFIX)
        assert disposition.request_key == request.request_context.request_key


@pytest.mark.parametrize("kind", ["stop_all", "halt"])
def test_stop_all_and_halt_need_no_program_and_no_start_value(kind: str) -> None:
    """Stop all and Halt name no run: they never read the Program cycle
    (``program_cycle_status`` fails the test) and a session-less one needs no
    ``A``, so they act with no Program loaded."""

    service = _OrdinaryStopService()
    request = ExecutionCommandRequestDTO.model_validate(
        {
            **_command(kind, {}, "rest").model_dump(mode="json"),
            "payload": {"request_context": _CONTEXT},
        }
    )
    disposition, _ = _dispatch(service)(request, _stamp("rest"))
    assert disposition.disposition == "received"
    assert [called for called, _ in service.ordinary] == [kind]
