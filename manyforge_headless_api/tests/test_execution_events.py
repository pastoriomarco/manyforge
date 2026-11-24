from __future__ import annotations

import copy
import hashlib
import json
import logging
import threading
from pathlib import Path
from typing import Any

import pytest
import rfc8785

from manyforge_headless_api.tests.paged_event_stream import paged_subscription
from manyforge_headless_api.execution import (
    RETRY_ELIGIBILITY_REASONS,
    ExecutionCommandClient,
    ExecutionEventPump,
)
from manyforge_headless_api.models import (
    FullStopIntentRequest,
    OperationIntentRequest,
    OperationRecoveryActionRequest,
    ProgramRunStopIntentRequest,
    StopIntentRequest,
    RecoveryPreflightConfirmation,
    RuntimeScaleControlRequest,
)
from manyforge_headless_api.native import HeadlessApiError


def test_recover_enable_preflight_and_confirmation_forward_exact_native_values(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    commands = []
    preflight = {"snapshot_id": "native-snapshot", "components": [
        {"component_id": "machine", "cause": {"code": "estop", "message": "E-stop",
         "vendor_code": "C2", "clearable": True, "operator_action": "recover", "concerns": ["arm"]},
         "stop": {"code": "estop", "message": "E-stop", "since_ns": 1, "vendor_code": "C2"}, "eligibility": "recover"},
        {"component_id": "arm", "owner_id": "machine", "cause": None, "eligibility": "ready"},
    ]}
    def exchange(command):
        commands.append(command)
        if command["command"] == "component_recovery_preflight":
            return {"ok": True, "command": command["command"], "preflight": preflight}
        return {"ok": True, "command": "component_action", "accepted": {"action": "enable"}}
    monkeypatch.setattr(client, "_exchange_component", exchange)
    assert client.component_recovery_preflight("arm")["preflight"] == preflight
    confirmation = {"snapshot_id": "native-snapshot", "confirmed": [
        {"component_id": "machine", "cause_code": "estop", "vendor_code": "C2", "stop_code": "estop", "stop_vendor_code": "C2", "eligibility": "recover"},
        {"component_id": "arm", "cause_code": None, "vendor_code": None, "stop_code": None, "stop_vendor_code": None, "eligibility": "ready"},
    ]}
    assert RecoveryPreflightConfirmation.model_validate(confirmation).model_dump(mode="json") == confirmation
    client.component_recover_enable("arm", confirmation)
    assert commands == [
        {"api_version": "manyforge.execution_command.v4", "command": "component_recovery_preflight",
         "request": {"component_id": "arm"}},
        {"api_version": "manyforge.execution_command.v4", "command": "component_action", "request": {
            "component_id": "arm", "action": "enable", "recovery_preflight": confirmation}},
    ]


@pytest.mark.parametrize("eligibility", ["blocked", "unknown", "excluded"])
def test_recover_enable_rejects_non_actionable_confirmations(eligibility):
    with pytest.raises(ValueError):
        RecoveryPreflightConfirmation.model_validate({"snapshot_id": "snapshot", "confirmed": [
            {"component_id": "arm", "cause_code": None, "vendor_code": None, "stop_code": None, "stop_vendor_code": None, "eligibility": eligibility},
        ]})


def test_unknown_member_requires_a_confirmed_recoverable_owner():
    owner = {"component_id": "machine", "cause_code": "estop", "vendor_code": "C2",
             "stop_code": "estop", "stop_vendor_code": "C2", "eligibility": "recover"}
    member = {"component_id": "arm", "cause_code": None, "vendor_code": None,
              "stop_code": None, "stop_vendor_code": None, "eligibility": "unknown"}
    assert RecoveryPreflightConfirmation.model_validate({"snapshot_id": "snapshot", "confirmed": [owner, member]})
    with pytest.raises(ValueError):
        RecoveryPreflightConfirmation.model_validate({"snapshot_id": "snapshot", "confirmed": [dict(owner, eligibility="ready"), member]})


@pytest.mark.parametrize("component", [
    {"component_id": "other", "cause": None, "eligibility": "ready"},
    {"component_id": "arm", "cause": None, "eligibility": "invented"},
    {"component_id": "arm", "cause": None, "eligibility": "ready", "extra": True},
])
def test_recovery_preflight_rejects_wrong_or_malformed_native_projection(monkeypatch, tmp_path, component):
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange_component", lambda command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native-snapshot", "components": [component]},
    })
    with pytest.raises(HeadlessApiError) as exc:
        client.component_recovery_preflight("arm")
    assert exc.value.code == "execution_command_response_invalid"


def test_recovery_preflight_keeps_simultaneous_fault_and_stop(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    row = {"component_id": "arm", "eligibility": "recover", "cause": {
        "code": "communication", "message": "Fault", "clearable": True,
        "operator_action": "recover", "concerns": [],
    }, "stop": {"code": "estop", "message": "Stopped", "since_ns": 3, "vendor_code": "C2"}}
    monkeypatch.setattr(client, "_exchange_component", lambda _command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native", "components": [row]},
    })
    assert client.component_recovery_preflight("arm")["preflight"]["components"] == [row]


def test_recovery_preflight_rejects_stop_cause_without_stop_evidence(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    row = {"component_id": "arm", "eligibility": "recover", "cause": {
        "code": "estop", "message": "Stopped", "clearable": True,
        "operator_action": "recover", "concerns": [],
    }}
    monkeypatch.setattr(client, "_exchange_component", lambda _command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native", "components": [row]},
    })
    with pytest.raises(HeadlessApiError):
        client.component_recovery_preflight("arm")


# A runner key (class 1, minter 4, sequence 1).
_RUNNER_KEY = "40" + "00000000000004" + "0000000000000001"
# Headless's own key (class 1, minter 5, sequence 1).
_HEADLESS_KEY = "40" + "00000000000005" + "0000000000000001"
# Formed HTTP client keys (class 3) of two previews.
_PREVIEW_KEY = "c0" + "01" * 15
_GRIPPER_KEY = "c0" + "02" * 15
# The Stop's key (a WebSocket key: session 1, sequence 1).
_STOP_KEY = "00" + "00000000000001" + "0000000000000001"
# The application start the pump fakes run under (16 hex, contract 1.12).
_APPLICATION_START_ID = "3f9a0000000000a1"


def _event(sequence: int) -> dict[str, Any]:
    payload = {
        "api_version": "manyforge.operation_journal_record.v10",
        "intent": {},
        "operation_state": {
            "operation_id": 1,
            "operation_revision": 1,
            "participants": [],
            "program_binding_id": "binding.1",
            "program_run_id": 1,
            "state": "completed",
        },
        "record_kind": "state_observed",
        "result": None,
        "runtime_scale_status": None,
    }


    canonical = rfc8785.dumps(payload)
    return {
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:00.00000000{sequence}Z",
        "operation_id": 1,
        "operation_revision": 1,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": "state_observed",
        "request_key": None,
        "sequence": sequence,
    }


@pytest.mark.parametrize(
    ("producer", "expected_producer"),
    [("component_manager", "component_manager"), (None, "kernel")],
)
def test_execution_rpc_refusal_retains_optional_producer(
    producer: str | None, expected_producer: str
) -> None:
    error = {
        "code": "execution_command_invalid",
        "details": {"cause_code": "stale_generation"},
        "message": "request refused",
    }
    if producer is not None:
        error["producer"] = producer

    with pytest.raises(HeadlessApiError) as refused:
        ExecutionCommandClient._raise_error_response(
            {"error": error, "ok": False}
        )

    assert refused.value.code == "execution_command_invalid"
    assert refused.value.details == {"cause_code": "stale_generation"}
    assert refused.value.producer == expected_producer


def _progress_event(
    sequence: int,
    *,
    state: str,
    progress: float,
    operation_id: int = 1,
) -> dict[str, Any]:
    event = copy.deepcopy(_event(sequence))
    event["operation_id"] = operation_id
    operation = event["payload"]["operation_state"]
    operation["operation_id"] = operation_id
    operation["state"] = state
    operation["updated_at_ns"] = 100 + sequence
    operation["participants"] = [
        {
            "participant_id": {"operation_id": operation_id, "ordinal": 1},
            "progress": progress,
            "state": state,
            "terminal_error": None,
        }
    ]
    canonical = rfc8785.dumps(event["payload"])
    event["payload_digest"] = f"sha256:{hashlib.sha256(canonical).hexdigest()}"
    return event


def _stop_row(sequence: int, payload: dict[str, Any]) -> dict[str, Any]:
    """One Stop event row: no operation and no request key in its envelope
    (contract §5.11); the payload is ``manyforge.stop_event.v1``."""

    canonical = rfc8785.dumps(payload)
    return {
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:00.00000000{sequence}Z",
        "operation_id": None,
        "operation_revision": None,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": f"stop_event_{payload['row']}",
        "request_key": None,
        "sequence": sequence,
    }


def _stop_accepted_event(sequence: int) -> dict[str, Any]:
    return _stop_row(sequence, {
        "accepted_at_ns": 100,
        "applied": "stop_all",
        "attribution": {"assurance": "unverified", "principal_id": None,
                        "transport": "http"},
        "caused_by": None,
        "covered": [{"operation_id": 4, "participant_ordinal": 1, "started": True}],
        "event_id": 1,
        "fence_degraded": None,
        "group_count": 0,
        "groups": [],
        "hold_change": ["all_groups"],
        "hold_revision_after": 2,
        "program_run_id": None,
        "reason": "",
        "request_key": _STOP_KEY,
        "requested": "stop_all",
        "row": "accepted",
        "schema_version": "manyforge.stop_event.v1",
        "source_label": "operator_ui",
        "stale_diagnostics": [],
        "stale_total": 0,
        "widen_reason": "none",
    })


def _stop_completed_event(sequence: int) -> dict[str, Any]:
    return _stop_row(sequence, {
        "accepted_at_ns": 100,
        "applied": "stop_all",
        "completed_at_ns": 1_000_000_100,
        "covered": [{
            "containing_stage_index": 0, "containing_stage_kind": "controlled",
            "error_code": None, "escalated": False, "operation_id": 4,
            "participant_ordinal": 1, "phase": "stopped", "started": True,
            "stop_path_collision": False,
        }],
        "event_id": 1,
        "result": "stopped",
        "row": "completed",
        "schema_version": "manyforge.stop_event.v1",
    })


def _page(events: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "operation_progress": [],
        "admission_generation": 1,
        "api_version": "manyforge.execution_command_response.v6",
        "command": "events",
        "execution_owner_ready": True,
        "runtime_faults": {
            "operation_journal_write_fault": None,
            "program_runner_loss": None,
            "kernel_internal_fault": None,
        },
        "event_page": {
            "events": events,
            "has_more": False,
            "next_after_sequence": events[-1]["sequence"] if events else 0,
            "schema_version": "manyforge.execution_event_page.v3",
        },
        "ok": True,
    }


