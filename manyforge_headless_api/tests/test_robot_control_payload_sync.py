from __future__ import annotations

import pytest

from manyforge_headless_api.control_dispatch import HeadlessExecutionControlDispatch
from manyforge_headless_api.control_protocol import (
    EXECUTION_COMMAND_SCHEMA,
    ControlFreshnessAuthority,
    ExecutionCommandRequestDTO,
    ExecutionIngressStamp,
)
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.identity import RequestAdmission
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.test_execution_events import (
    _direct_command_request,
    _direct_motion_request,
)


class _Service:
    def __init__(self, execution):
        self.execution = execution

    def _execution(self):
        if self.execution is None:
            raise HeadlessApiError("execution_owner_unavailable", "owner unavailable")
        return self.execution

    @staticmethod
    def program_cycle_status(**_kwargs):
        return {
            "execution_control_revision": 7,
            "cycle_generation": 3,
            "program_run_id": None,
            "state": "idle_after_reset",
            "allowed_commands": [{"command_kind": "operation_start", "allowed": True,
                                  "disabled_reason": None}],
        }

    def execution_submit(self, request, attribution):
        return self._execution().submit(request, trusted_principal_id=attribution.principal_id)

    def execution_stop(self, request, attribution):
        return self._execution().stop(request, trusted_principal_id=attribution.principal_id)


_APPLICATION_START_ID = "3f9a0000000000a1"


def _dispatch(service, payload, kind="operation_start"):
    # A WebSocket command: its session and sequence are its key (class 0)
    # and the session belongs to one start, so it names no ``A``.
    freshness = ControlFreshnessAuthority(secret=b"x" * 32).issue(
        execution_control_revision=7
    )
    request = ExecutionCommandRequestDTO.model_validate(
        {
            "command_envelope": {
                "schema_version": EXECUTION_COMMAND_SCHEMA,
                "command_kind": kind,
                "source_label": "test",
                "control_session_number": 1,
                "client_sequence": "1",
                "freshness": freshness.model_dump(mode="json"),
                "expected_execution_control_revision": 7,
            },
            "payload": payload,
        }
    )
    return HeadlessExecutionControlDispatch(
        service, RequestAdmission(_APPLICATION_START_ID)
    )(
        request,
        ExecutionIngressStamp(transport_kind="websocket",
                              received_server_sequence=1, received_monotonic_ns=1),
    )


def _payload(model):
    return model.model_dump(mode="json")


def _client(exchange, calls, result):
    client = object.__new__(ExecutionCommandClient)
    client._exchange = exchange
    client.submit = lambda request, **_kwargs: (calls.append("submit") or result)
    client.stop = lambda request, **_kwargs: (calls.append("stop") or {"ok": True})
    return client


@pytest.mark.parametrize("native_ok", [True, False])
def test_direct_motion_reaches_native_submit_without_bridge(native_ok):
    calls = []
    result = ({"ok": True, "accepted": {}} if native_ok else
              {"ok": False, "error": {"code": "component_not_ready"}})

    def exchange(_command):
        pytest.fail("unexpected execution bridge request")

    response, body = _dispatch(_Service(_client(exchange, calls, result)),
                               _payload(_direct_motion_request()))
    assert calls == ["submit"]
    assert body is result
    assert response.disposition == ("received" if native_ok else "rejected")
    assert response.code == ("operation_start_received" if native_ok else "component_not_ready")


def test_gripper_does_not_sync():
    calls = []
    client = _client(lambda _command: pytest.fail("unexpected payload sync"),
                     calls, {"ok": True, "accepted": {}})
    response, _ = _dispatch(_Service(client), _payload(_direct_command_request()))
    assert response.disposition == "received"
    assert calls == ["submit"]


def test_set_tcp_does_not_sync():
    from manyforge_headless_api.models import OperationIntentRequest
    from manyforge_headless_api.tests.test_models import _planning_operation

    body = _planning_operation("set-tcp", {
        "kind": "set_tcp",
        "tcp": {"kind": "default_of_target",
                "target": {"component_id": "robot", "command_target_id": "arm"}},
    })
    calls = []
    client = _client(lambda _command: pytest.fail("unexpected payload sync"),
                     calls, {"ok": True, "accepted": {}})
    response, _ = _dispatch(_Service(client), _payload(OperationIntentRequest.model_validate(body)))
    assert response.disposition == "received"
    assert calls == ["submit"]


def test_stop_does_not_sync():
    calls = []
    client = _client(lambda _command: pytest.fail("unexpected payload sync"), calls, {})
    response, _ = _dispatch(_Service(client), {
        "components": [{"controllable_component_id": "robot", "expected_graph_revision": "graph",
                        "expected_component_lifecycle_generation": 1}],
    }, kind="operation_stop")
    assert response.disposition == "received"
    assert calls == ["stop"]


def test_invalid_motion_is_rejected_before_submit():
    calls = []
    client = _client(lambda _command: pytest.fail("unexpected payload sync"), calls, {})
    payload = _payload(_direct_motion_request())
    payload["participants"] = []
    with pytest.raises(HeadlessApiError) as exc:
        _dispatch(_Service(client), payload)
    assert exc.value.code == "execution_control_payload_invalid"
    assert calls == []


def test_unavailable_motion_is_rejected_before_submit():
    calls = []
    client = _client(lambda _command: pytest.fail("unexpected payload sync"), calls, {})
    service = _Service(client)
    status = service.program_cycle_status()
    status["allowed_commands"][0].update(allowed=False, disabled_reason="command_not_allowed")
    service.program_cycle_status = lambda **_kwargs: status
    response, _ = _dispatch(service, _payload(_direct_motion_request()))
    assert response.disposition == "rejected"
    assert response.code == "command_not_allowed"
    assert calls == []


def test_mixed_operation_reaches_native_submit():
    calls = []
    motion = _payload(_direct_motion_request())
    gripper = _payload(_direct_command_request())
    motion["participants"].append(gripper["participants"][0])
    motion["coordination_policy"] = "coordinated_all"
    client = _client(lambda _command: pytest.fail("unexpected execution bridge request"),
                     calls, {"ok": True, "accepted": {}})
    response, _ = _dispatch(_Service(client), motion)
    assert response.disposition == "received"
    assert calls == ["submit"]
