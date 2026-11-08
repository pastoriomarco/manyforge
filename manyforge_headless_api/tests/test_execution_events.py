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
    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.component_recovery_preflight("arm")["preflight"] == preflight
    confirmation = {"snapshot_id": "native-snapshot", "confirmed": [
        {"component_id": "machine", "cause_code": "estop", "vendor_code": "C2", "stop_code": "estop", "stop_vendor_code": "C2", "eligibility": "recover"},
        {"component_id": "arm", "cause_code": None, "vendor_code": None, "stop_code": None, "stop_vendor_code": None, "eligibility": "ready"},
    ]}
    assert RecoveryPreflightConfirmation.model_validate(confirmation).model_dump(mode="json") == confirmation
    client.component_recover_enable("arm", confirmation)
    assert commands == [
        {"api_version": "manyforge.execution_command.v3", "command": "component_recovery_preflight",
         "request": {"component_id": "arm"}},
        {"api_version": "manyforge.execution_command.v3", "command": "component_action", "request": {
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
    monkeypatch.setattr(client, "_exchange_control", lambda command: {
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
    monkeypatch.setattr(client, "_exchange_control", lambda _command: {
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
    monkeypatch.setattr(client, "_exchange_control", lambda _command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native", "components": [row]},
    })
    with pytest.raises(HeadlessApiError):
        client.component_recovery_preflight("arm")


def _event(owner_epoch: str, sequence: int) -> dict[str, Any]:
    payload = {
        "api_version": "manyforge.operation_journal_record.v1",
        "effect_pending": False,
        "intent": {},
        "operation_state": {
            "operation_id": "operation.1",
            "operation_revision": 1,
            "participants": [],
            "program_binding_id": "binding.1",
            "program_run_id": "run.1",
            "state": "completed",
        },
        "owner_epoch": owner_epoch,
        "record_kind": "state_observed",
        "result": None,
        "runtime_scale_status": None,
    }


    canonical = rfc8785.dumps(payload)
    return {
        "effect_pending": False,
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:00.00000000{sequence}Z",
        "operation_id": "operation.1",
        "operation_revision": 1,
        "owner_epoch": owner_epoch,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": "state_observed",
        "request_id": None,
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
    owner_epoch: str,
    sequence: int,
    *,
    state: str,
    progress: float,
    operation_id: str = "operation.1",
) -> dict[str, Any]:
    event = copy.deepcopy(_event(owner_epoch, sequence))
    event["operation_id"] = operation_id
    operation = event["payload"]["operation_state"]
    operation["operation_id"] = operation_id
    operation["state"] = state
    operation["updated_at_ns"] = 100 + sequence
    operation["participants"] = [
        {
            "participant_id": "operation.1.participant.1",
            "progress": progress,
            "state": state,
            "terminal_error": None,
        }
    ]
    canonical = rfc8785.dumps(event["payload"])
    event["payload_digest"] = f"sha256:{hashlib.sha256(canonical).hexdigest()}"
    return event


def _stop_projection_event(owner_epoch: str, sequence: int) -> dict[str, Any]:
    payload = {
        "acceptance": {
            "entry_state": "active",
            "owner_epoch": owner_epoch,
            "request_id": "stop-request.1",
            "stop_generation": 1,
            "stop_id": "stop.1",
        },
        "accepted": {
            "request_id": "stop-request.1",
            "stop_generation": 1,
            "stop_id": "stop.1",
        },
        "affected_state": {"operations": [], "resources": []},
        "api_version": "manyforge.stop_acceptance_projection.v2",
        "intent": {"intent_kind": "full_stop", "value": {}},
        "record_kind": "stop_acceptance_projected",
    }
    canonical = rfc8785.dumps(payload)
    return {
        "effect_pending": True,
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:00.00000000{sequence}Z",
        "operation_id": None,
        "operation_revision": None,
        "owner_epoch": owner_epoch,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": "stop_acceptance_projected",
        "request_id": "stop-request.1",
        "sequence": sequence,
    }


def _stop_terminal_event(owner_epoch: str, sequence: int) -> dict[str, Any]:
    payload = {
        "acceptance": {
            "accepted_at_ns": 100,
            "request_id": "stop-request.1",
            "stop_generation": 1,
            "stop_id": "stop.1",
        },
        "acceptance_to_terminal_latency_ns": 1_000_000_000,
        "api_version": "manyforge.stop_terminal_projection.v1",
        "attempted_stage_count": 1,
        "attempted_stages": [],
        "certificate_evidence": [],
        "certificate_evidence_count": 1,
        "completed_at_ns": 1_000_000_100,
        "details_truncated": True,
        "durable_projection_digest": "sha256:" + "8" * 64,
        "escalated": False,
        "fault_latch": None,
        "hard_stop_floor_used": False,
        "owner_epoch": owner_epoch,
        "participant_settlement": [],
        "participant_settlement_count": 1,
        "record_kind": "stop_terminal_projected",
        "request_id": "stop-request.1",
        "result": "succeeded_nominal",
        "state": "succeeded",
        "stop_certificate_id": "stop-certificate.1",
        "stop_certificate_revision": 1,
        "stop_generation": 1,
        "stop_id": "stop.1",
    }
    canonical = rfc8785.dumps(payload)
    return {
        "effect_pending": False,
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:01.00000000{sequence}Z",
        "operation_id": None,
        "operation_revision": None,
        "owner_epoch": owner_epoch,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": "stop_terminal_projected",
        "request_id": None,
        "sequence": sequence,
    }


def _page(owner_epoch: str, events: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "operation_progress": [],
        "admission_generation": 1,
        "api_version": "manyforge.execution_command_response.v4",
        "command": "events",
        "owner_epoch": owner_epoch,
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
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v2",
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
            "api_version": "manyforge.execution_command_response.v4",
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
        {"api_version": "manyforge.execution_command.v3", "command": command}
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
            "api_version": "manyforge.execution_command_response.v4",
            "command": "status",
            "ok": True,
        },
    )
    with pytest.raises(HeadlessApiError) as mismatched:
        client.identity()
    assert mismatched.value.code == "execution_command_response_invalid"


def test_execution_client_validates_event_cursor_and_owner(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.1"
    response = _page(owner_epoch, [_event(owner_epoch, 1)])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
    assert page["next_after_sequence"] == 1
    assert page["admission_generation"] == 1

    response["admission_generation"] = 0
    with pytest.raises(HeadlessApiError) as invalid_admission:
        client.events(owner_epoch, 0)
    assert invalid_admission.value.code == "execution_command_response_invalid"
    response["admission_generation"] = 1

    response["event_page"]["next_after_sequence"] = 2
    with pytest.raises(HeadlessApiError) as invalid:
        client.events(owner_epoch, 0)
    assert invalid.value.code == "execution_command_response_invalid"

    response = _page(owner_epoch, [])
    response["event_page"]["has_more"] = True
    with pytest.raises(HeadlessApiError) as stalled:
        client.events(owner_epoch, 0)
    assert stalled.value.code == "execution_command_response_invalid"


def test_execution_client_accepts_only_paired_null_stop_projection_identity(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.stop"
    stop_event = _stop_projection_event(owner_epoch, 1)
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page(owner_epoch, [stop_event])
    )
    page = client.events(owner_epoch, 0)
    assert page["events"][0]["operation_id"] is None
    assert page["events"][0]["operation_revision"] is None

    stop_event["operation_revision"] = 1
    with pytest.raises(HeadlessApiError) as mixed:
        client.events(owner_epoch, 0)
    assert mixed.value.code == "execution_command_response_invalid"

    stop_event["operation_revision"] = None
    stop_event["request_id"] = None
    with pytest.raises(HeadlessApiError) as uncorrelated:
        client.events(owner_epoch, 0)
    assert uncorrelated.value.code == "execution_command_response_invalid"

    ordinary = _event(owner_epoch, 1)
    ordinary["operation_id"] = None
    ordinary["operation_revision"] = None
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page(owner_epoch, [ordinary])
    )
    with pytest.raises(HeadlessApiError) as unattributed:
        client.events(owner_epoch, 0)
    assert unattributed.value.code == "execution_command_response_invalid"


def test_execution_client_accepts_stop_terminal_as_stop_scoped_fact(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.stop"
    terminal = _stop_terminal_event(owner_epoch, 1)
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page(owner_epoch, [terminal])
    )
    page = client.events(owner_epoch, 0)
    assert page["events"][0]["record_kind"] == "stop_terminal_projected"
    assert page["events"][0]["request_id"] is None
    assert page["events"][0]["operation_id"] is None

    terminal["request_id"] = "stop-request.1"
    with pytest.raises(HeadlessApiError) as invalid_request_identity:
        client.events(owner_epoch, 0)
    assert invalid_request_identity.value.code == ("execution_command_response_invalid")


def test_execution_client_requires_backend_residency_projection(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = {
        "api_version": "manyforge.execution_command_response.v4",
        "command": "read",
        "ok": True,
        "operation": {"operation_id": "operation.1"},
        "resident": True,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.read("operation.1")["resident"] is True

    response.pop("resident")
    with pytest.raises(HeadlessApiError) as missing:
        client.read("operation.1")
    assert missing.value.code == "execution_command_response_invalid"

    response["resident"] = 1
    with pytest.raises(HeadlessApiError) as non_boolean:
        client.read("operation.1")
    assert non_boolean.value.code == "execution_command_response_invalid"


def test_execution_client_uses_internal_attribution_for_program_run_stop(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    request = ProgramRunStopIntentRequest.model_validate(
        {
            "request_context": {
                "request_id": "program-stop.1",
                "request_owner_epoch": "execution.epoch.1",
                "source_label": "manyforge_behavior",
                "correlation_id": "program-run.1",
            },
            "program_run_id": "program-run.1",
            "reason": "operator motion Stop",
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
            "api_version": "manyforge.execution_command_response.v4",
            "command": "stop_program_run",
            "ok": True,
            "accepted": {
                "request_id": request.request_context.request_id,
            },
        }

    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.stop_program_run(request)["ok"] is True
    assert captured == {
        "api_version": "manyforge.execution_command.v3",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "stop_program_run",
        "request": request.model_dump(mode="json", exclude_none=True),
    }

    public_client = ExecutionCommandClient(tmp_path)
    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        public_client.stop_program_run(request)
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def _ordinary_lane_forbidden(command: dict[str, Any]) -> dict[str, Any]:
    raise AssertionError(f"{command['command']} reached the ordinary socket")


def test_execution_client_reads_one_exact_internal_stop_status(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path, attribution_transport="internal")
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return {
            "api_version": "manyforge.execution_command_response.v4",
            "command": "read_stop",
            "ok": True,
            "stop_status": {
                "request_id": "program-stop.1",
                "schema_version": "manyforge.execution_stop_status.v1",
                "state": "succeeded",
                "stop_generation": 4,
                "stop_id": "stop.1",
            },
        }

    # Stop status is read beside Stop on the control socket, never on the
    # ordinary one.
    monkeypatch.setattr(client, "_exchange", _ordinary_lane_forbidden)
    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.stop_status("stop.1")["stop_status"]["state"] == "succeeded"
    assert captured == {
        "api_version": "manyforge.execution_command.v3",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "read_stop",
        "stop_id": "stop.1",
    }

    # The additive terminal_projection string, one of three values.
    status: dict[str, Any] = {
        "request_id": "program-stop.1",
        "schema_version": "manyforge.execution_stop_status.v1",
        "state": "succeeded",
        "stop_generation": 4,
        "stop_id": "stop.1",
    }
    reply = {
        "api_version": "manyforge.execution_command_response.v4",
        "command": "read_stop",
        "ok": True,
        "stop_status": status,
    }
    monkeypatch.setattr(client, "_exchange_control", lambda command: copy.deepcopy(reply))
    for projection in ("pending", "recorded", "failed"):
        status["terminal_projection"] = projection
        assert client.stop_status("stop.1")["stop_status"]["terminal_projection"] == projection
    for invalid in ("not_applicable", None, 1, {"state": "recorded"}):
        status["terminal_projection"] = invalid
        with pytest.raises(HeadlessApiError):
            client.stop_status("stop.1")
    status.pop("terminal_projection")
    status["extra"] = "x"
    with pytest.raises(HeadlessApiError):
        client.stop_status("stop.1")

    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        ExecutionCommandClient(tmp_path).stop_status("stop.1")
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def test_execution_client_releases_one_exact_internal_program_run_fence(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path, attribution_transport="internal")
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return {
            "api_version": "manyforge.execution_command_response.v4",
            "command": "release_program_run_stop_fence",
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    assert (
        client.release_program_run_stop_fence("program-run.1", "stop.1", 4)["ok"]
        is True
    )
    assert captured == {
        "api_version": "manyforge.execution_command.v3",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "release_program_run_stop_fence",
        "program_run_id": "program-run.1",
        "stop_generation": 4,
        "stop_id": "stop.1",
    }

    with pytest.raises(HeadlessApiError) as unavailable_publicly:
        ExecutionCommandClient(tmp_path).release_program_run_stop_fence(
            "program-run.1", "stop.1", 4
        )
    assert unavailable_publicly.value.code == ("execution_request_attribution_invalid")


def test_execution_client_projects_scoped_and_full_stop_commands(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    context = {
        "request_id": "request.stop.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
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
    full = FullStopIntentRequest.model_validate(
        {"request_context": context, "expected_stop_generation": 0}
    )
    control_commands: list[dict[str, Any]] = []

    def control_exchange(command: dict[str, Any]) -> dict[str, Any]:
        control_commands.append(command)
        malformed = command["command"] == "malformed_stop"
        return {
            "api_version": "manyforge.execution_command_response.v4",
            "accepted": {
                "request_id": (
                    command["request_id"] if malformed else context["request_id"]
                ),
                "resolution_disposition": (
                    "promoted_to_full" if malformed else "exact"
                ),
                "resolution_reason": (
                    "explicit_full_stop"
                    if command["command"] == "full_stop"
                    else (
                        "malformed_ordinary_intent"
                        if malformed
                        else "exact_current_scope"
                    )
                ),
            },
            "command": "stop" if malformed else command["command"],
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange_control", control_exchange)
    assert client.stop(stop, trusted_principal_id=None)["ok"] is True
    assert client.full_stop(full, trusted_principal_id=None)["ok"] is True
    assert (
        client.recognized_malformed_stop(
            request_id="malformed-stop.http.1",
            recognized_route="/api/execution/stops",
            reason="recognized_stop_payload_invalid",
            trusted_principal_id=None,
        )["ok"]
        is True
    )
    assert set(control_commands[0]["request"]) == {
        "components",
        "request_context",
    }
    assert control_commands[0]["request"]["request_context"] == context
    assert control_commands[2]["command"] == "malformed_stop"



def _direct_motion_request() -> OperationIntentRequest:
    return OperationIntentRequest.model_validate(
        {
            "request_context": {
                "request_id": "preview-request-1",
                "request_owner_epoch": "owner-1",
                "source_label": "composer_ui",
                "correlation_id": None,
            },
            "program_id": "manyforge.direct_control.motion",
            "program_revision": "sha256:program",
            "program_binding_id": "sha256:binding",
            "program_run_id": "direct-control-owner-1",
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
                        "contact_policy_id": None,
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
        "program_id": "manyforge.direct_control.motion",
        "program_revision": "sha256:program",
        "program_run_id": "direct-control-owner-1",
        "request_id": "preview-request-1",
        "request_owner_epoch": "owner-1",
        "required_capability_id": "execution.trajectory.execute",
        "retained_bundle_id": "sha256:bundle",
        "scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 3,
            "content_hash": "sha256:" + "2" * 64,
        },
        "schema_version": "manyforge.direct_control_motion_preview.v4",
        "resolved_tcp": {
            "entry": {
                "target": {"component_id": "robot", "command_target_id": "arm"},
                "frame": {"component_id": "robot", "frame_id": "flange"},
            },
            "source": "active_tcp", "owner_epoch": "owner-1",
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
        "api_version": "manyforge.execution_command_response.v4",
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
    value["request_context"]["request_id"] = "preview-gripper-1"
    value["program_id"] = "manyforge.direct_control.gripper"
    value["program_revision"] = "sha256:gripper-program"
    value["program_binding_id"] = "sha256:gripper-binding"
    value["program_run_id"] = "direct-control-gripper-owner-1"
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
        "program_id": "manyforge.direct_control.gripper",
        "program_revision": "sha256:gripper-program",
        "program_run_id": "direct-control-gripper-owner-1",
        "request_id": "preview-gripper-1",
        "request_owner_epoch": "owner-1",
        "required_capability_id": "tool.gripper.command",
        "retained_bundle_id": "sha256:bundle",
        "schema_version": ("manyforge.direct_control_capability_command_preview.v2"),
        "source_coordinate_name": "finger_joint",
        "source_kind": "model_role",
        "target_position": 0.03,
    }
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return {
        "api_version": "manyforge.execution_command_response.v4",
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
            "api_version": "manyforge.execution_command_response.v4",
            "command": "read_manual_handover",
            "ok": True,
            "availability": {
                "available": True,
                "cleanup_required": False,
                "unavailable_reason": None,
            },
            "request_id": request.request_context.request_id,
            "request_owner_epoch": request.request_context.request_owner_epoch,
            "intent_digest": "sha256:"
            + hashlib.sha256(rfc8785.dumps(operation)).hexdigest(),
            "handover": (
                None
                if case == "idle"
                else {
                    "context": {
                        "program_runner_owner_epoch": "runner.1",
                        "program_run_id": "parent.1",
                        "claim_generation": 1,
                        "selection_revision": 0 if case == "unused" else 3,
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


class _PagedExecution:
    def subscribe_events(self, after_cursor: int, owner_epoch: str | None):
        # The pump reads the subscription only.
        return paged_subscription(self, after_cursor, owner_epoch)

    def __init__(self) -> None:
        self.owner_epoch = "execution-host.epoch.pump"
        self.admission_generation = 1
        self.fail = False
        self.event = _event(self.owner_epoch, 1)
        self.progress: list[dict[str, Any]] = []

    def identity(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v4",
            "command": "identity",
            "ok": True,
            "identity": {"owner_epoch": self.owner_epoch},
        }

    def events(
        self, owner_epoch: str, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.fail:
            raise HeadlessApiError("execution_command_unavailable", "host lost")
        assert owner_epoch == self.owner_epoch
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
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v2",
        }


class _StreamingExecution(_PagedExecution):
    def __init__(self) -> None:
        super().__init__()
        self._events: list[dict[str, Any]] = []
        self._lock = threading.Lock()

    def append(self, event: dict[str, Any]) -> None:
        with self._lock:
            self._events.append(event)

    def events(
        self, owner_epoch: str, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        assert owner_epoch == self.owner_epoch
        with self._lock:
            available = [
                event for event in self._events if event["sequence"] > after_sequence
            ]
            events = available[:limit]
        next_sequence = events[-1]["sequence"] if events else after_sequence
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
            "has_more": len(available) > len(events),
            "next_after_sequence": next_sequence,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v2",
        }


class _IdleExecution:
    def subscribe_events(self, after_cursor: int, owner_epoch: str | None):
        # The pump reads the subscription only.
        return paged_subscription(self, after_cursor, owner_epoch)

    def __init__(
        self,
        *,
        include_epoch: bool = True,
    ) -> None:
        self.owner_epoch = "kernel-owner.runtime-epoch"
        self.admission_generation = 1
        self.include_epoch = include_epoch
        self.status_calls = 0
        self.events_calls = 0

    def identity(self) -> dict[str, Any]:
        self.status_calls += 1
        details = {"owner_epoch": self.owner_epoch} if self.include_epoch else {}
        return {
            "admission_generation": self.admission_generation,
            "api_version": "manyforge.execution_command_response.v4",
            "command": "identity",
            "error": {
                "code": "execution_owner_not_ready",
                "details": details,
                "message": "the managed execution owner has no active physical product",
            },
            "ok": False,
        }

    def events(
        self, _owner_epoch: str, _after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        self.events_calls += 1
        assert _owner_epoch == self.owner_epoch
        return {
            "owner_epoch": self.owner_epoch, "execution_owner_ready": False,
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
            "api_version": "manyforge.execution_command_response.v4",
            "command": "identity",
            "error": {
                "code": "execution_owner_not_ready",
                "details": {"owner_epoch": self.owner_epoch},
                "message": "the managed execution owner has no active physical product",
            },
            "ok": False,
        }

    def events(
        self, owner_epoch: str, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.retired:
            self.fail = False
        elif self.fail:
            self.retired = True
        page = super().events(owner_epoch, after_sequence, limit=limit)
        page["execution_owner_ready"] = not self.retired
        if not after_sequence:
            self.fail = True
        return page


def test_event_pump_delivers_without_frontend_polling_and_stops_cleanly() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert core.ingested[0]["source_owner_epoch"] == execution.owner_epoch
    assert core.ingested[0]["source_sequence"] == 1
    assert core.ingested[0]["payload_json"] == rfc8785.dumps(
        _event(execution.owner_epoch, 1)["payload"]
    ).decode("utf-8")


def test_event_pump_wakes_cold_state_when_kernel_admission_advances() -> None:
    core = _RecordingCore()
    execution = _StreamingExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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

        def events(self, owner_epoch: str, after_sequence: int, *, limit: int = 16):
            return {
                **super().events(owner_epoch, after_sequence, limit=limit),
                "component_states": self.component_states,
            }

    core = _RecordingCore()
    execution = _ComponentExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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

        def events(self, owner_epoch: str, after_sequence: int, *, limit: int = 16):
            return {
                **super().events(owner_epoch, after_sequence, limit=limit),
                "signal_samples": [dict(sample) for sample in self.samples],
            }

    core = _RecordingCore()
    execution = _SignalExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
    pump.set_change_observer(changed.set)
    pump.start()
    assert changed.wait(timeout=1.0)
    changed.clear()
    payload = {
        "api_version": "manyforge.active_tcp_change.v2",
        "owner_epoch": execution.owner_epoch,
        "component_id": "robot-a",
        "previous": None,
        "entry": {"target": {"component_id": "robot-a", "command_target_id": "arm"},
                  "frame": {"component_id": "tool-a", "frame_id": "tcp"}},
        "source": "robot_control",
        "offset_sent": True,
        "registration_generation": 1,
        "sequence": 1,
    }
    event = _event(execution.owner_epoch, 1)
    event.update(record_kind="active_tcp_changed", operation_id=None, operation_revision=None,
                 payload=payload, payload_digest=f"sha256:{hashlib.sha256(rfc8785.dumps(payload)).hexdigest()}")
    try:
        execution.append(event)
        assert changed.wait(timeout=1.0)
        assert core.ingested[0]["record_kind"] == "active_tcp_changed"
        assert pump.last_error is None
    finally:
        pump.shutdown()

def test_event_pump_logs_only_error_transitions(caplog) -> None:
    pump = ExecutionEventPump(_RecordingCore(), _StreamingExecution())
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
    execution.progress = [_progress_row(execution.owner_epoch, 0.1)]
    execution.append(
        _progress_event(execution.owner_epoch, 1, state="executing", progress=0.1)
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.set_change_observer(lambda: wakes.append(None))
    pump.set_progress_observer(samples.append)
    pump.start()
    for _ in range(100):
        if len(core.ingested) >= 1 and wakes:
            break
        threading.Event().wait(0.01)
    first_wake_count = len(wakes)

    execution.progress = [_progress_row(execution.owner_epoch, 0.8)]
    for _ in range(100):
        if samples and samples[-1] == execution.progress:
            break
        threading.Event().wait(0.01)
    threading.Event().wait(0.03)

    assert samples[-1] == execution.progress
    assert len(core.ingested) == 1
    assert len(wakes) == first_wake_count == 1

    execution.append(
        _progress_event(execution.owner_epoch, 2, state="completed", progress=1.0)
    )
    for _ in range(100):
        if len(core.ingested) >= 2 and len(wakes) > first_wake_count:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert len(core.ingested) == 2
    assert len(wakes) == first_wake_count + 1


def _progress_row(epoch: str, progress: float) -> dict[str, Any]:
    return {"owner_epoch": epoch, "operation_id": "operation.1", "operation_revision": 1, "progress": progress}


@pytest.mark.parametrize("invalid", [None, [{"progress": 0.5}], [_progress_row("wrong-owner", 0.5)],
                                     [_progress_row("owner.1", float("nan"))],
                                     [_progress_row("owner.1", 0.5)] * 2])
def test_native_progress_requires_bounded_valid_unique_owner_rows(monkeypatch, tmp_path, invalid) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _page("owner.1", [])
    response["operation_progress"] = invalid
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    with pytest.raises(HeadlessApiError, match="progress"):
        client.events("owner.1", 0)


def test_native_progress_on_empty_journal_page(monkeypatch, tmp_path) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _page("owner.1", [])
    response["operation_progress"] = [_progress_row("owner.1", 0.6)]
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.events("owner.1", 0)["operation_progress"] == response["operation_progress"]


def test_progress_is_latest_only_bounded_and_quiet_when_unchanged() -> None:
    pump = ExecutionEventPump(_RecordingCore(), _StreamingExecution())
    samples = []
    pump.set_progress_observer(samples.append)
    pump._capture_progress([_progress_row("owner.1", 0.1)])
    pump._publish_progress(1.0)
    for sequence in range(2, 9):
        pump._capture_progress([_progress_row("owner.1", sequence / 10)])
        pump._publish_progress(1.1)
    assert len(samples) == 1
    pump._publish_progress(1.21)
    assert samples[-1][0]["progress"] == 0.8
    pump._publish_progress(2.0)
    pump._capture_progress([_progress_row("owner.1", 0.8)])
    pump._publish_progress(2.01)
    assert len(samples) == 2
    pump._capture_progress([])
    pump._publish_progress(2.1)
    assert samples[-1] == []


def test_event_pump_operation_semantic_cache_evicts_at_the_native_resident_bound(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # The change-detection memo is bounded by eviction, never refusal.
    monkeypatch.setattr(
        "manyforge_headless_api.execution.MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS",
        1,
    )
    core = _RecordingCore()
    execution = _StreamingExecution()
    for sequence in (1, 2, 3):
        execution.append(
            _progress_event(
                execution.owner_epoch,
                sequence,
                state="executing",
                progress=0.1,
                operation_id=f"operation.{sequence}",
            )
        )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    for _ in range(200):
        if pump.last_error is not None or any(
            call.get("through_source_sequence") == 3 for call in core.completed_calls
        ):
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert pump.last_error is None
    assert any(call.get("through_source_sequence") == 3 for call in core.completed_calls)

def test_event_pump_reconciles_typed_idle_owner_through_stable_events() -> None:
    core = _RecordingCore()
    execution = _IdleExecution()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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
    assert core.completed_calls[0] == {
        "source_owner_epoch": execution.owner_epoch,
        "through_source_sequence": 0,
    }


def test_event_pump_wakes_cold_state_when_idle_kernel_admission_advances() -> None:
    core = _RecordingCore()
    execution = _IdleExecution()
    changed = threading.Event()
    wakes: list[None] = []
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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


def test_event_pump_reconciles_idle_after_owner_retirement_at_event_high_water() -> (
    None
):
    core = _RecordingCore()
    execution = _RetiringExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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
    assert core.completed_calls[-1] == {
        "source_owner_epoch": execution.owner_epoch,
        "through_source_sequence": 1,
    }


def test_event_pump_rejects_untyped_idle_owner_response() -> None:
    core = _RecordingCore()
    execution = _IdleExecution(include_epoch=False)
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    deadline = threading.Event()
    for _ in range(100):
        if pump.last_error is not None:
            break
        deadline.wait(0.01)
    pump.shutdown()

    # The owner epoch comes only from the subscription's snapshot: an
    # owner that names none never connects, so nothing completes.
    assert pump.last_error is not None
    assert "execution_owner_unavailable" in pump.last_error
    assert not core.completed.is_set()


def test_event_pump_preserves_global_stop_projection_null_identity() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    execution.event = _stop_projection_event(execution.owner_epoch, 1)
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert len(core.ingested) == 1
    assert core.ingested[0]["record_kind"] == "stop_acceptance_projected"
    assert core.ingested[0]["operation_id"] is None
    assert core.ingested[0]["operation_revision"] is None


def test_event_pump_preserves_terminal_stop_projection_and_digest() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    execution.event = _stop_terminal_event(execution.owner_epoch, 1)
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert core.ingested[0]["record_kind"] == "stop_terminal_projected"
    assert core.ingested[0]["request_id"] is None
    assert core.ingested[0]["operation_id"] is None
    assert core.ingested[0]["payload_json"] == rfc8785.dumps(
        execution.event["payload"]
    ).decode("utf-8")
    assert core.ingested[0]["payload_digest"] == execution.event["payload_digest"]


def test_event_pump_marks_a_lost_current_source_unknown() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    execution.fail = True
    assert core.unknown.wait(timeout=1.0)
    pump.shutdown()

    # A lost source is a lost subscription.
    assert pump.last_error is not None
    assert "execution_owner_unavailable" in pump.last_error


def test_operation_settled_classifies_rest_states_and_terminal_stop() -> None:
    def event(state: str) -> dict[str, Any]:
        return {"record_kind": "state_observed", "payload": {"operation_state": {"state": state}}}

    for state in ("completed", "stopped", "failed", "interrupted",
                  "intervention_required", "cancelled_pre_dispatch"):
        assert ExecutionEventPump._operation_is_settled(event(state)), state  # noqa: SLF001
    for state in ("queued", "planning", "validating", "prepared", "executing", "stopping"):
        assert not ExecutionEventPump._operation_is_settled(event(state)), state  # noqa: SLF001
    assert ExecutionEventPump._operation_is_settled(  # noqa: SLF001
        {"record_kind": "stop_terminal_projected", "payload": {}}
    )
    assert not ExecutionEventPump._operation_is_settled(  # noqa: SLF001
        {"record_kind": "stop_acceptance_projected", "payload": {}}
    )


def test_event_pump_keeps_alarm_sources_current_on_every_iteration() -> None:
    """The pump is the alarm-source refresh cadence.

    It offers the refresh on every iteration, also while the execution owner
    is failing, and a failing refresh never degrades the operation feed.
    """
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
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
        "schema_version": "manyforge.kernel_recovery_assessment.v1",
        "assessment_revision": 2,
        "control_command_epoch": 0,
        "execution_id": 1,
        "allowed_actions": ["retry_motion_from_current_state"],
    }
    if retry_eligibility != "absent":
        assessment["retry_eligibility"] = retry_eligibility
    return {
        "api_version": "manyforge.execution_command_response.v4",
        "command": "get_operation_recovery",
        "ok": True,
        "operation_revision": 3,
        "participants": [{"operation_id": "operation.1"}],
        "stop_id": None,
        "stop_generation": None,
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
    assert client.operation_recovery("operation.1")["recovery_assessment"][
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
    assert client.operation_recovery("operation.1")["recovery_assessment"][
        "retry_eligibility"
    ] == eligibility


def test_recovery_assessment_without_retry_eligibility_stays_valid(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = _recovery_response()
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert "retry_eligibility" not in client.operation_recovery("operation.1")[
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
        client.operation_recovery("operation.1")
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
            "api_version": "manyforge.execution_command_response.v4",
            "command": "apply_operation_recovery",
            "ok": True,
            "result": {
                "request_id": request["request_id"],
                "action": request["action"],
                "accepted": True,
                "resulting_assessment": {},
            },
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    client.apply_operation_recovery(
        "operation.1",
        3,
        OperationRecoveryActionRequest.model_validate(
            {
                "request_id": "recovery.1",
                "action": "retry_motion_from_current_state",
                "expected_assessment_revision": 2,
                "expected_control_command_epoch": 0,
                "expected_execution_id": 1,
            }
        ),
    )
    # The native decoder requires exactly these fields.
    assert set(commands[0]["request"]) == {
        "request_id",
        "action",
        "principal_id",
        "expected_assessment_revision",
        "expected_control_command_epoch",
        "expected_execution_id",
        "expected_stop_certificate_id",
        "expected_stop_owner_generation",
        "reason",
    }


def test_recovery_assessment_reason_union_is_the_frozen_seventeen() -> None:
    assert len(RETRY_ELIGIBILITY_REASONS) == 17
    assert "none" in RETRY_ELIGIBILITY_REASONS


def test_operation_events_pass_retry_fact_payloads_through_unchanged(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    # Native ingest owns the event-level wire shapes; Python only forwards them.
    owner_epoch = "execution.epoch.1"
    event = copy.deepcopy(_event(owner_epoch, 1))
    event["payload"]["api_version"] = "manyforge.operation_journal_record.v4"
    event["payload"]["operation_state"]["motion_retry_candidate"] = "not_a_candidate"
    client = ExecutionCommandClient(tmp_path)
    response = _page(owner_epoch, [event])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
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
                          "details": {"owner_epoch": self.owner_epoch}},
            }

        def events(self, owner_epoch: str, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
            page = super().events(owner_epoch, after_sequence, limit=limit)
            page["execution_owner_ready"] = self.ready
            return page

    core = _RecordingCore()
    execution = ModeExecution()
    wakes: list[bool] = []
    changed = threading.Event()
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
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
    assert all(call["source_owner_epoch"] == execution.owner_epoch
               for call in core.completed_calls)


@pytest.mark.parametrize("malformed_kind", [None, {}], ids=["typed_error", "type_error"])
def test_page_parse_errors_keep_runtime_fault_context_for_pump(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, malformed_kind: Any
) -> None:
    epoch = "kernel.1"
    fault = {
        "request_owner_epoch": epoch, "write_kind": "state_observed",
        "error": {"code": "io_error", "message": "Journal failed", "details": {}},
        "operation_id": None, "operation_revision": None, "request_id": None,
        "program_run_id": None, "execution_scope": None,
    }
    response = _page(epoch, [_event(epoch, 1)])
    response["runtime_faults"]["operation_journal_write_fault"] = fault
    response["event_page"]["events"][0]["record_kind"] = malformed_kind
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _request: response)
    expected_error = HeadlessApiError if malformed_kind is None else TypeError
    with pytest.raises(expected_error) as failed:
        client.events(epoch, 0, limit=1)
    snapshot = failed.value.runtime_fault_snapshot
    assert snapshot["owner_epoch"] == epoch
    assert snapshot["runtime_faults"]["operation_journal_write_fault"] == fault

    class FailedExecution(_PagedExecution):
        """Streams the same malformed row with the same runtime fault: the
        pump's page parse fails with the fault context."""

        def __init__(self) -> None:
            super().__init__()
            self.owner_epoch = epoch
            self.event = response["event_page"]["events"][0]

        def events(self, owner_epoch: str, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
            page = super().events(owner_epoch, after_sequence, limit=limit)
            page["runtime_faults"] = response["runtime_faults"]
            return page

    core = _RecordingCore()
    pump = ExecutionEventPump(core, FailedExecution(), poll_seconds=0.01, page_limit=1)  # type: ignore[arg-type]
    pump.start()
    assert core.unknown.wait(timeout=1.0)
    pump.shutdown()
    assert core.runtime_fault_snapshots
    assert json.loads(core.runtime_fault_snapshots[0]["current_runtime_faults_json"])[
        "operation_journal_write_fault"
    ] == fault
    assert core.completed_calls == []
    assert pump.last_error is not None  # Fault delivery does not turn page failure into Good.


def _rule_state_event(owner_epoch: str, sequence: int, state: str) -> dict[str, Any]:
    payload = {
        "owner_epoch": owner_epoch,
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "rule_id": "door",
        "state": state,
        "triggering_acquisition": None,
    }
    return {
        "effect_pending": False,
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:01.00000000{sequence}Z",
        "operation_id": None,
        "operation_revision": None,
        "owner_epoch": owner_epoch,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(rfc8785.dumps(payload)).hexdigest()}",
        "record_kind": "interrupt_rule_state_changed",
        "request_id": None,
        "sequence": sequence,
    }


def _rule_catalog(owner_epoch: str) -> dict[str, Any]:
    return {
        "owner_epoch": owner_epoch,
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "bundle": "bundle.1",
        "graph": "graph.1",
        "rule_ids": ["door"],
        "referenced_program_blackboard_keys": [],
    }


def _rule_baseline(owner_epoch: str, through: int) -> dict[str, Any]:
    return {"status": "ready", "snapshot": {
        "owner_epoch": owner_epoch,
        "catalog_generation": "1",
        "active_publication_generation": 1,
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "through_sequence": through,
        "states": [{"rule_id": "door", "state": "clear", "triggering_acquisition": None}],
    }}


def test_execution_client_passes_global_rule_wire_and_null_identity_state_rows(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.rules"
    row = _rule_state_event(owner_epoch, 1, "tripped")
    response = _page(owner_epoch, [row])
    response["interrupt_rule_catalog"] = _rule_catalog(owner_epoch)
    response["interrupt_rule_state_baseline"] = {"status": "projection_pending"}
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
    assert page["events"][0]["record_kind"] == "interrupt_rule_state_changed"
    assert page["interrupt_rule_catalog"] == response["interrupt_rule_catalog"]
    assert page["interrupt_rule_state_baseline"] == {"status": "projection_pending"}

    row["operation_id"] = "operation.1"
    with pytest.raises(HeadlessApiError):
        client.events(owner_epoch, 0)


class _RuleStreamingExecution(_StreamingExecution):
    def __init__(self) -> None:
        super().__init__()
        self.baseline_through = 0

    def events(self, owner_epoch: str, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        page = super().events(owner_epoch, after_sequence, limit=limit)
        page["interrupt_rule_catalog"] = _rule_catalog(owner_epoch)
        page["interrupt_rule_state_baseline"] = _rule_baseline(owner_epoch, self.baseline_through)
        return page


def test_event_pump_projects_global_rules_and_forwards_rule_rows_to_the_historian() -> None:
    core = _RecordingCore()
    execution = _RuleStreamingExecution()
    epoch = execution.owner_epoch
    execution.append(_event(epoch, 1))
    execution.append(_rule_state_event(epoch, 2, "tripped"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01)  # type: ignore[arg-type]
    pump.set_global_rules_observer(published.append)
    pump.start()
    for _ in range(200):
        if core.completed_calls and published:
            break
        threading.Event().wait(0.01)
    execution.append(_rule_state_event(epoch, 3, "inactive"))
    for _ in range(200):
        if published and published[-1] == [{"rule_id": "door", "state": "inactive", "keys": []}]:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    # Rule rows reach the historian unchanged; snapshots cover them.
    assert [row["source_sequence"] for row in core.ingested] == [1, 2, 3]
    tripped = _rule_state_event(epoch, 2, "tripped")
    assert core.ingested[1] == {
        "source_owner_epoch": epoch,
        "source_sequence": 2,
        "record_kind": "interrupt_rule_state_changed",
        "request_id": None,
        "operation_id": None,
        "operation_revision": None,
        "occurred_at_ns": tripped["occurred_at_ns"],
        "occurred_at_wall": tripped["occurred_at_wall"],
        "effect_pending": False,
        "payload_json": rfc8785.dumps(tripped["payload"]).decode("utf-8"),
        "payload_digest": tripped["payload_digest"],
    }
    assert set(json.loads(core.ingested[1]["payload_json"])) == {
        "owner_epoch", "catalog_generation", "active_publication_generation",
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
    execution.append(_event(execution.owner_epoch, 1))
    execution.append(_rule_state_event(execution.owner_epoch, 2, "exploded"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01)  # type: ignore[arg-type]
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
            if core.completed_calls and published:
                break
            threading.Event().wait(0.01)
        pump.shutdown()
    finally:
        module_logger.removeHandler(caplog.handler)
        module_logger.setLevel(previous_level)

    # The historian still receives the row; only the GUI fold is unreadable.
    assert [row["source_sequence"] for row in core.ingested] == [1, 2]
    # The lost row is not covered by the cut (through 0): pending, not clear.
    assert published == [[{"rule_id": "door", "state": "pending", "keys": []}]]
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
    execution.append(_event(execution.owner_epoch, 1))
    execution.append(_rule_state_event(execution.owner_epoch, 2, "tripped"))
    published: list[list[dict[str, Any]] | None] = []
    pump = ExecutionEventPump(core, execution, poll_seconds=0.01)  # type: ignore[arg-type]
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
    owner_epoch = "execution-host.epoch.tcp"
    row = _event(owner_epoch, 1)
    row.update(record_kind="active_tcp_changed", operation_id=None, operation_revision=None,
               request_id=None, payload={"api_version": "manyforge.active_tcp_change.v2",
                   "owner_epoch": owner_epoch, "component_id": "robot", "previous": None,
                   "entry": None, "source": "reset", "offset_sent": False,
                   "registration_generation": 1, "sequence": 1})
    row["payload_digest"] = "sha256:" + hashlib.sha256(rfc8785.dumps(row["payload"])).hexdigest()
    response = _page(owner_epoch, [row])
    baseline = [{"component_id": "robot", "entry": None, "owner_epoch": owner_epoch,
                 "registration_generation": 1, "sequence": 1}]
    response["active_tcp_baseline"] = baseline
    client = ExecutionCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
    assert page["active_tcp_baseline"] == baseline
    assert page["events"][0]["payload"] == row["payload"]
    row["operation_id"] = "unexpected"
    with pytest.raises(HeadlessApiError):
        client.events(owner_epoch, 0)


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
    rows = [{"component_id": "robot", "entry": None, "owner_epoch": "owner.1",
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
    receipt["schema_version"] = "manyforge.direct_control_motion_preview.v4"
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
    response = {"api_version": "manyforge.execution_command_response.v4", "ok": False,
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
        "api_version": "manyforge.execution_command_response.v4",
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
    failure = {"api_version": "manyforge.execution_command_response.v4", "ok": False,
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