def test_execution_client_uses_exact_focused_projection_commands(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    commands: list[dict[str, Any]] = []
    observation_paths: list[Path] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        commands.append(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": command["command"],
            "ok": True,
        }

    def exchange_observation(
        command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        observation_paths.append(socket_path)
        return exchange(command)

    monkeypatch.setattr(client, "_exchange", exchange)
    monkeypatch.setattr(client, "_exchange_on", exchange_observation)
    client.identity()
    client.targets()
    client.control_state()
    client.observation()
    client.diagnostics()

    assert commands == [
        {"api_version": "manyforge.execution_command.v4", "command": command}
        for command in (
            "identity",
            "targets",
            "control_state",
            "observation",
            "status",
        )
    ]
    # One-shot observation snapshots share the ordinary request/reply lane;
    # only long-poll sessions use the dedicated observation socket.
    assert observation_paths == []

    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _command: {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "status",
            "ok": True,
        },
    )
    with pytest.raises(HeadlessApiError) as mismatched:
        client.identity()
    assert mismatched.value.code == "execution_command_response_invalid"


def test_execution_client_validates_event_cursor(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _page([_event(1)])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(0)
    assert page["next_after_sequence"] == 1
    assert page["admission_generation"] == 1

    response["admission_generation"] = 0
    with pytest.raises(HeadlessApiError) as invalid_admission:
        client.events(0)
    assert invalid_admission.value.code == "execution_command_response_invalid"
    response["admission_generation"] = 1

    response["event_page"]["next_after_sequence"] = 2
    with pytest.raises(HeadlessApiError) as invalid:
        client.events(0)
    assert invalid.value.code == "execution_command_response_invalid"

    response = _page([])
    response["event_page"]["has_more"] = True
    with pytest.raises(HeadlessApiError) as stalled:
        client.events(0)
    assert stalled.value.code == "execution_command_response_invalid"


def test_execution_client_accepts_only_paired_null_stop_event_identity(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    stop_event = _stop_accepted_event(1)
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page([stop_event])
    )
    page = client.events(0)
    assert page["events"][0]["operation_id"] is None
    assert page["events"][0]["operation_revision"] is None

    stop_event["operation_revision"] = 1
    with pytest.raises(HeadlessApiError) as mixed:
        client.events(0)
    assert mixed.value.code == "execution_command_response_invalid"

    stop_event["operation_revision"] = None
    # The request key lives in the payload; the row's envelope names none.
    for key in (_STOP_KEY, "stop-request.1", _STOP_KEY.upper()):
        stop_event["request_key"] = key
        with pytest.raises(HeadlessApiError) as keyed:
            client.events(0)
        assert keyed.value.code == "execution_command_response_invalid"
    stop_event["request_key"] = None

    ordinary = _event(1)
    ordinary["operation_id"] = None
    ordinary["operation_revision"] = None
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page([ordinary])
    )
    with pytest.raises(HeadlessApiError) as unattributed:
        client.events(0)
    assert unattributed.value.code == "execution_command_response_invalid"

    # Operation numbers are integers (contract 1.5): text is refused.
    for operation_id in ("operation.1", "1", 0, True):
        texted = _event(1)
        texted["operation_id"] = operation_id
        monkeypatch.setattr(
            client, "_exchange", lambda _command, texted=texted: _page([texted])
        )
        with pytest.raises(HeadlessApiError) as text_identity:
            client.events(0)
        assert text_identity.value.code == "execution_command_response_invalid"


def test_execution_client_accepts_stop_completion_as_stop_scoped_fact(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    terminal = _stop_completed_event(1)
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page([terminal])
    )
    page = client.events(0)
    assert page["events"][0]["record_kind"] == "stop_event_completed"
    assert page["events"][0]["request_key"] is None
    assert page["events"][0]["operation_id"] is None

    terminal["request_key"] = _STOP_KEY
    with pytest.raises(HeadlessApiError) as invalid_request_identity:
        client.events(0)
    assert invalid_request_identity.value.code == ("execution_command_response_invalid")


def test_execution_client_requires_backend_residency_projection(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "read",
        "ok": True,
        "operation": {"operation_id": 1},
        "resident": True,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.read(1)["resident"] is True

    response.pop("resident")
    with pytest.raises(HeadlessApiError) as missing:
        client.read(1)
    assert missing.value.code == "execution_command_response_invalid"

    response["resident"] = 1
    with pytest.raises(HeadlessApiError) as non_boolean:
        client.read(1)
    assert non_boolean.value.code == "execution_command_response_invalid"


def test_execution_client_uses_internal_attribution_for_program_run_stop(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    request = ProgramRunStopIntentRequest.model_validate(
        {
            "request_context": {
                "request_key": _RUNNER_KEY,
                "source_label": "manyforge_behavior",
            },
            "program_run_id": 1,
        }
    )
    client = ExecutionCommandClient(
        tmp_path,
        attribution_transport="internal",
    )
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "stop_program_run",
            "ok": True,
            "accepted": {
                "request_key": request.request_context.request_key,
            },
        }

    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.stop_program_run(request)["ok"] is True
    assert captured == {
        "api_version": "manyforge.execution_command.v4",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "stop_program_run",
        # The host's Program-run end body: exactly the context and the run.
        "request": {
            "program_run_id": 1,
            "request_context": {
                "request_key": _RUNNER_KEY,
                "source_label": "manyforge_behavior",
            },
        },
    }

    public_client = ExecutionCommandClient(tmp_path)
    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        public_client.stop_program_run(request)
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def _ordinary_lane_forbidden(command: dict[str, Any]) -> dict[str, Any]:
    raise AssertionError(f"{command['command']} reached the ordinary socket")


def test_execution_client_sends_runtime_scale_on_the_control_socket(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    captured: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.append(command)
        return {"ok": True, "command": "set_runtime_scale_control", "accepted": {}}

    # Speed scaling travels beside Stop, never on the ordinary socket.
    monkeypatch.setattr(client, "_exchange", _ordinary_lane_forbidden)
    monkeypatch.setattr(client, "_exchange_control", exchange)
    request = RuntimeScaleControlRequest.model_validate(
        {
            "request_context": {
                "request_key": _RUNNER_KEY,
                "source_label": "test",
            },
            "selector": {
                "program_binding_id": "binding-1",
                "component_id": "robot",
                "command_target_id": "arm",
            },
            "expected_control_generation": 0,
            "scale": 0.4,
        }
    )
    assert client.set_runtime_scale_control(request, trusted_principal_id=None)["ok"] is True
    assert [command["command"] for command in captured] == ["set_runtime_scale_control"]
    assert captured[0]["request"] == request.model_dump(mode="json")


def _stop_status(**changes: Any) -> dict[str, Any]:
    return {
        "accepted_at_ns": 100,
        "applied": "halt",
        "completed_at_ns": 200,
        "covered": [{
            "operation_id": 4, "participant_ordinal": 1, "started": True,
            "done": True, "phase": "stopped", "containing_stage_index": 0,
            "escalated": False, "stop_path_collision": False, "error_code": None,
        }],
        "event_id": 1,
        "request_key": _RUNNER_KEY,
        "requested": "halt",
        "result": "stopped",
        "schema_version": "manyforge.execution_stop_status.v2",
        "state": "complete",
        "widen_reason": "none",
        **changes,
    }


def test_execution_client_reads_one_internal_stop_event(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path, attribution_transport="internal")
    captured: dict[str, Any] = {}
    status = _stop_status()

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "read_stop",
            "ok": True,
            "stop_status": copy.deepcopy(status),
        }

    # A Stop event is read beside Stop on the control socket, never on the
    # ordinary one.
    monkeypatch.setattr(client, "_exchange", _ordinary_lane_forbidden)
    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.read_stop(1)["stop_status"]["state"] == "complete"
    assert captured == {
        "api_version": "manyforge.execution_command.v4",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "read_stop",
        "event_id": 1,
    }
    # A pending event has no completion time yet.
    status.update(state="pending", result="pending", completed_at_ns=None)
    assert client.read_stop(1)["stop_status"]["state"] == "pending"
    for invalid in (
        {"state": "complete", "completed_at_ns": None},
        {"state": "pending", "completed_at_ns": 5},
        {"event_id": 2},
        {"result": "succeeded"},
        {"state": "succeeded"},
        {"schema_version": "manyforge.execution_stop_status.v1"},
        {"request_key": "program-stop.1"},
        # Removed Stop identity fields are not accepted beside the event id.
        {"stop_id": 1},
        {"stop_generation": 1},
    ):
        status.clear()
        status.update(_stop_status(**invalid))
        with pytest.raises(HeadlessApiError):
            client.read_stop(1)
    # Overwritten in the ring: the Kernel's refusal passes through unchanged.
    monkeypatch.setattr(
        client,
        "_exchange_control",
        lambda command: {
            "api_version": "manyforge.execution_command_response.v6",
            "error": {"code": "stop_status_not_resident", "message": "gone"},
            "ok": False,
        },
    )
    assert client.read_stop(1)["error"]["code"] == "stop_status_not_resident"
    with pytest.raises(ValueError):
        client.read_stop("stop.1")  # type: ignore[arg-type]

    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        ExecutionCommandClient(tmp_path).read_stop(1)
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def test_execution_client_releases_a_halt_at_its_hold_revision(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path, attribution_transport="internal")
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "release_halt",
            "ok": True,
            "scope": {"schema_version": "manyforge.program_execution_scope.v4"},
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.release_halt(1, 0)["ok"] is True
    assert captured == {
        "api_version": "manyforge.execution_command.v4",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "release_halt",
        "expected_hold_revision": 0,
        "program_run_id": 1,
    }
    for run, revision in (("program-run.1", 0), (0, 0), (1, -1), (1, True)):
        with pytest.raises(ValueError):
            client.release_halt(run, revision)  # type: ignore[arg-type]
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda command: {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "release_halt",
            "ok": True,
        },
    )
    with pytest.raises(HeadlessApiError):
        client.release_halt(1, 0)

    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        ExecutionCommandClient(tmp_path).release_halt(1, 0)
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def test_execution_client_projects_scoped_and_cell_wide_stop_commands(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    context = {
        "request_key": _STOP_KEY,
        "source_label": "operator_ui",
    }
    stop = StopIntentRequest.model_validate(
        {
            "request_context": context,
            "components": [
                {
                    "controllable_component_id": "robot.1",
                    "expected_graph_revision": "sha256:" + "1" * 64,
                    "expected_component_lifecycle_generation": 2,
                }
            ],
            "reason": None,
        }
    )
    cell_wide = FullStopIntentRequest.model_validate({"request_context": context})
    control_commands: list[dict[str, Any]] = []

    def control_exchange(command: dict[str, Any]) -> dict[str, Any]:
        control_commands.append(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "accepted": {"event_id": len(control_commands)},
            "command": command["command"],
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange", _ordinary_lane_forbidden)
    monkeypatch.setattr(client, "_exchange_control", control_exchange)
    assert client.stop(stop, trusted_principal_id=None)["ok"] is True
    assert client.stop_all(cell_wide, trusted_principal_id=None)["ok"] is True
    assert client.halt(cell_wide, trusted_principal_id=None)["ok"] is True
    for route in ("/api/execution/stops", "/api/execution/stop-all", "/api/execution/halt"):
        assert (
            client.recognized_stop(
                request_key=_HEADLESS_KEY,
                recognized_route=route,
                reason="recognized_stop_payload_invalid",
                source_label=None,
                trusted_principal_id=None,
            )["ok"]
            is True
        )
    assert [command["command"] for command in control_commands] == [
        "stop", "stop_all", "halt", "recognized_stop", "recognized_stop", "recognized_stop",
    ]
    assert set(control_commands[0]["request"]) == {
        "components",
        "request_context",
    }
    assert control_commands[0]["request"]["request_context"] == context
    # Stop all and Halt carry the request context alone (no generation).
    assert control_commands[1]["request"] == {"request_context": context}
    assert control_commands[2]["request"] == {"request_context": context}
    assert control_commands[3]["request_key"] == _HEADLESS_KEY
    assert [command.get("recognized_route") for command in control_commands[3:]] == [
        "/api/execution/stops", "/api/execution/stop-all", "/api/execution/halt",
    ]
    assert "request_id" not in control_commands[3]
    assert [command["source_label"] for command in control_commands[3:]] == [None] * 3
    assert client.recognized_stop(
        request_key=_HEADLESS_KEY,
        recognized_route="/api/execution/stops",
        reason="recognized_stop_start_not_current",
        source_label="operator_ui",
        trusted_principal_id=None,
    )["ok"] is True
    assert {
        key: control_commands[6][key]
        for key in ("command", "reason", "recognized_route", "source_label")
    } == {
        "command": "recognized_stop",
        "reason": "recognized_stop_start_not_current",
        "recognized_route": "/api/execution/stops",
        "source_label": "operator_ui",
    }

    # A reply naming another command is refused.
    monkeypatch.setattr(
        client,
        "_exchange_control",
        lambda command: {
            "api_version": "manyforge.execution_command_response.v6",
            "accepted": {},
            "command": "stop",
            "ok": True,
        },
    )
    for call in (
        lambda: client.stop_all(cell_wide, trusted_principal_id=None),
        lambda: client.halt(cell_wide, trusted_principal_id=None),
        lambda: client.recognized_stop(
            request_key=_HEADLESS_KEY,
            recognized_route="/api/execution/halt",
            reason="recognized_stop_payload_invalid",
            source_label=None,
            trusted_principal_id=None,
        ),
    ):
        with pytest.raises(HeadlessApiError):
            call()


def test_execution_client_cell_stop_is_internal_only(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """The Program node's cell Stop (``kernel.cell_stop``) is the runner's."""

    context = {"request_key": _RUNNER_KEY, "source_label": "program_runner"}
    request = FullStopIntentRequest.model_validate({"request_context": context})
    client = ExecutionCommandClient(tmp_path, attribution_transport="internal")
    captured: list[dict[str, Any]] = []

    def control_exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.append(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "accepted": {"event_id": 3},
            "command": "cell_stop",
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange_control", control_exchange)
    assert client.cell_stop(request)["accepted"]["event_id"] == 3
    assert captured == [{
        "api_version": "manyforge.execution_command.v4",
        "attribution": {"transport": "internal", "assurance": "unverified",
                        "principal_id": None},
        "command": "cell_stop",
        "request": {"request_context": context},
    }]
    with pytest.raises(HeadlessApiError) as public:
        ExecutionCommandClient(tmp_path).cell_stop(request)
    assert public.value.code == "execution_request_attribution_invalid"


def _direct_motion_request() -> OperationIntentRequest:
    return OperationIntentRequest.model_validate(
        {
            "request_context": {
                "request_key": _PREVIEW_KEY,
                "source_label": "composer_ui",
            },
            "program_revision": "sha256:program",
            "program_binding_id": "sha256:binding",
            "program_run_id": None,
            "expected_graph_revision": "sha256:graph",
            "expected_retained_bundle_id": "sha256:bundle",
            "expected_scene_revision": None,
            "participants": [
                {
                    "target": {"component_id": "controlled_component",
                               "command_target_id": "command_target"},
                    "required_capability_id": "execution.trajectory.execute",
                    "action_span": None,
                    "payload": {
                        "kind": "motion",
                        "move_id": "robot-control-preview-request-1",
                        "base_frame": {"component_id": "controlled_component", "frame_id": "base"},
                        "tcp_endpoint": {"kind": "current"},
                        "tolerances": {
                            "joint_position_tol_rad": 0.05,
                            "translation_tol_m": 0.005,
                            "rotation_tol_rad": 0.05,
                        },
                        "speed_profile": {
                            "velocity_scale": 1.0,
                            "acceleration_scale": 1.0,
                            "max_tcp_linear_speed_mps": None,
                        },
                        "timeouts": {
                            "planning_timeout_s": 2.0,
                            "execution_timeout_s": 10.0,
                        },
                        "cache_mode": "default",
                        "target_mode": "absolute",
                        "target_reference": None,
                        "offset_reference": None,
                        "motion": {
                            "kind": "joint_goal",
                            "joint_names": ["joint1"],
                            "positions_rad": [0.2],
                        },
                        "collision_rules": [],
                        "per_joint_dynamics_limits": {},
                        "motion_interrupt_scopes": [],
                        "runtime_object_bindings": [],
                    },
                }
            ],
            "coordination_policy": "independent",
        }
    )


def _preview_response(operation: dict[str, Any]) -> dict[str, Any]:
    receipt = {
        "collision_world_revision": {
            "content_hash": "collision-4",
            "revision_id": 4,
        },
        "commit_eligible": True,
        "target": copy.deepcopy(operation["participants"][0]["target"]),
        "final_positions_rad": [0.2],
        "final_tcp_pose": {
            "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.4},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        },
        "graph_revision": "sha256:graph",
        "intent_digest": (
            "sha256:" + hashlib.sha256(rfc8785.dumps(operation)).hexdigest()
        ),
        "program_binding_id": "sha256:binding",
        "program_revision": "sha256:program",
        "program_run_id": None,
        "request_key": _PREVIEW_KEY,
        "required_capability_id": "execution.trajectory.execute",
        "retained_bundle_id": "sha256:bundle",
        "scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 3,
            "content_hash": "sha256:" + "2" * 64,
        },
        "schema_version": "manyforge.direct_control_motion_preview.v5",
        "resolved_tcp": {
            "entry": {
                "target": {"component_id": "robot", "command_target_id": "arm"},
                "frame": {"component_id": "robot", "frame_id": "flange"},
            },
            "source": "active_tcp",
            "registration_generation": 1, "sequence": 0,
        },
        "path_preview": _bounded_motion_path(),
        "source_joint_names": ["joint1"],
        "start_positions_rad": [0.0],
    }
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "preview_motion",
        "ok": True,
        "preview_receipt": receipt,
    }


def test_motion_request_keeps_mounted_tcp_owner_and_direct_frame() -> None:
    value = _direct_motion_request().model_dump(mode="json")
    motion = value["participants"][0]["payload"]
    motion["tcp_endpoint"] = {
        "kind": "entry",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "frame": {"component_id": "tool", "frame_id": "tcp"},
    }
    parsed = OperationIntentRequest.model_validate(value).model_dump(mode="json")
    assert parsed["participants"][0]["payload"]["tcp_endpoint"] == motion["tcp_endpoint"]
    from manyforge_headless_api.models import ComponentFrameReferenceRequest
    frame = {"type": "component_frame", "frame": {"component_id": "tool", "frame_id": "tcp"}}
    assert ComponentFrameReferenceRequest.model_validate(frame).model_dump() == frame


def _direct_command_request() -> OperationIntentRequest:
    value = _direct_motion_request().model_dump(mode="json")
    value["request_context"]["request_key"] = _GRIPPER_KEY
    value["program_revision"] = "sha256:gripper-program"
    value["program_binding_id"] = "sha256:gripper-binding"
    participant = value["participants"][0]
    participant["required_capability_id"] = "tool.gripper.command"
    participant["payload"] = {
        "kind": "capability_command",
        "command_schema_id": "gripper.command_position",
        "command_schema_revision": "sha256:gripper-schema",
        "command_value": {
            "kind": "gripper_position",
            "position": {"kind": "linear_m", "value_m": 0.03},
            "max_effort": None,
        },
        "completion_timeout_s": 2.0,
        "caller_options": [{"option_id": "command_duration_s", "value_s": 0.5}],
    }
    return OperationIntentRequest.model_validate(value)


def _command_preview_response(operation: dict[str, Any]) -> dict[str, Any]:
    receipt = {
        "command_schema_id": "gripper.command_position",
        "command_schema_revision": "sha256:gripper-schema",
        "commit_eligible": True,
        "target": copy.deepcopy(operation["participants"][0]["target"]),
        "coordinate_kind": "linear_m",
        "graph_revision": "sha256:graph",
        "intent_digest": (
            "sha256:" + hashlib.sha256(rfc8785.dumps(operation)).hexdigest()
        ),
        "max_position": 0.04,
        "min_position": 0.0,
        "program_binding_id": "sha256:gripper-binding",
        "program_revision": "sha256:gripper-program",
        "program_run_id": None,
        "request_key": _GRIPPER_KEY,
        "required_capability_id": "tool.gripper.command",
        "retained_bundle_id": "sha256:bundle",
        "schema_version": ("manyforge.direct_control_capability_command_preview.v3"),
        "source_coordinate_name": "finger_joint",
        "source_kind": "model_role",
        "target_position": 0.03,
    }
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "preview_capability_command",
        "ok": True,
        "preview_receipt": receipt,
    }


@pytest.mark.parametrize(
    "case",
    [
        "idle",
        "unused",
        "held",
        "large",
        "stale",
        "malformed",
        "extra",
        "blocked",
        "cleanup",
        "cleanup_without_parent",
        "bad_availability",
    ],
)
def test_manual_handover_query_is_one_bounded_shape_checked_read(
    tmp_path, monkeypatch, case
):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    captured = []

    def exchange(command):
        captured.append(command)
        operation = command["operation"]
        result = {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "read_manual_handover",
            "ok": True,
            "availability": {
                "available": True,
                "cleanup_required": False,
                "unavailable_reason": None,
            },
            "request_key": request.request_context.request_key,
            "intent_digest": "sha256:"
            + hashlib.sha256(rfc8785.dumps(operation)).hexdigest(),
            "handover": (
                None
                if case == "idle"
                else {
                    "context": {
                        "program_run_id": 3,
                        "claim_generation": 1,
                        "hold_revision": 0 if case == "unused" else 3,
                    },
                    "producer_node_indices": (
                        []
                        if case == "unused"
                        else list(range(100_000)) if case == "large" else [1, 3]
                    ),
                }
            ),
        }
        if case == "stale":
            result["intent_digest"] = "sha256:" + "0" * 64  # Echo, not compared.
        elif case == "malformed":
            result["handover"]["producer_node_indices"] = [True]
        elif case == "extra":
            result["permission"] = True
        elif case == "blocked":
            result["availability"] = {
                "available": False,
                "cleanup_required": False,
                "unavailable_reason": "manual_control_target_busy",
            }
        elif case in {"cleanup", "cleanup_without_parent"}:
            result["availability"]["cleanup_required"] = True
            if case == "cleanup_without_parent":
                result["handover"] = None
        elif case == "bad_availability":
            result["availability"]["unavailable_reason"] = "contradictory"
        assert len(rfc8785.dumps(result)) < 4 * 1024 * 1024
        return result

    monkeypatch.setattr(client, "_exchange", exchange)
    if case in {
        "malformed",
        "extra",
        "cleanup_without_parent",
        "bad_availability",
    }:
        with pytest.raises(HeadlessApiError):
            client.read_manual_handover(request, trusted_principal_id=None)
    else:
        result = client.read_manual_handover(request, trusted_principal_id=None)
        assert (result["handover"] is None) == (case == "idle")
    assert len(captured) == 1
    assert captured[0]["command"] == "read_manual_handover"
    assert captured[0]["operation"] == request.model_dump(mode="json")


def test_execution_client_verifies_complete_motion_preview_receipt(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return _preview_response(command["operation"])

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.preview_motion(request, trusted_principal_id=None)

    assert captured["command"] == "preview_motion"
    assert captured["attribution"] == {
        "transport": "http",
        "assurance": "unverified",
        "principal_id": None,
    }
    assert response["preview_receipt"]["final_positions_rad"] == [0.2]

    def malformed(command: dict[str, Any]) -> dict[str, Any]:
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["source_joint_names"] = [["unhashable"]]
        receipt.pop("receipt_digest")
        receipt["receipt_digest"] = (
            "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        )
        return response

    monkeypatch.setattr(client, "_exchange", malformed)
    with pytest.raises(HeadlessApiError) as invalid:
        client.preview_motion(request, trusted_principal_id=None)
    assert invalid.value.code == "execution_command_response_invalid"

    def undeclared(command: dict[str, Any]) -> dict[str, Any]:
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["trajectory"] = []
        receipt.pop("receipt_digest")
        receipt["receipt_digest"] = (
            "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        )
        return response

    monkeypatch.setattr(client, "_exchange", undeclared)
    with pytest.raises(HeadlessApiError) as invalid_extra:
        client.preview_motion(request, trusted_principal_id=None)
    assert invalid_extra.value.code == "execution_command_response_invalid"

    def reordered_source(command: dict[str, Any]) -> dict[str, Any]:
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["source_joint_names"] = ["other_joint"]
        receipt.pop("receipt_digest")
        receipt["receipt_digest"] = (
            "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        )
        return response

    monkeypatch.setattr(client, "_exchange", reordered_source)
    with pytest.raises(HeadlessApiError) as mismatched_source:
        client.preview_motion(request, trusted_principal_id=None)
    assert mismatched_source.value.code == "execution_command_response_invalid"

    def unsafe_collision_revision(command: dict[str, Any]) -> dict[str, Any]:
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["collision_world_revision"]["revision_id"] = 2**53
        return response

    monkeypatch.setattr(client, "_exchange", unsafe_collision_revision)
    with pytest.raises(HeadlessApiError) as unsafe_revision:
        client.preview_motion(request, trusted_principal_id=None)
    assert unsafe_revision.value.code == "execution_command_response_invalid"


def _with_scene_revision(
    response: dict[str, Any], scene_revision: dict[str, Any]
) -> dict[str, Any]:
    receipt = response["preview_receipt"]
    receipt["scene_revision"] = scene_revision
    receipt.pop("receipt_digest")
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return response


def test_motion_preview_receipt_names_the_latest_scene_revision(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    latest = {
        "scene_id": "sha256:" + "9" * 64,
        "revision_id": 40,
        "content_hash": "sha256:" + "3" * 64,
    }

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        return _with_scene_revision(_preview_response(command["operation"]), latest)

    monkeypatch.setattr(client, "_exchange", exchange)
    # The intent names no Scene revision; the owner plans on the latest one.
    response = client.preview_motion(request, trusted_principal_id=None)
    assert response["preview_receipt"]["scene_revision"] == latest


def test_capability_command_preview_receipt_names_no_scene_revision(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_command_request()
    revision = {
        "scene_id": "sha256:" + "1" * 64,
        "revision_id": 4,
        "content_hash": "sha256:" + "3" * 64,
    }

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        return _with_scene_revision(
            _command_preview_response(command["operation"]), revision
        )

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as undeclared:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert undeclared.value.code == "execution_command_response_invalid"


def test_execution_client_verifies_capability_command_preview_receipt(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_command_request()
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return _command_preview_response(command["operation"])

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.preview_capability_command(request, trusted_principal_id=None)

    assert captured["command"] == "preview_capability_command"
    assert response["preview_receipt"]["target_position"] == 0.03

    def wrong_target(command: dict[str, Any]) -> dict[str, Any]:
        response = _command_preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["target"]["component_id"] = "other"
        receipt.pop("receipt_digest")
        receipt["receipt_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        return response

    monkeypatch.setattr(client, "_exchange", wrong_target)
    with pytest.raises(HeadlessApiError):
        client.preview_capability_command(request, trusted_principal_id=None)

    def component_local(command: dict[str, Any]) -> dict[str, Any]:
        response = _command_preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["source_kind"] = "component_local"
        receipt["source_coordinate_name"] = "gripper_position"
        receipt["min_position"] = None
        receipt["max_position"] = None
        receipt.pop("receipt_digest")
        receipt["receipt_digest"] = (
            "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        )
        return response

    monkeypatch.setattr(client, "_exchange", component_local)
    local_response = client.preview_capability_command(
        request, trusted_principal_id=None
    )
    assert local_response["preview_receipt"]["source_kind"] == ("component_local")

    def wrong_kind(command: dict[str, Any]) -> dict[str, Any]:
        response = _command_preview_response(command["operation"])
        response["command"] = "preview_motion"
        return response

    monkeypatch.setattr(client, "_exchange", wrong_kind)
    with pytest.raises(HeadlessApiError) as invalid:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert invalid.value.code == "execution_command_response_invalid"


class _RecordingCore:
    def __init__(self) -> None:
        self.ingested: list[dict[str, Any]] = []
        self.completed_calls: list[dict[str, Any]] = []
        self.runtime_fault_snapshots: list[dict[str, Any]] = []
        self.completed = threading.Event()
        self.unknown = threading.Event()
        self.alarm_refreshes = 0
        self.alarm_refresh_failure: Exception | None = None
        self.operation_tables: list[dict[str, Any]] = []
        self.component_faults: list[list[dict[str, Any]]] = []

    def refresh_alarm_sources(self) -> dict[str, Any]:
        self.alarm_refreshes += 1
        if self.alarm_refresh_failure is not None:
            raise self.alarm_refresh_failure
        return {"refreshed": True}

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        self.ingested.append(kwargs)
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **kwargs: Any) -> dict[str, Any]:
        self.completed_calls.append(kwargs)
        self.completed.set()
        return {"status": "complete"}

    def observe_runtime_fault_conditions(self, **kwargs: Any) -> dict[str, Any]:
        self.runtime_fault_snapshots.append(kwargs)
        return {"status": "observed", "changed": False}

    def mark_operation_event_source_unknown(self, **_kwargs: Any) -> dict[str, Any]:
        self.unknown.set()
        return {"status": "marked_unknown"}

    def observe_operation_table(self, operations_json: str) -> dict[str, Any]:
        self.operation_tables.append(json.loads(operations_json))
        return {"changed": False, "fully_read": True,
                "schema_version": "manyforge.operation_table_observation.v1"}

    def observe_component_faults(self, faults_json: str) -> dict[str, Any]:
        self.component_faults.append(json.loads(faults_json))
        return {"status": "observed", "changed": False}


class _PagedExecution:
    def subscribe_events(self, after_cursor: int):
        # The pump reads the subscription only.
        return paged_subscription(self, after_cursor)

    def __init__(self) -> None:
        self.application_start_id = _APPLICATION_START_ID
        self.admission_generation = 1
        self.fail = False
        self.event = _event(1)
        self.progress: list[dict[str, Any]] = []

    def identity(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "identity",
            "ok": True,
            "identity": {"application_start_id": self.application_start_id},
        }

    def events(self, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.fail:
            raise HeadlessApiError("execution_command_unavailable", "host lost")
        assert limit == 1
        events = [] if after_sequence else [self.event]
        return {
            "execution_owner_ready": True,
            "runtime_faults": {
                "operation_journal_write_fault": None,
                "program_runner_loss": None,
                "kernel_internal_fault": None,
            },
            "operation_progress": self.progress,
            "admission_generation": self.admission_generation,
            "events": events,
            "has_more": False,
            "next_after_sequence": after_sequence or 1,
            "schema_version": "manyforge.execution_event_page.v3",
        }


class _StreamingExecution(_PagedExecution):
    def __init__(self) -> None:
        super().__init__()
        self._events: list[dict[str, Any]] = []
        self._lock = threading.Lock()
        # Stream v3 memory facts (OD-205 (b)); absent when None.
        self.operations: dict[str, Any] | None = None
        self.active_tcp_baseline: list[dict[str, Any]] | None = None

    def append(self, event: dict[str, Any]) -> None:
        with self._lock:
            self._events.append(event)

    def events(self, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        with self._lock:
            available = [
                event for event in self._events if event["sequence"] > after_sequence
            ]
            events = available[:limit]
        next_sequence = events[-1]["sequence"] if events else after_sequence
        memory_facts = {
            name: value
            for name, value in (("operations", self.operations),
                                ("active_tcp_baseline", self.active_tcp_baseline))
            if value is not None
        }
        return {
            **memory_facts,
            "execution_owner_ready": True,
            "runtime_faults": {
                "operation_journal_write_fault": None,
                "program_runner_loss": None,
                "kernel_internal_fault": None,
            },
            "operation_progress": self.progress,
            "admission_generation": self.admission_generation,
            "events": events,
            "has_more": len(available) > len(events),
            "next_after_sequence": next_sequence,
            "schema_version": "manyforge.execution_event_page.v3",
        }


class _IdleExecution:
    def subscribe_events(self, after_cursor: int):
        # The pump reads the subscription only.
        return paged_subscription(self, after_cursor)

    def __init__(self) -> None:
        self.application_start_id = _APPLICATION_START_ID
        self.admission_generation = 1
        self.status_calls = 0
        self.events_calls = 0

    def identity(self) -> dict[str, Any]:
        self.status_calls += 1
        details = {"application_start_id": self.application_start_id}
        return {
            "admission_generation": self.admission_generation,
            "api_version": "manyforge.execution_command_response.v6",
            "command": "identity",
            "error": {
                "code": "execution_owner_not_ready",
                "details": details,
                "message": "the managed execution owner has no active physical product",
            },
            "ok": False,
        }

    def events(self, _after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        self.events_calls += 1
        return {
            "execution_owner_ready": False,
            "runtime_faults": {
                "operation_journal_write_fault": None,
                "program_runner_loss": None,
                "kernel_internal_fault": None,
            },
            "events": [], "has_more": False, "next_after_sequence": _after_sequence,
            "operation_progress": [], "admission_generation": self.admission_generation,
        }


class _RetiringExecution(_PagedExecution):
    def __init__(self) -> None:
        super().__init__()
        self.retired = False

    def identity(self) -> dict[str, Any]:
        if not self.retired:
            return super().identity()
        return {
            "admission_generation": self.admission_generation,
            "api_version": "manyforge.execution_command_response.v6",
            "command": "identity",
            "error": {
                "code": "execution_owner_not_ready",
                "details": {"application_start_id": self.application_start_id},
                "message": "the managed execution owner has no active physical product",
            },
            "ok": False,
        }

    def events(self, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.retired:
            self.fail = False
        elif self.fail:
            self.retired = True
        page = super().events(after_sequence, limit=limit)
        page["execution_owner_ready"] = not self.retired
        if not after_sequence:
            self.fail = True
        return page


def test_event_pump_delivers_without_frontend_polling_and_stops_cleanly() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert "source_owner_epoch" not in core.ingested[0]
    assert core.ingested[0]["source_sequence"] == 1
    assert core.ingested[0]["payload_json"] == rfc8785.dumps(
        _event(1)["payload"]
    ).decode("utf-8")


def test_event_pump_wakes_cold_state_when_kernel_admission_advances() -> None:
    core = _RecordingCore()
    execution = _StreamingExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )

    def observe() -> None:
        wakes.append(None)
        changed.set()

    pump.set_change_observer(observe)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    baseline = len(wakes)

    execution.admission_generation += 1
    assert changed.wait(timeout=1.0)
    pump.shutdown()

    assert core.ingested == []
    assert len(wakes) == baseline + 1



def test_event_pump_wakes_cold_state_when_component_states_change() -> None:
    # The host publishes a component state change as a stream
    # `state` message without any journal row; it must reach `/ws/control`.
    class _ComponentExecution(_StreamingExecution):
        component_states: list[dict[str, Any]] = [
            {"component_id": "gripper", "lifecycle": "enabled"}
        ]

        def events(self, after_sequence: int, *, limit: int = 16):
            return {
                **super().events(after_sequence, limit=limit),
                "component_states": self.component_states,
            }

    core = _RecordingCore()
    execution = _ComponentExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )

    def observe() -> None:
        wakes.append(None)
        changed.set()

    pump.set_change_observer(observe)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    baseline = len(wakes)

    execution.component_states = [{"component_id": "gripper", "lifecycle": "prepared"}]
    assert changed.wait(timeout=1.0)
    pump.shutdown()

    assert core.ingested == []
    assert len(wakes) == baseline + 1


def test_event_pump_wakes_cold_state_on_a_signal_value_not_its_age() -> None:
    # I/O changes are pushed on `/ws/control`; the age that
    # changes on every read wakes nothing.
    class _SignalExecution(_StreamingExecution):
        samples: list[dict[str, Any]] = [{"signal_id": "io/di0", "value": False,
                                          "source": "device", "age_ns": 1, "fresh": True}]

        def events(self, after_sequence: int, *, limit: int = 16):
            return {
                **super().events(after_sequence, limit=limit),
                "signal_samples": [dict(sample) for sample in self.samples],
            }

    core = _RecordingCore()
    execution = _SignalExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )

    def observe() -> None:
        wakes.append(None)
        changed.set()

    pump.set_change_observer(observe)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    baseline = len(wakes)

    execution.samples = [{**execution.samples[0], "age_ns": 5}]
    assert not changed.wait(timeout=0.1)
    execution.samples = [{**execution.samples[0], "value": True}]
    assert changed.wait(timeout=1.0)
    changed.clear()
    execution.samples = [{**execution.samples[0], "fresh": False}]
    assert changed.wait(timeout=1.0)
    pump.shutdown()

    assert core.ingested == []
    assert len(wakes) == baseline + 2


def test_event_pump_wakes_cold_state_for_active_tcp_change() -> None:
    core = _RecordingCore()
    execution = _StreamingExecution()
    changed = threading.Event()
    pump = ExecutionEventPump(core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
    pump.set_change_observer(changed.set)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    payload = {
        "api_version": "manyforge.active_tcp_change.v3",
        "component_id": "robot-a",
        "previous": None,
        "entry": {"target": {"component_id": "robot-a", "command_target_id": "arm"},
                  "frame": {"component_id": "tool-a", "frame_id": "tcp"}},
        "source": "robot_control",
        "offset_sent": True,
        "registration_generation": 1,
        "sequence": 1,
    }
    event = _event(1)
    event.update(record_kind="active_tcp_changed", operation_id=None, operation_revision=None,
                 payload=payload, payload_digest=f"sha256:{hashlib.sha256(rfc8785.dumps(payload)).hexdigest()}")
    try:
        # The baseline in `state` wakes the panel; the row goes to History.
        execution.active_tcp_baseline = [{
            "component_id": "robot-a", "entry": payload["entry"],
            "registration_generation": 1, "sequence": 1,
        }]
        execution.append(event)
        assert changed.wait(timeout=1.0)
        for _ in range(100):
            if core.ingested:
                break
            threading.Event().wait(0.01)
        assert core.ingested[0]["record_kind"] == "active_tcp_changed"
        assert pump.last_error is None
    finally:
        pump.shutdown()

def test_event_pump_logs_only_error_transitions(caplog) -> None:
    pump = ExecutionEventPump(
        _RecordingCore(), _StreamingExecution(), application_start_id=_APPLICATION_START_ID)
    error = HeadlessApiError("execution_command_unavailable", "host lost")
    logger = logging.getLogger("manyforge_headless_api.execution")
    previous_level, previous_propagate = logger.level, logger.propagate
    logger.addHandler(caplog.handler)
    logger.setLevel(logging.INFO)
    logger.propagate = False  # Capture once regardless of parent logger setup.
    try:
        assert pump._record_error(error)  # noqa: SLF001
        assert not pump._record_error(error)  # noqa: SLF001
        assert pump._record_error(None)  # noqa: SLF001
        assert not pump._record_error(None)  # noqa: SLF001
    finally:
        logger.removeHandler(caplog.handler)
        logger.setLevel(previous_level)
        logger.propagate = previous_propagate
    assert len(caplog.records) == 2
    assert "execution_command_unavailable" in caplog.records[0].message
    assert caplog.records[1].message == "Execution event pump recovered"
    assert pump.last_error is None


def test_event_pump_progress_advances_without_journal_events_or_cold_rebuild() -> None:
    core = _RecordingCore()
    execution = _StreamingExecution()
    wakes: list[None] = []
    samples: list[list[dict[str, Any]]] = []
    execution.progress = [_progress_row(0.1)]
    execution.operations = {"change": 1, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    execution.append(
        _progress_event(1, state="executing", progress=0.1)
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.set_change_observer(lambda: wakes.append(None))
    pump.set_progress_observer(samples.append)
    pump.start()
    for _ in range(100):
        if len(core.ingested) >= 1 and wakes:
            break
        threading.Event().wait(0.01)
    first_wake_count = len(wakes)

    execution.progress = [_progress_row(0.8)]
    for _ in range(100):
        if samples and samples[-1] == execution.progress:
            break
        threading.Event().wait(0.01)
    threading.Event().wait(0.03)

    assert samples[-1] == execution.progress
    assert len(core.ingested) == 1
    assert len(wakes) == first_wake_count == 1

    # The completion changes the Kernel's operations (the wake); its row is
    # History only.
    execution.operations = {"change": 2, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    execution.append(
        _progress_event(2, state="completed", progress=1.0)
    )
    for _ in range(100):
        if len(core.ingested) >= 2 and len(wakes) > first_wake_count:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert len(core.ingested) == 2
    assert len(wakes) == first_wake_count + 1


def _progress_row(progress: float) -> dict[str, Any]:
    return {"operation_id": 1, "operation_revision": 1, "progress": progress}


@pytest.mark.parametrize("invalid", [None, [{"progress": 0.5}],
                                     [{**_progress_row(0.5), "owner_epoch": "owner.1"}],
                                     [_progress_row(float("nan"))],
                                     [_progress_row(0.5)] * 2,
                                     [{**_progress_row(0.5), "operation_id": "operation.1"}]])
def test_native_progress_requires_bounded_valid_unique_owner_rows(monkeypatch, tmp_path, invalid) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _page([])
    response["operation_progress"] = invalid
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    with pytest.raises(HeadlessApiError, match="progress"):
        client.events(0)


def test_native_progress_on_empty_journal_page(monkeypatch, tmp_path) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _page([])
    response["operation_progress"] = [_progress_row(0.6)]
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.events(0)["operation_progress"] == response["operation_progress"]


def test_progress_is_latest_only_bounded_and_quiet_when_unchanged() -> None:
    pump = ExecutionEventPump(
        _RecordingCore(), _StreamingExecution(), application_start_id=_APPLICATION_START_ID)
    samples = []
    pump.set_progress_observer(samples.append)
    pump._capture_progress([_progress_row(0.1)])
    pump._publish_progress(1.0)
    for sequence in range(2, 9):
        pump._capture_progress([_progress_row(sequence / 10)])
        pump._publish_progress(1.1)
    assert len(samples) == 1
    pump._publish_progress(1.21)
    assert samples[-1][0]["progress"] == 0.8
    pump._publish_progress(2.0)
    pump._capture_progress([_progress_row(0.8)])
    pump._publish_progress(2.01)
    assert len(samples) == 2
    pump._capture_progress([])
    pump._publish_progress(2.1)
    assert samples[-1] == []


def test_event_pump_reconciles_typed_idle_owner_through_stable_events() -> None:
    core = _RecordingCore()
    execution = _IdleExecution()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.set_change_observer(lambda: wakes.append(None))
    pump.start()
    assert core.completed.wait(timeout=1.0)
    for _ in range(100):
        if len(core.completed_calls) >= 4:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert pump.last_error is None
    # The idle owner is read once by the subscription, never polled; the
    # pump keeps completing idle snapshots from the stream.
    assert execution.status_calls == 1
    assert execution.events_calls >= 1
    assert len(core.completed_calls) >= 4
    assert wakes == [None]
    assert core.completed_calls[0] == {"through_source_sequence": 0}


def test_event_pump_wakes_cold_state_when_idle_kernel_admission_advances() -> None:
    core = _RecordingCore()
    execution = _IdleExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )

    def observe() -> None:
        wakes.append(None)
        changed.set()

    pump.set_change_observer(observe)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    baseline = len(wakes)

    execution.admission_generation += 1
    assert changed.wait(timeout=1.0)
    pump.shutdown()

    assert len(wakes) == baseline + 1
    assert execution.events_calls >= 1


class _TableExecution(_IdleExecution):
    """An idle owner whose stream ``state`` carries the v4 Kernel memory
    facts (OD-205 (b), OD-213): the operations table and the Stop facts."""

    def __init__(self) -> None:
        super().__init__()
        self.operations: dict[str, Any] = {"change": 1, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
        self.stop_progress_sequence = 0

    def events(self, _after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        page = super().events(_after_sequence, limit=limit)
        page["operations"] = dict(self.operations)
        page["last_stop_event_id"] = None
        page["stop_progress_sequence"] = self.stop_progress_sequence
        return page


def test_event_pump_wakes_the_panel_and_refreshes_rest_on_an_operations_change() -> None:
    core = _RecordingCore()
    execution = _TableExecution()
    changed = threading.Event()
    wakes: list[None] = []
    settled: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )

    def observe() -> None:
        wakes.append(None)
        changed.set()

    pump.set_change_observer(observe)
    pump.set_settled_observer(lambda: settled.append(None))
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    baseline, settled_baseline = len(wakes), len(settled)

    # A change of an operation in Kernel memory, no row: one wake, one refresh.
    execution.operations = {"change": 2, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    assert changed.wait(timeout=1.0)
    changed.clear()
    assert len(wakes) == baseline + 1
    assert len(settled) == settled_baseline + 1

    # A participant ended its Stop: one wake, one refresh.
    execution.stop_progress_sequence = 1
    assert changed.wait(timeout=1.0)
    changed.clear()
    assert len(wakes) == baseline + 2
    assert len(settled) == settled_baseline + 2

    # Unchanged facts: quiet.
    threading.Event().wait(0.1)
    pump.shutdown()
    assert len(wakes) == baseline + 2
    assert len(settled) == settled_baseline + 2
    assert pump.last_error is None
    # Each table's alarm members go to the alarm owner as received (OD-205
    # (b)); the resident list is the runner's (OD-213).
    assert core.operation_tables[0] == {"change": 1, "faulted": [], "faulted_total": 0}
    assert core.operation_tables[-1] == {"change": 2, "faulted": [], "faulted_total": 0}


def test_events_reply_passes_the_stream_memory_facts_and_rejects_a_bad_table(
    monkeypatch, tmp_path
) -> None:
    response = _page([])
    response["operations"] = {"change": 0, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    response["last_stop_event_id"] = 3
    response["stop_progress_sequence"] = 7
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(0)
    assert page["operations"] == {"change": 0, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    assert page["last_stop_event_id"] == 3
    assert page["stop_progress_sequence"] == 7
    # The `events` command reply carries no table.
    del response["operations"]
    assert client.events(0)["operations"] is None
    response["operations"] = {"change": -1, "faulted": [], "faulted_total": 0, "resident": [], "resident_total": 0}
    with pytest.raises(HeadlessApiError):
        client.events(0)
    # OD-213 (stream v4): the resident list and its count are required, and
    # the list never exceeds its count.
    for resident in (
        {"faulted_total": 0},
        {"faulted_total": 0, "resident": [], "resident_total": -1},
        {"faulted_total": 0, "resident": None, "resident_total": 0},
        {"faulted_total": 0, "resident": [{"operation_id": 1}], "resident_total": 0},
    ):
        response["operations"] = {"change": 0, "faulted": [], **resident}
        with pytest.raises(HeadlessApiError):
            client.events(0)


def test_event_pump_reconciles_idle_after_owner_retirement_at_event_high_water() -> (
    None
):
    core = _RecordingCore()
    execution = _RetiringExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.start()
    deadline = threading.Event()
    for _ in range(100):
        if len(core.completed_calls) >= 2:
            break
        deadline.wait(0.01)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert core.completed_calls[-1] == {"through_source_sequence": 1}


def test_event_pump_preserves_stop_acceptance_null_identity() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    execution.event = _stop_accepted_event(1)
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert len(core.ingested) == 1
    assert core.ingested[0]["record_kind"] == "stop_event_accepted"
    assert core.ingested[0]["operation_id"] is None
    assert core.ingested[0]["operation_revision"] is None


def test_event_pump_preserves_stop_completion_and_digest() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    execution.event = _stop_completed_event(1)
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert core.ingested[0]["record_kind"] == "stop_event_completed"
    assert core.ingested[0]["request_key"] is None
    assert core.ingested[0]["operation_id"] is None
    assert "request_id" not in core.ingested[0]
    assert core.ingested[0]["payload_json"] == rfc8785.dumps(
        execution.event["payload"]
    ).decode("utf-8")
    assert core.ingested[0]["payload_digest"] == execution.event["payload_digest"]


def test_event_pump_marks_a_lost_current_source_unknown() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    execution.fail = True
    assert core.unknown.wait(timeout=1.0)
    pump.shutdown()

    # A lost source is a lost subscription.
    assert pump.last_error is not None
    assert "execution_owner_unavailable" in pump.last_error


def test_event_pump_keeps_alarm_sources_current_on_every_iteration() -> None:
    """The pump is the alarm-source refresh cadence.

    It offers the refresh on every iteration, also while the execution owner
    is failing, and a failing refresh never degrades the operation feed.
    """
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, application_start_id=_APPLICATION_START_ID,
        poll_seconds=0.01, page_limit=1
    )
    core.alarm_refresh_failure = RuntimeError("source publication invalid")
    pump.start()
    assert core.completed.wait(timeout=1.0)
    assert not core.unknown.is_set()
    assert pump.last_error is None
    core.alarm_refresh_failure = None
    execution.fail = True
    assert core.unknown.wait(timeout=1.0)
    refreshes = core.alarm_refreshes
    for _ in range(100):
        if core.alarm_refreshes >= refreshes + 3:
            break
        threading.Event().wait(0.01)
    pump.shutdown()
    assert core.alarm_refreshes >= refreshes + 3


def _recovery_response(retry_eligibility: object = "absent") -> dict[str, Any]:
    assessment: dict[str, Any] = {
        "schema_version": "manyforge.kernel_recovery_assessment.v2",
        "assessment_revision": 2,
        "control_command_epoch": 0,
        "execution_id": 1,
        "allowed_actions": ["retry_motion_from_current_state"],
    }
    if retry_eligibility != "absent":
        assessment["retry_eligibility"] = retry_eligibility
    return {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "get_operation_recovery",
        "ok": True,
        "operation_revision": 3,
        "participants": [{"operation_id": 1}],
        "stop_event_id": None,
        "recovery_assessment": assessment,
    }


def _retry_eligibility(**changes: object) -> dict[str, object]:
    eligibility: dict[str, object] = {
        "eligible": True,
        "cause": "planning_no_effect",
        "effect": "known_no_effect",
        "reason": "none",
    }
    return {**eligibility, **changes}


@pytest.mark.parametrize("cause", ["planning_no_effect", "collision_stop_nominal", "not_eligible"])
@pytest.mark.parametrize("effect", ["known_no_effect", "effect_observed", "effect_uncertain"])
def test_recovery_assessment_accepts_closed_retry_eligibility(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, cause: str, effect: str
) -> None:
    client = ExecutionCommandClient(tmp_path)
    eligibility = _retry_eligibility(
        eligible=cause != "not_eligible", cause=cause, effect=effect, reason="none"
    )
    response = _recovery_response(eligibility)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.operation_recovery(1)["recovery_assessment"][
        "retry_eligibility"
    ] == eligibility


_RETRY_REASONS = tuple(sorted(RETRY_ELIGIBILITY_REASONS))


@pytest.mark.parametrize("reason", _RETRY_REASONS)
def test_recovery_assessment_accepts_every_frozen_retry_reason(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, reason: str
) -> None:
    client = ExecutionCommandClient(tmp_path)
    eligibility = _retry_eligibility(
        eligible=reason == "none", cause="not_eligible", reason=reason
    )
    response = _recovery_response(eligibility)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.operation_recovery(1)["recovery_assessment"][
        "retry_eligibility"
    ] == eligibility


def test_recovery_assessment_without_retry_eligibility_stays_valid(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _recovery_response()
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert "retry_eligibility" not in client.operation_recovery(1)[
        "recovery_assessment"
    ]


@pytest.mark.parametrize(
    "eligibility",
    [
        None,
        [],
        "eligible",
        _retry_eligibility(eligible=None),
        _retry_eligibility(eligible=1),
        _retry_eligibility(eligible="true"),
        _retry_eligibility(cause=None),
        _retry_eligibility(cause=["planning_no_effect"]),
        _retry_eligibility(cause="planning_retry"),
        _retry_eligibility(effect=None),
        _retry_eligibility(effect={"effect": "effect_observed"}),
        _retry_eligibility(effect="effect_unknown"),
        _retry_eligibility(reason=None),
        _retry_eligibility(reason=""),
        _retry_eligibility(reason="NONE"),
        _retry_eligibility(reason="blocked"),
        _retry_eligibility(reason="stop_not_settled "),
        _retry_eligibility(reason=3),
        _retry_eligibility(reason=["none"]),
        {k: v for k, v in _retry_eligibility().items() if k != "reason"},
        {k: v for k, v in _retry_eligibility().items() if k != "eligible"},
        {**_retry_eligibility(), "extra": None},
    ],
)
def test_recovery_assessment_rejects_malformed_retry_eligibility(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, eligibility: object
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _recovery_response(eligibility)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    with pytest.raises(HeadlessApiError) as malformed:
        client.operation_recovery(1)
    assert malformed.value.code == "execution_command_response_invalid"


def test_recovery_action_request_is_serialized_with_the_native_ten_fields(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    commands: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        commands.append(command)
        request = command["request"]
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": "apply_operation_recovery",
            "ok": True,
            "result": {
                "request_key": request["request_key"],
                "action": request["action"],
                "accepted": True,
                "resulting_assessment": {},
            },
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    client.apply_operation_recovery(
        1,
        3,
        OperationRecoveryActionRequest.model_validate(
            {
                "request_key": _RUNNER_KEY,
                "action": "retry_motion_from_current_state",
                "expected_assessment_revision": 2,
                "expected_control_command_epoch": 0,
                "expected_execution_id": 1,
            }
        ),
    )
    # The native decoder requires exactly these fields.
    assert set(commands[0]["request"]) == {
        "request_key",
        "action",
        "principal_id",
        "expected_assessment_revision",
        "expected_control_command_epoch",
        "expected_execution_id",
        "reason",
    }


def test_recovery_assessment_reason_union_is_the_frozen_seventeen() -> None:
    assert len(RETRY_ELIGIBILITY_REASONS) == 17
    assert "none" in RETRY_ELIGIBILITY_REASONS


def test_operation_events_pass_retry_fact_payloads_through_unchanged(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    # Native ingest owns the event-level wire shapes; Python only forwards them.
    event = copy.deepcopy(_event(1))
    event["payload"]["api_version"] = "manyforge.operation_journal_record.v4"
    event["payload"]["operation_state"]["motion_retry_candidate"] = "not_a_candidate"
    client = ExecutionCommandClient(tmp_path)
    response = _page([event])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(0)
    assert page["events"][0] == event

def test_stable_event_readiness_tracks_active_idle_active_same_kernel() -> None:
    class ModeExecution(_PagedExecution):
        def __init__(self) -> None:
            super().__init__()
            self.ready = True
            self.identities = 0

        def identity(self) -> dict[str, Any]:
            self.identities += 1
            if self.ready:
                return super().identity()
            return {
                "ok": False,
                "error": {"code": "execution_owner_not_ready", "message": "No product",
                          "details": {"application_start_id": self.application_start_id}},
            }

        def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
            page = super().events(after_sequence, limit=limit)
            page["execution_owner_ready"] = self.ready
            return page

    core = _RecordingCore()
    execution = ModeExecution()
    wakes: list[bool] = []
    changed = threading.Event()
    pump = ExecutionEventPump(core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
    def observe() -> None:
        wakes.append(execution.ready)
        changed.set()

    pump.set_change_observer(observe)
    pump.start()
    try:
        assert changed.wait(timeout=1.0)
        changed.clear()
        execution.ready = False
        assert changed.wait(timeout=1.0)
        changed.clear()
        execution.ready = True
        assert changed.wait(timeout=1.0)
    finally:
        pump.shutdown()
    assert pump.last_error is None
    # Readiness follows the stream's state; the owner is read once.
    assert execution.identities == 1
    assert wakes == [True, False, True]
    assert len(core.ingested) == 1  # The same journal cursor survives mode changes.
    assert all(set(call) == {"through_source_sequence"}
               for call in core.completed_calls)


@pytest.mark.parametrize("malformed_kind", [None, {}], ids=["typed_error", "type_error"])
def test_page_parse_errors_keep_runtime_fault_context_for_pump(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, malformed_kind: Any
) -> None:
    fault = {
        "write_kind": "state_observed",
        "error": {"code": "io_error", "message": "Journal failed", "details": {}},
        "operation_id": 1, "operation_revision": 1, "request_key": _STOP_KEY,
        "program_run_id": 1,
        "execution_scope": {"node_instance_id": "move", "node_attempt_id": 1},
    }
    response = _page([_event(1)])
    response["runtime_faults"]["operation_journal_write_fault"] = fault
    response["event_page"]["events"][0]["record_kind"] = malformed_kind
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _request: response)
    expected_error = HeadlessApiError if malformed_kind is None else TypeError
    with pytest.raises(expected_error) as failed:
        client.events(0, limit=1)
    snapshot = failed.value.runtime_fault_snapshot
    assert set(snapshot) == {"runtime_faults", "execution_owner_ready"}
    assert snapshot["runtime_faults"]["operation_journal_write_fault"] == fault

    class FailedExecution(_PagedExecution):
        """Streams the same malformed row with the same runtime fault: the
        pump's page parse fails with the fault context."""

        def __init__(self) -> None:
            super().__init__()
            self.event = response["event_page"]["events"][0]

        def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
            page = super().events(after_sequence, limit=limit)
            page["runtime_faults"] = response["runtime_faults"]
            return page

    core = _RecordingCore()
    pump = ExecutionEventPump(core, FailedExecution(), application_start_id=_APPLICATION_START_ID, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
    pump.start()
    assert core.unknown.wait(timeout=1.0)
    pump.shutdown()
    assert core.runtime_fault_snapshots
    assert json.loads(core.runtime_fault_snapshots[0]["current_runtime_faults_json"])[
        "operation_journal_write_fault"
    ] == fault
    assert core.completed_calls == []
    assert pump.last_error is not None  # Fault delivery does not turn page failure into Good.


def _rule_state_event(sequence: int, state: str) -> dict[str, Any]:
    payload = {
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "rule_id": "door",
        "state": state,
        "triggering_acquisition": None,
    }
    return {
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:01.00000000{sequence}Z",
        "operation_id": None,
        "operation_revision": None,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(rfc8785.dumps(payload)).hexdigest()}",
        "record_kind": "interrupt_rule_state_changed",
        "request_key": None,
        "sequence": sequence,
    }


def _rule_catalog() -> dict[str, Any]:
    return {
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "bundle": "bundle.1",
        "graph": "graph.1",
        "rule_ids": ["door"],
        "referenced_program_blackboard_keys": [],
    }


def _rule_baseline(state: str = "clear") -> dict[str, Any]:
    return {"status": "ready", "snapshot": {
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "states": [{"rule_id": "door", "state": state, "triggering_acquisition": None}],
    }}


def test_execution_client_passes_global_rule_wire_and_null_identity_state_rows(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    row = _rule_state_event(1, "tripped")
    response = _page([row])
    response["interrupt_rule_catalog"] = _rule_catalog()
    response["interrupt_rule_state_baseline"] = {"status": "projection_pending"}
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(0)
    assert page["events"][0]["record_kind"] == "interrupt_rule_state_changed"
    assert page["interrupt_rule_catalog"] == response["interrupt_rule_catalog"]
    assert page["interrupt_rule_state_baseline"] == {"status": "projection_pending"}

    row["operation_id"] = 1
    with pytest.raises(HeadlessApiError):
        client.events(0)


class _RuleStreamingExecution(_StreamingExecution):
    def __init__(self) -> None:
        super().__init__()
        # The Kernel's in-memory rule state, independent of the rows.
        self.baseline_state = "clear"

    def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        page = super().events(after_sequence, limit=limit)
        page["interrupt_rule_catalog"] = _rule_catalog()
        page["interrupt_rule_state_baseline"] = _rule_baseline(self.baseline_state)
        return page


def test_event_pump_projects_global_rules_and_forwards_rule_rows_to_the_historian() -> None:
    core = _RecordingCore()
    execution = _RuleStreamingExecution()
    execution.baseline_state = "tripped"
    execution.append(_event(1))
    execution.append(_rule_state_event(2, "tripped"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01)  # type: ignore[arg-type]
    pump.set_global_rules_observer(published.append)
    pump.start()
    for _ in range(200):
        if core.completed_calls and published:
            break
        threading.Event().wait(0.01)
    execution.baseline_state = "inactive"
    execution.append(_rule_state_event(3, "inactive"))
    for _ in range(200):
        if published and published[-1] == [{"rule_id": "door", "state": "inactive", "keys": []}]:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    # Rule rows reach the historian unchanged; snapshots cover them.
    assert [row["source_sequence"] for row in core.ingested] == [1, 2, 3]
    tripped = _rule_state_event(2, "tripped")
    assert core.ingested[1] == {
        "source_sequence": 2,
        "record_kind": "interrupt_rule_state_changed",
        "request_key": None,
        "operation_id": None,
        "operation_revision": None,
        "occurred_at_ns": tripped["occurred_at_ns"],
        "occurred_at_wall": tripped["occurred_at_wall"],
        "payload_json": rfc8785.dumps(tripped["payload"]).decode("utf-8"),
        "payload_digest": tripped["payload_digest"],
    }
    assert set(json.loads(core.ingested[1]["payload_json"])) == {
        "catalog_generation", "active_publication_generation",
        "retained_bundle_id", "graph_revision", "rule_id", "state",
        "triggering_acquisition",
    }
    assert core.completed_calls[0]["through_source_sequence"] == 2
    assert core.completed_calls[-1]["through_source_sequence"] == 3
    assert published[0] == [{"rule_id": "door", "state": "tripped", "keys": []}]
    assert published[-1] == [{"rule_id": "door", "state": "inactive", "keys": []}]
    assert len(published) == 2


def test_event_pump_keeps_the_historian_feed_when_the_rule_wire_is_malformed(caplog) -> None:
    core = _RecordingCore()
    execution = _RuleStreamingExecution()
    execution.baseline_state = "exploded"
    execution.append(_event(1))
    execution.append(_rule_state_event(2, "tripped"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01)  # type: ignore[arg-type]
    pump.set_global_rules_observer(published.append)
    # Capture on the module logger itself: suite-wide logging configuration
    # (propagation or levels set by other tests) must not hide the warning.
    module_logger = logging.getLogger("manyforge_headless_api.execution")
    previous_level = module_logger.level
    module_logger.addHandler(caplog.handler)
    module_logger.setLevel(logging.WARNING)
    try:
        pump.start()
        for _ in range(200):
            if core.completed_calls and "global rule status is unreadable" in caplog.text:
                break
            threading.Event().wait(0.01)
        pump.shutdown()
    finally:
        module_logger.removeHandler(caplog.handler)
        module_logger.setLevel(previous_level)

    # The historian still receives the rows; only the GUI fold is unreadable
    # (unknown, never clear: nothing is published).
    assert [row["source_sequence"] for row in core.ingested] == [1, 2]
    assert published == []
    assert pump.last_error is None
    assert "global rule status is unreadable" in caplog.text


class _RefusingRuleRowCore(_RecordingCore):
    def __init__(self) -> None:
        super().__init__()
        self.refusals = 0

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        if kwargs["record_kind"] == "interrupt_rule_state_changed":
            self.refusals += 1
            raise HeadlessApiError("operation_event_invalid", "historian refused the row")
        return super().ingest_operation_event(**kwargs)


def test_event_pump_surfaces_a_refused_rule_row_like_any_refused_row() -> None:
    core = _RefusingRuleRowCore()
    execution = _RuleStreamingExecution()
    execution.append(_event(1))
    execution.append(_rule_state_event(2, "tripped"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, application_start_id=_APPLICATION_START_ID, poll_seconds=0.01)  # type: ignore[arg-type]
    pump.set_global_rules_observer(published.append)
    pump.start()
    for _ in range(200):
        if core.refusals >= 2 and core.unknown.is_set():
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    # The shared refusal path: source quality unknown, error recorded, no
    # snapshot past the refused row, and the row is retried, not skipped.
    assert core.unknown.is_set()
    assert pump.last_error is not None
    assert "historian refused the row" in str(pump.last_error)
    assert core.refusals >= 2
    assert [row["source_sequence"] for row in core.ingested][:1] == [1]
    assert not core.completed_calls
    assert published and published[-1] is None


def test_active_tcp_change_and_baseline_survive_events_reply(monkeypatch, tmp_path):
    row = _event(1)
    row.update(record_kind="active_tcp_changed", operation_id=None, operation_revision=None,
               request_key=None, payload={"api_version": "manyforge.active_tcp_change.v3",
                   "component_id": "robot", "previous": None,
                   "entry": None, "source": "reset", "offset_sent": False,
                   "registration_generation": 1, "sequence": 1})
    row["payload_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(row["payload"])).hexdigest()
    response = _page([row])
    baseline = [{"component_id": "robot", "entry": None,
                 "registration_generation": 1, "sequence": 1}]
    response["active_tcp_baseline"] = baseline
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(0)
    assert page["active_tcp_baseline"] == baseline
    assert page["events"][0]["payload"] == row["payload"]
    row["operation_id"] = 1
    with pytest.raises(HeadlessApiError):
        client.events(0)


@pytest.mark.parametrize("payload", [
    {"kind": "set_tcp", "tcp": {"kind": "entry",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "frame": {"component_id": "tool", "frame_id": "tcp"}}},
    {"kind": "set_tcp", "tcp": {"kind": "default_of_target",
        "target": {"component_id": "tool", "command_target_id": "gripper"}}},
])
def test_public_execution_submit_forwards_set_tcp_and_attribution(monkeypatch, tmp_path, payload):
    from manyforge_headless_api.tests.test_models import _planning_operation
    operation = _planning_operation("manual.set-tcp", payload)
    request = OperationIntentRequest.model_validate(operation)
    client = ExecutionCommandClient(tmp_path)
    commands = []
    rows = [{"component_id": "robot", "entry": None,
             "registration_generation": 1, "sequence": 0}]
    def exchange(command):
        commands.append(command)
        return {"ok": True, "command": "submit", "accepted": {}, "active_tcp": rows}
    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.submit(request, trusted_principal_id="operator.1")["active_tcp"] == rows
    assert commands[0]["operation"]["participants"][0]["payload"] == payload
    assert commands[0]["attribution"] == {"transport": "http", "assurance": "externally_authenticated", "principal_id": "operator.1"}


def _bounded_motion_path() -> dict[str, Any]:
    return {
        "source_joint_names": ["joint1"],
        "pairs": [{"body_a": "component/arm/model/arm/body/link/collision/0",
                   "body_b": "scene/wall/shape/0"}],
        "samples": [
            {"time_from_start_s": 0.0, "positions_rad": [0.0], "contacts": [], "status": "checked"},
            {"time_from_start_s": 1.0, "positions_rad": [0.2],
             "contacts": [{"pair_index": 0, "disposition": "refused"}], "status": "checked"},
        ],
    }


def _v2_preview_response(operation: dict[str, Any]) -> dict[str, Any]:
    response = _preview_response(operation)
    receipt = response["preview_receipt"]
    receipt["schema_version"] = "manyforge.direct_control_motion_preview.v5"
    receipt["path_preview"] = _bounded_motion_path()
    receipt.pop("receipt_digest")
    receipt["receipt_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    return response


def test_motion_preview_v4_validates_shape_and_digest(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    for factory in (_preview_response, _v2_preview_response):
        monkeypatch.setattr(client, "_exchange", lambda command: factory(command["operation"]))
        assert client.preview_motion(request, trusted_principal_id=None)["ok"] is True
    def tampered(command):
        response = _v2_preview_response(command["operation"])
        response["preview_receipt"]["path_preview"]["samples"][1]["positions_rad"] = [0.3]
        return response
    monkeypatch.setattr(client, "_exchange", tampered)
    with pytest.raises(HeadlessApiError, match="digest"):
        client.preview_motion(request, trusted_principal_id=None)


def test_motion_preview_v4_tcp_snapshot_is_closed_and_digest_bound(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    def response_with(source):
        def exchange(command):
            response = _preview_response(command["operation"])
            receipt = response["preview_receipt"]
            receipt["resolved_tcp"]["source"] = source
            receipt.pop("receipt_digest")
            receipt["receipt_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
            return response
        return exchange
    for source in ("active_tcp", "joint_flange_fallback"):
        monkeypatch.setattr(client, "_exchange", response_with(source))
        assert client.preview_motion(request, trusted_principal_id=None)["preview_receipt"]["resolved_tcp"]["source"] == source

    payload = request.model_dump(mode="json")
    payload["participants"][0]["payload"]["tcp_endpoint"] = {
        "kind": "entry",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "frame": {"component_id": "robot", "frame_id": "flange"},
    }
    monkeypatch.setattr(client, "_exchange", response_with("explicit_entry"))
    explicit = client.preview_motion(OperationIntentRequest.model_validate(payload), trusted_principal_id=None)
    assert explicit["preview_receipt"]["resolved_tcp"]["source"] == "explicit_entry"

    payload["participants"][0]["payload"]["tcp_endpoint"]["frame"]["frame_id"] = "other"
    with pytest.raises(HeadlessApiError) as mismatch:
        client.preview_motion(OperationIntentRequest.model_validate(payload), trusted_principal_id=None)
    assert mismatch.value.code == "execution_command_response_invalid"

    def tampered(command):
        response = _preview_response(command["operation"])
        response["preview_receipt"]["resolved_tcp"]["entry"]["frame"]["frame_id"] = "different_tcp"
        return response
    monkeypatch.setattr(client, "_exchange", tampered)
    with pytest.raises(HeadlessApiError, match="digest"):
        client.preview_motion(request, trusted_principal_id=None)


@pytest.mark.parametrize("mutation", [
    lambda tcp: tcp.update(extra=True),
    lambda tcp: tcp.pop("sequence"),
    lambda tcp: tcp.update(source="other"),
    lambda tcp: tcp.update(owner_epoch="other-owner"),
    lambda tcp: tcp.update(registration_generation=0),
    lambda tcp: tcp.update(registration_generation=True),
    lambda tcp: tcp.update(sequence=-1),
    lambda tcp: tcp.update(sequence=2**53),
    lambda tcp: tcp["entry"]["frame"].update(extra=True),
    lambda tcp: tcp["entry"]["target"].update(component_id=""),
])
def test_motion_preview_v4_rejects_invalid_tcp_snapshot(monkeypatch, tmp_path, mutation):
    client = ExecutionCommandClient(tmp_path)
    def exchange(command):
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        mutation(receipt["resolved_tcp"])
        receipt.pop("receipt_digest")
        try:
            receipt["receipt_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
        except rfc8785.CanonicalizationError:
            # Out-of-domain integers cannot have a canonical digest.
            receipt["receipt_digest"] = "sha256:" + "0" * 64
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(_direct_motion_request(), trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_v4_rejects_fallback_for_pose(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    payload = _direct_motion_request().model_dump(mode="json")
    payload["participants"][0]["payload"]["motion"] = {
        "kind": "pose_goal", "goal": {
            "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.4},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        },
    }
    request = OperationIntentRequest.model_validate(payload)
    def exchange(command):
        response = _preview_response(command["operation"])
        response["preview_receipt"]["resolved_tcp"]["source"] = "joint_flange_fallback"
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_rejects_old_receipt_without_tcp_snapshot(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    def exchange(command):
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["schema_version"] = "manyforge.direct_control_motion_preview.v3"
        del receipt["resolved_tcp"]
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(_direct_motion_request(), trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


@pytest.mark.parametrize("mutation", [
    lambda p: p.update(extra=True),
    lambda p: p["samples"][0].update(time_from_start_s=float("nan")),
    lambda p: p["samples"][1].update(time_from_start_s=0.0),
    lambda p: p["samples"][0].update(positions_rad=[float("inf")]),
    lambda p: p["samples"][0].update(positions_rad=[10**400]),
    lambda p: p["samples"][1]["contacts"][0].update(pair_index=1),
    lambda p: p["samples"][1]["contacts"][0].update(disposition="clear"),
    lambda p: p["samples"][1]["contacts"].append(dict(p["samples"][1]["contacts"][0])),
    lambda p: p["pairs"].append(dict(p["pairs"][0])),
    lambda p: p["pairs"][0].update(body_a=""),
    lambda p: p["pairs"][0].update(body_a="\ud800"),
    lambda p: p.update(source_joint_names=["wrong"]),
    lambda p: p["samples"].extend([dict(p["samples"][-1])] * 31),
    lambda p: p["pairs"].extend([dict(p["pairs"][0])] * 64),
    lambda p: p.update(source_joint_names=[f"j{i}" for i in range(257)]),
])
def test_motion_preview_v2_rejects_malformed_path(monkeypatch, tmp_path, mutation):
    client = ExecutionCommandClient(tmp_path)
    def exchange(command):
        response = _v2_preview_response(command["operation"])
        mutation(response["preview_receipt"]["path_preview"])
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(_direct_motion_request(), trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def _bounded_diagnostic():
    path = _bounded_motion_path()
    path.update(schema_version="manyforge.motion_preview_diagnostic.v2", kind="validated_path", reason="validation_collision")
    for sample in path["samples"]:
        sample["clearance_violations"] = []
    return path


@pytest.mark.parametrize("mutation", [
    lambda p: p.update(schema_version="v3"),
    lambda p: p.update(kind="other"),
    lambda p: p.update(reason=""),
    lambda p: p["samples"][0].pop("clearance_violations"),
    lambda p: p["samples"][0]["clearance_violations"].append({"pair_index": 1, "distance_m": .001, "required_clearance_m": .002}),
    lambda p: p["samples"][0]["clearance_violations"].append({"pair_index": 0, "distance_m": .002, "required_clearance_m": .001}),
    lambda p: p.update(kind="collision_disabled_ik_pose"),
])
def test_diagnostic_v2_strictly_rejects_malformed_evidence(mutation):
    from manyforge_headless_api.execution import _motion_path_preview_valid
    diagnostic = _bounded_diagnostic()
    assert _motion_path_preview_valid(diagnostic, diagnostic=True)
    mutation(diagnostic)
    assert not _motion_path_preview_valid(diagnostic, diagnostic=True)


def test_ik_diagnostic_path_requires_multiple_samples():
    from manyforge_headless_api.execution import _motion_path_preview_valid
    diagnostic = _bounded_diagnostic()
    diagnostic["kind"] = "collision_disabled_ik_path"
    assert len(diagnostic["samples"]) >= 2
    assert _motion_path_preview_valid(diagnostic, diagnostic=True)
    diagnostic["samples"] = diagnostic["samples"][:1]
    assert not _motion_path_preview_valid(diagnostic, diagnostic=True)


def test_failed_motion_preview_diagnostic_is_optional_and_never_a_receipt(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    error = {"code": "validation_collision", "message": "collision", "details": {}}
    response = {"api_version": "manyforge.execution_command_response.v6", "ok": False,
                "error": error, "diagnostic_preview": _bounded_diagnostic()}
    monkeypatch.setattr(client, "_exchange", lambda _command: copy.deepcopy(response))
    assert client.preview_motion(request, trusted_principal_id=None) == response
    for invalid in (None, {**_bounded_diagnostic(), "receipt_digest": "forged"},
                    {**_bounded_diagnostic(), "token": "forged"},
                    {**_bounded_diagnostic(), "padding": "x" * (48 * 1024)}):
        response["diagnostic_preview"] = invalid
        result = client.preview_motion(request, trusted_principal_id=None)
        assert result["ok"] is False and result["error"] == error
        assert "diagnostic_preview" not in result and "preview_receipt" not in result
    response["diagnostic_preview"] = _bounded_diagnostic()
    response["preview_receipt"] = {"commit_eligible": True}
    result = client.preview_motion(request, trusted_principal_id=None)
    assert "diagnostic_preview" not in result and "preview_receipt" not in result and result["error"] == error


def test_cartesian_diagnostic_failure_details_survive_execution_client(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    failure = {
        "api_version": "manyforge.execution_command_response.v6",
        "ok": False,
        "error": {
            "code": "cartesian_incomplete",
            "message": "Cartesian solver failed",
            "details": {
                "reason": "cartesian_ik_failure",
                "cartesian_collision_disabled_diagnostic": "failed",
                "original_error_code": "cartesian_incomplete",
            },
        },
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: copy.deepcopy(failure))
    assert client.preview_motion(_direct_motion_request(), trusted_principal_id=None) == failure


def test_motion_preview_path_limits_and_oversized_failure_preserves_error(monkeypatch, tmp_path):
    from manyforge_headless_api.execution import _motion_path_preview_valid

    names = [f"joint{i}" for i in range(256)]
    path = {"source_joint_names": names,
            "pairs": [{"body_a": f"component/arm/model/arm/body/link/collision/{i}",
                       "body_b": "scene/wall/shape/0"} for i in range(64)],
            "samples": [{"time_from_start_s": float(i), "positions_rad": [0.0] * 256,
                         "contacts": [], "status": "unknown"} for i in range(32)]}
    path["pairs"].sort(key=lambda pair: (pair["body_a"], pair["body_b"]))
    assert _motion_path_preview_valid(path)
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    diagnostic = _bounded_diagnostic()
    diagnostic["pairs"][0]["body_b"] = "scene/" + "a" * (48 * 1024) + "/shape/0"
    failure = {"api_version": "manyforge.execution_command_response.v6", "ok": False,
               "error": {"code": "validation_collision", "message": "collision", "details": {}},
               "diagnostic_preview": diagnostic}
    monkeypatch.setattr(client, "_exchange", lambda _command: copy.deepcopy(failure))
    result = client.preview_motion(request, trusted_principal_id=None)
    assert result == {key: value for key, value in failure.items() if key != "diagnostic_preview"}


def test_motion_preview_rejects_unsupported_schema_and_oversized_v2(monkeypatch, tmp_path):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    def exchange(command):
        response = _v2_preview_response(command["operation"])
        response["preview_receipt"]["path_preview"]["pairs"][0]["body_b"] = "scene/" + "a" * (48 * 1024) + "/shape/0"
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"
    def unknown(command):
        response = _preview_response(command["operation"])
        response["preview_receipt"]["schema_version"] = "manyforge.direct_control_motion_preview.v2"
        return response
    monkeypatch.setattr(client, "_exchange", unknown)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_accepts_unmapped_raw_body_identity():
    from manyforge_headless_api.execution import _motion_path_preview_valid
    path = _bounded_motion_path()
    path["pairs"][0]["body_a"] = "custom raw body identity"
    path["samples"][1]["contacts"][0]["disposition"] = "unknown"
    assert _motion_path_preview_valid(path)


def test_recovery_assessment_names_its_stop_by_event_id(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _recovery_response()
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    response["stop_event_id"] = 9
    assert client.operation_recovery(1)["stop_event_id"] == 9
    for invalid in (0, "9", True):
        response["stop_event_id"] = invalid
        with pytest.raises(HeadlessApiError):
            client.operation_recovery(1)
    response["stop_event_id"] = None
    response["recovery_assessment"]["schema_version"] = (
        "manyforge.kernel_recovery_assessment.v1"
    )
    with pytest.raises(HeadlessApiError):
        client.operation_recovery(1)


def test_pump_keeps_the_journal_queue_level_for_the_disk_check() -> None:
    """OD-215: the pump keeps the Kernel journal's (queue full, storage
    failure) levels from each runtime-fault snapshot, for the disk check."""

    core = _RecordingCore()
    pump = ExecutionEventPump(core, _PagedExecution(), application_start_id=_APPLICATION_START_ID)  # type: ignore[arg-type]
    assert pump.journal_disk_signals == (False, False)
    fault = {
        "write_kind": "state_observed",
        "error": {"code": "operation_journal_records_lost", "message": "dropped", "details": {}},
        "operation_id": None, "operation_revision": None, "request_key": None,
        "program_run_id": None, "execution_scope": None, "queue_full": True,
    }
    faults = {"operation_journal_write_fault": fault, "program_runner_loss": None,
              "kernel_internal_fault": None}
    pump._observe_runtime_fault_snapshot({"runtime_faults": faults})
    assert pump.journal_disk_signals == (True, False)
    fault["queue_full"] = False
    fault["error"]["code"] = "operation_journal_storage_failure"
    pump._observe_runtime_fault_snapshot({"runtime_faults": faults})
    assert pump.journal_disk_signals == (False, True)
    faults["operation_journal_write_fault"] = None
    pump._observe_runtime_fault_snapshot({"runtime_faults": faults})
    assert pump.journal_disk_signals == (False, False)
