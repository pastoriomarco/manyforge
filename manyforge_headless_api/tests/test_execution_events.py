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
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
)
from manyforge_headless_api.native import HeadlessApiError


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


def _stop_fault_clear_event(owner_epoch: str, sequence: int) -> dict[str, Any]:
    payload = {
        "api_version": "manyforge.stop_fault_clear_projection.v1",
        "fault_latch": {
            "captured_operation_count": 1,
            "captured_resource_count": 1,
            "cell_id": "cell.test",
            "latch_generation": 1,
            "latch_id": "stop-fault.1",
            "persistence_degraded": False,
            "reason_code": "stop_fault_recovery_cleared",
            "recovery_assessment_revision": 2,
            "recovery_status": "cleared",
            "state": "cleared",
            "stop_certificate": {
                "stop_certificate_id": "stop-certificate.1",
                "stop_certificate_revision": 1,
                "stop_generation": 1,
                "stop_id": "stop.1",
            },
            "stop_generation": 1,
            "stop_id": "stop.1",
            "trigger": "escalated",
        },
        "owner_epoch": owner_epoch,
        "record_kind": "stop_fault_cleared",
        "request_id": "stop-fault-reset.1",
        "result": {
            "accepted": True,
            "error": None,
            "replay_status": "accepted_new",
            "request_id": "stop-fault-reset.1",
        },
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
        "record_kind": "stop_fault_cleared",
        "request_id": "stop-fault-reset.1",
        "sequence": sequence,
    }


def _page(owner_epoch: str, events: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "operation_progress": [],
        "admission_generation": 1,
        "api_version": "manyforge.execution_command_response.v1",
        "command": "events",
        "owner_epoch": owner_epoch,
        "execution_owner_ready": True,
        "runtime_faults": {"operation_journal_write_fault": None, "program_runner_loss": None},
        "event_page": {
            "events": events,
            "has_more": False,
            "next_after_sequence": events[-1]["sequence"] if events else 0,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
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
            "api_version": "manyforge.execution_command_response.v1",
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
        {"api_version": "manyforge.execution_command.v1", "command": command}
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
            "api_version": "manyforge.execution_command_response.v1",
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


def test_execution_client_accepts_stop_fault_clear_as_request_scoped_fact(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.stop"
    cleared = _stop_fault_clear_event(owner_epoch, 1)
    monkeypatch.setattr(
        client, "_exchange", lambda _command: _page(owner_epoch, [cleared])
    )
    page = client.events(owner_epoch, 0)
    assert page["events"][0]["record_kind"] == "stop_fault_cleared"
    assert page["events"][0]["request_id"] == "stop-fault-reset.1"
    assert page["events"][0]["operation_id"] is None

    cleared["request_id"] = None
    with pytest.raises(HeadlessApiError) as invalid_request_identity:
        client.events(owner_epoch, 0)
    assert invalid_request_identity.value.code == ("execution_command_response_invalid")


def test_execution_client_requires_backend_residency_projection(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    response = {
        "api_version": "manyforge.execution_command_response.v1",
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
            "api_version": "manyforge.execution_command_response.v1",
            "command": "stop_program_run",
            "ok": True,
            "accepted": {
                "request_id": request.request_context.request_id,
            },
        }

    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.stop_program_run(request)["ok"] is True
    assert captured == {
        "api_version": "manyforge.execution_command.v1",
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
            "api_version": "manyforge.execution_command_response.v1",
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
        "api_version": "manyforge.execution_command.v1",
        "attribution": {
            "transport": "internal",
            "assurance": "unverified",
            "principal_id": None,
        },
        "command": "read_stop",
        "stop_id": "stop.1",
    }

    # SX-2226: the additive terminal_projection string, one of three values.
    status: dict[str, Any] = {
        "request_id": "program-stop.1",
        "schema_version": "manyforge.execution_stop_status.v1",
        "state": "succeeded",
        "stop_generation": 4,
        "stop_id": "stop.1",
    }
    reply = {
        "api_version": "manyforge.execution_command_response.v1",
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
            "api_version": "manyforge.execution_command_response.v1",
            "command": "release_program_run_stop_fence",
            "ok": True,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    assert (
        client.release_program_run_stop_fence("program-run.1", "stop.1", 4)["ok"]
        is True
    )
    assert captured == {
        "api_version": "manyforge.execution_command.v1",
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


def test_execution_client_projects_scoped_full_and_fault_recovery_commands(
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
            "api_version": "manyforge.execution_command_response.v1",
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

    latch = {
        "captured_operation_ids": ["operation.1"],
        "captured_resources": ["joint.1"],
        "cell_id": "cell.1",
        "clear_projection": {
            "error": None,
            "retry_generation": None,
            "state": "not_applicable",
        },
        "evidence_refs": [],
        "graph_revision": "sha256:" + "2" * 64,
        "latch_generation": 1,
        "latch_id": "stop-fault.1",
        "persistence_degraded": False,
        "reason_code": "stop_fault_recovery_ready",
        "recovery_assessment_revision": 2,
        "recovery_status": "ready",
        "retained_bundle_id": "sha256:" + "3" * 64,
        "state": "latched",
        "stop_certificate": {
            "stop_certificate_id": "stop-certificate.1",
            "stop_certificate_revision": 1,
            "stop_generation": 1,
            "stop_id": "stop.1",
        },
        "stop_generation": 1,
        "stop_id": "stop.1",
        "terminal_projection": {
            "error": None,
            "retry_generation": None,
            "state": "recorded",
        },
        "trigger": "scope_promotion",
    }
    expectation = {
        "active_control_configuration_generation": 4,
        "active_control_configuration_id": "configuration.1",
        "control_domain_id": "domain.1",
    }
    normal_commands: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        normal_commands.append(command)
        if command["command"] == "get_stop_fault":
            return {
                "api_version": "manyforge.execution_command_response.v1",
                "command": "get_stop_fault",
                "ok": True,
                "stop_fault": {
                    "configuration_expectations": [expectation],
                    "latch": latch,
                    "recovery_unavailable_reason": None,
                },
            }
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "recover_stop_fault",
            "ok": True,
            "result": {
                "accepted": True,
                "error": None,
                "latch": {**latch, "state": "cleared"},
                "replay_status": "accepted_new",
                "request_id": "request.recover.1",
            },
        }

    def ordinary_exchange(command: dict[str, Any]) -> dict[str, Any]:
        assert command["command"] != "get_stop_fault"
        return exchange(command)

    def control_exchange(command: dict[str, Any]) -> dict[str, Any]:
        assert command["command"] == "get_stop_fault"
        return exchange(command)

    # The Stop-fault read is served on the control socket; recovery is not.
    monkeypatch.setattr(client, "_exchange", ordinary_exchange)
    monkeypatch.setattr(client, "_exchange_control", control_exchange)
    offer = client.stop_fault()["stop_fault"]
    recovery = StopFaultRecoveryIntentRequest.model_validate(
        {
            "request_context": {
                **context,
                "request_id": "request.recover.1",
            },
            "expected_latch_id": latch["latch_id"],
            "expected_latch_generation": latch["latch_generation"],
            "expected_recovery_assessment_revision": latch[
                "recovery_assessment_revision"
            ],
            "expected_stop_certificate_id": latch["stop_certificate"][
                "stop_certificate_id"
            ],
            "expected_stop_certificate_revision": latch["stop_certificate"][
                "stop_certificate_revision"
            ],
            "expected_cell_id": latch["cell_id"],
            "expected_retained_bundle_id": latch["retained_bundle_id"],
            "expected_graph_revision": latch["graph_revision"],
            "configuration_expectations": offer["configuration_expectations"],
            "reason": "operator confirmed non-moving clear",
        }
    )
    assert client.recover_stop_fault(recovery)["result"]["accepted"] is True
    assert [command["command"] for command in normal_commands] == [
        "get_stop_fault",
        "recover_stop_fault",
    ]
    assert normal_commands[1]["request"]["request_context"]["correlation_id"] is None

    # The same response can publish a fault before containment is settled,
    # but cannot advertise recovery readiness or fabricated configuration.
    pending = copy.deepcopy(offer)
    pending["configuration_expectations"] = []
    pending["latch"].update(
        stop_certificate=None,
        recovery_status="waiting_for_observation",
        terminal_projection={
            "error": None,
            "retry_generation": None,
            "state": "pending",
        },
    )
    assert client._valid_stop_fault_offer(pending)
    for key, value in (("recovery_status", "ready"), ("state", "recovery_pending")):
        invalid = copy.deepcopy(pending)
        invalid["latch"][key] = value
        assert not client._valid_stop_fault_offer(invalid)
    invalid = copy.deepcopy(pending)
    invalid["configuration_expectations"] = [expectation]
    assert not client._valid_stop_fault_offer(invalid)
    invalid = copy.deepcopy(offer)
    invalid["latch"]["stop_certificate"]["stop_id"] = "wrong-stop"
    assert not client._valid_stop_fault_offer(invalid)

    # A certified latch names why recovery is unavailable instead of carrying
    # an empty proof; a proof and a reason never appear together.
    unavailable = copy.deepcopy(offer)
    unavailable["configuration_expectations"] = []
    unavailable["recovery_unavailable_reason"] = (
        "stop_fault_recovery_installed_configuration_unavailable"
    )
    assert client._valid_stop_fault_offer(unavailable)
    invalid = copy.deepcopy(unavailable)
    invalid["recovery_unavailable_reason"] = None
    assert not client._valid_stop_fault_offer(invalid)
    invalid = copy.deepcopy(offer)
    invalid["recovery_unavailable_reason"] = "stop_fault_recovery_installed_configuration_unavailable"
    assert not client._valid_stop_fault_offer(invalid)
    invalid = copy.deepcopy(offer)
    del invalid["recovery_unavailable_reason"]
    assert not client._valid_stop_fault_offer(invalid)


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
            "expected_scene_revision": {
                "scene_id": "sha256:" + "1" * 64,
                "revision_id": 3,
                "content_hash": "sha256:" + "2" * 64,
            },
            "participants": [
                {
                    "component_role_id": "controlled_component",
                    "target_role_id": "command_target",
                    "required_capability_id": "execution.trajectory.execute",
                    "action_span": None,
                    "payload": {
                        "kind": "motion",
                        "move_id": "robot-control-preview-request-1",
                        "base_frame_role_id": "base",
                        "tcp_endpoint": {"target_role_id": "command_target", "frame_role_id": "tcp"},
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
                        "planning_profile": "balanced",
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
        "component_role_id": "controlled_component",
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
        "schema_version": "manyforge.direct_control_motion_preview.v1",
        "source_joint_names": ["joint1"],
        "start_positions_rad": [0.0],
        "target_role_id": "command_target",
    }
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "preview_motion",
        "ok": True,
        "preview_receipt": receipt,
    }


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
        "component_role_id": "controlled_component",
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
        "scene_revision": operation["expected_scene_revision"],
        "schema_version": ("manyforge.direct_control_capability_command_preview.v1"),
        "source_coordinate_name": "finger_joint",
        "source_kind": "model_role",
        "target_position": 0.03,
        "target_role_id": "command_target",
    }
    receipt["receipt_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(receipt)).hexdigest()
    )
    return {
        "api_version": "manyforge.execution_command_response.v1",
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
def test_manual_handover_query_is_one_bounded_identity_checked_read(
    tmp_path, monkeypatch, case
):
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    captured = []

    def exchange(command):
        captured.append(command)
        operation = command["operation"]
        result = {
            "api_version": "manyforge.execution_command_response.v1",
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
            result["intent_digest"] = "sha256:" + "0" * 64
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
        "stale",
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


@pytest.mark.parametrize(
    ("revision_id", "scene_id", "accepted"),
    [
        (4, "sha256:" + "1" * 64, True),
        (2, "sha256:" + "1" * 64, False),
        (4, "sha256:" + "9" * 64, False),
    ],
)
def test_motion_preview_receipt_may_name_a_newer_revision_of_the_same_scene(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    revision_id: int,
    scene_id: str,
    accepted: bool,
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_motion_request()
    latched = {
        "scene_id": scene_id,
        "revision_id": revision_id,
        "content_hash": "sha256:" + "3" * 64,
    }

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        return _with_scene_revision(_preview_response(command["operation"]), latched)

    monkeypatch.setattr(client, "_exchange", exchange)
    if accepted:
        response = client.preview_motion(request, trusted_principal_id=None)
        assert response["preview_receipt"]["scene_revision"] == latched
    else:
        with pytest.raises(HeadlessApiError) as mismatch:
            client.preview_motion(request, trusted_principal_id=None)
        assert mismatch.value.code == "execution_command_response_identity_mismatch"


def test_capability_command_preview_receipt_echoes_the_exact_scene_revision(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    request = _direct_command_request()
    newer = {
        "scene_id": "sha256:" + "1" * 64,
        "revision_id": 4,
        "content_hash": "sha256:" + "3" * 64,
    }

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        return _with_scene_revision(
            _command_preview_response(command["operation"]), newer
        )

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as mismatch:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert mismatch.value.code == "execution_command_response_identity_mismatch"


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

    def stale_schema(command: dict[str, Any]) -> dict[str, Any]:
        response = _command_preview_response(command["operation"])
        response["preview_receipt"]["command_schema_revision"] = "sha256:stale"
        return response

    monkeypatch.setattr(client, "_exchange", stale_schema)
    with pytest.raises(HeadlessApiError) as mismatch:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert mismatch.value.code == "execution_command_response_identity_mismatch"


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
    def __init__(self) -> None:
        self.owner_epoch = "execution-host.epoch.pump"
        self.admission_generation = 1
        self.fail = False
        self.event = _event(self.owner_epoch, 1)
        self.stop_fault_offer: dict[str, Any] | None = None
        self.stop_fault_calls = 0
        self.progress: list[dict[str, Any]] = []

    def identity(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v1",
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
            "runtime_faults": {"operation_journal_write_fault": None, "program_runner_loss": None},
            "operation_progress": self.progress,
            "admission_generation": self.admission_generation,
            "events": events,
            "has_more": False,
            "next_after_sequence": after_sequence or 1,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
        }

    def stop_fault(self) -> dict[str, Any]:
        self.stop_fault_calls += 1
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "get_stop_fault",
            "ok": True,
            "stop_fault": self.stop_fault_offer,
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
            "runtime_faults": {"operation_journal_write_fault": None, "program_runner_loss": None},
            "operation_progress": self.progress,
            "admission_generation": self.admission_generation,
            "events": events,
            "has_more": len(available) > len(events),
            "next_after_sequence": next_sequence,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
        }


def _stop_fault_response(stop_fault: Any) -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "get_stop_fault",
        "ok": True,
        "stop_fault": stop_fault,
    }


def _active_stop_fault_offer() -> dict[str, Any]:
    return {
        "recovery_unavailable_reason": None,
        "configuration_expectations": [],
        "latch": {
            "captured_operation_ids": [],
            "captured_resources": [],
            "cell_id": "cell.test",
            "clear_projection": {
                "error": None,
                "retry_generation": None,
                "state": "not_applicable",
            },
            "evidence_refs": [],
            "graph_revision": "sha256:" + "2" * 64,
            "latch_generation": 1,
            "latch_id": "stop-fault.test.idle",
            "persistence_degraded": False,
            "reason_code": "stop_fault_latched",
            "recovery_assessment_revision": 1,
            "recovery_status": "waiting_for_observation",
            "retained_bundle_id": "sha256:" + "3" * 64,
            "state": "latched",
            "stop_certificate": None,
            "stop_generation": 1,
            "stop_id": "stop.test.idle",
            "terminal_projection": {
                "error": None,
                "retry_generation": None,
                "state": "pending",
            },
            "trigger": "physical_rest_unproven",
        },
    }


class _IdleExecution:
    def __init__(
        self,
        *,
        include_epoch: bool = True,
        stop_fault_response: dict[str, Any] | None = None,
    ) -> None:
        self.owner_epoch = "kernel-owner.runtime-epoch"
        self.admission_generation = 1
        self.include_epoch = include_epoch
        self.status_calls = 0
        self.events_calls = 0
        self.stop_fault_calls = 0
        self.stop_fault_response = stop_fault_response

    def identity(self) -> dict[str, Any]:
        self.status_calls += 1
        details = {"owner_epoch": self.owner_epoch} if self.include_epoch else {}
        return {
            "admission_generation": self.admission_generation,
            "api_version": "manyforge.execution_command_response.v1",
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
            "runtime_faults": {"operation_journal_write_fault": None, "program_runner_loss": None},
            "events": [], "has_more": False, "next_after_sequence": _after_sequence,
            "operation_progress": [], "admission_generation": self.admission_generation,
        }

    def stop_fault(self) -> dict[str, Any]:
        self.stop_fault_calls += 1
        if self.stop_fault_response is not None:
            return self.stop_fault_response
        return _stop_fault_response(None)


class _RetiringExecution(_PagedExecution):
    def __init__(self) -> None:
        super().__init__()
        self.retired = False

    def identity(self) -> dict[str, Any]:
        if not self.retired:
            return super().identity()
        return {
            "admission_generation": self.admission_generation,
            "api_version": "manyforge.execution_command_response.v1",
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


def test_event_pump_logs_only_error_transitions(caplog) -> None:
    pump = ExecutionEventPump(_RecordingCore(), _StreamingExecution())
    error = HeadlessApiError("execution_command_unavailable", "host lost")
    with caplog.at_level("INFO", logger="manyforge_headless_api.execution"):
        assert pump._record_error(error)  # noqa: SLF001
        assert not pump._record_error(error)  # noqa: SLF001
        assert pump._record_error(None)  # noqa: SLF001
        assert not pump._record_error(None)  # noqa: SLF001
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


def test_event_pump_operation_semantic_cache_has_the_native_resident_bound(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        "manyforge_headless_api.execution.MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS",
        1,
    )
    core = _RecordingCore()
    execution = _StreamingExecution()
    execution.append(
        _progress_event(
            execution.owner_epoch,
            1,
            state="executing",
            progress=0.1,
            operation_id="operation.1",
        )
    )
    execution.append(
        _progress_event(
            execution.owner_epoch,
            2,
            state="executing",
            progress=0.1,
            operation_id="operation.2",
        )
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    for _ in range(100):
        if pump.last_error is not None:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert pump.last_error is not None
    assert "execution_control_projection_capacity_invalid" in pump.last_error


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
        if execution.status_calls >= 4:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert pump.last_error is None
    assert execution.status_calls >= 1
    assert execution.events_calls >= 1
    assert execution.status_calls >= 4
    assert wakes == [None]
    assert core.completed_calls[0] == {
        "current_stop_fault_json": "null",
        "source_owner_epoch": execution.owner_epoch,
        "through_source_sequence": 0,
    }


def test_event_pump_completes_idle_snapshot_with_active_stop_fault_offer() -> None:
    core = _RecordingCore()
    offer = _active_stop_fault_offer()
    assert ExecutionCommandClient._valid_stop_fault_offer(offer)
    execution = _IdleExecution(
        stop_fault_response=_stop_fault_response(offer)
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert execution.events_calls >= 1
    assert execution.stop_fault_calls >= 1
    assert core.completed_calls[0] == {
        "current_stop_fault_json": rfc8785.dumps(offer["latch"]).decode("utf-8"),
        "source_owner_epoch": execution.owner_epoch,
        "through_source_sequence": 0,
    }


@pytest.mark.parametrize(
    ("stop_fault_response", "expected_error"),
    [
        (
            {
                "api_version": "manyforge.execution_command_response.v1",
                "error": {
                    "code": "execution_command_unavailable",
                    "details": {},
                    "message": "the stop-fault snapshot is temporarily unavailable",
                },
                "ok": False,
            },
            "execution_command_unavailable",
        ),
        (
            {
                "api_version": "manyforge.execution_command_response.v1",
                "command": "get_stop_fault",
                "ok": False,
            },
            "execution_command_response_invalid",
        ),
    ],
    ids=["unavailable", "malformed_error_envelope"],
)
def test_event_pump_keeps_idle_source_open_when_stop_fault_read_fails(
    stop_fault_response: dict[str, Any],
    expected_error: str,
) -> None:
    core = _RecordingCore()
    execution = _IdleExecution(stop_fault_response=stop_fault_response)
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

    assert pump.last_error is not None
    assert expected_error in pump.last_error
    assert execution.events_calls >= 1
    assert execution.stop_fault_calls >= 1
    assert core.completed_calls == []
    assert not core.completed.is_set()


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


def test_event_pump_supplies_fresh_current_stop_fault_condition() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    latch = {"cell_id": "cell.test", "latch_id": "stop-fault.test.1"}
    execution.stop_fault_offer = {
        "configuration_expectations": [],
        "latch": latch,
        "recovery_unavailable_reason": None,
    }
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core,
        execution,
        poll_seconds=0.01,
        condition_poll_seconds=0.05,
        page_limit=1,
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert execution.stop_fault_calls >= 1
    assert core.completed_calls[0]["current_stop_fault_json"] == (
        rfc8785.dumps(latch).decode("utf-8")
    )


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

    assert pump.last_error is not None
    assert "execution_owner_not_ready" in pump.last_error
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

    assert pump.last_error is not None
    assert "execution_command_unavailable" in pump.last_error


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
    """The pump is the alarm-source refresh cadence (spec 530 §11).

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


class _ComponentConditionCore(_RecordingCore):
    def __init__(self, component_ids: list[str]) -> None:
        super().__init__()
        self.component_ids = component_ids
        self.source_registered = False
        self.configuration: tuple[str, str] | None = ("bundle.1", "graph.1")
        self.observed: list[dict[str, Any]] = []
        self.observed_empty: list[dict[str, Any]] = []
        self.empty_status = "observed"
        self.observe_status: str | None = None
        self.marked_unknown = 0
        self.observation_pending = False

    def component_lifecycle_condition_scope(self) -> dict[str, Any]:
        bundle, graph = self.configuration or (None, None)
        return {
            "component_ids": list(self.component_ids),
            "graph_revision": graph,
            "retained_bundle_id": bundle,
            "source_registered": self.source_registered,
        }

    def refresh_alarm_sources(self) -> dict[str, Any]:
        self.alarm_refreshes += 1
        return {
            "component_lifecycle_observation_pending": self.observation_pending,
            "refreshed": True,
        }

    def observe_component_lifecycle_conditions(self, **kwargs: Any) -> dict[str, Any]:
        self.observed.append(kwargs)
        if self.observe_status is not None:
            return {"status": self.observe_status}
        # Like the core: an unread component leaves quality unknown.
        if kwargs.get("unavailable_component_ids"):
            return {"status": "incomplete"}
        self.observation_pending = False
        return {"status": "observed"}

    def observe_empty_component_lifecycle_configuration(
        self, **kwargs: Any
    ) -> dict[str, Any]:
        self.observed_empty.append(kwargs)
        return {"status": self.empty_status}

    def mark_component_lifecycle_source_unknown(self) -> dict[str, Any]:
        self.marked_unknown += 1
        return {"status": "marked_unknown"}


def _component_snapshot(component_id: str, cause: str) -> dict[str, Any]:
    return {
        "active_public_pointer_digest": "sha256:" + "a" * 64,
        "component_lifecycle_generation": 3,
        "component_lifecycle_state": "active",
        "controllable_component_id": component_id,
        "domains": [
            {
                "control_domain_id": "domain.arm",
                "kind": "controller",
                "resources": ["resource.arm"],
                "topology_revision": "topology.rev",
            }
        ],
        "graph_revision": "graph.1",
        "lifecycle_progress": {
            "blocking_cause_class": cause,
            "blocking_reason": None,
            "delayed": True,
            "incident": None,
            "next_intent": None,
            "phase": "satisfied",
            "waiting_since_ns": 1_759_900_000_000,
        },
        "lifecycle_target": {
            "origin": "startup",
            "requested_state": "active",
            "target_generation": 1,
        },
        "provider_occurrences": [],
        "retained_bundle_id": "bundle.1",
        "snapshot_digest": "sha256:" + "b" * 64,
    }


class _FakeCCLC:
    def __init__(self) -> None:
        self.epochs = ["cclc.epoch.1"]
        self.snapshots: dict[str, Any] = {}
        self.calls: list[str] = []

    def owner_identity(self) -> dict[str, Any]:
        self.calls.append("owner_identity")
        epoch = self.epochs[0] if len(self.epochs) == 1 else self.epochs.pop(0)
        if isinstance(epoch, Exception):
            raise epoch
        return {"request_owner_epoch": epoch}

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        self.calls.append(component_id)
        value = self.snapshots[component_id]
        if isinstance(value, Exception):
            raise value
        return value


def _condition_pump(
    core: _ComponentConditionCore, cclc: _FakeCCLC | None
) -> ExecutionEventPump:
    return ExecutionEventPump(  # type: ignore[arg-type]
        core, _PagedExecution(), cclc=cclc, component_condition_poll_seconds=5.0
    )


def test_component_conditions_forward_only_condition_facts_at_a_bounded_cadence() -> None:
    core = _ComponentConditionCore(["arm", "gripper"])
    cclc = _FakeCCLC()
    cclc.snapshots = {
        "arm": _component_snapshot("arm", "emergency_stop"),
        "gripper": HeadlessApiError("component_lifecycle_snapshot_unavailable", "no row"),
    }
    pump = _condition_pump(core, cclc)
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 1
    observed = core.observed[0]
    assert observed["cclc_owner_epoch"] == "cclc.epoch.1"
    assert observed["unavailable_component_ids"] == ["gripper"]
    snapshots = rfc8785.dumps(
        [
            {
                "component_lifecycle_state": "active",
                "controllable_component_id": "arm",
                "domains": [
                    {
                        "control_domain_id": "domain.arm",
                        "resources": ["resource.arm"],
                    }
                ],
                "graph_revision": "graph.1",
                "lifecycle_progress": {
                    "blocking_cause_class": "emergency_stop",
                    "blocking_reason": None,
                    "incident": None,
                    "phase": "satisfied",
                },
                "lifecycle_target": {
                    "origin": "startup",
                    "requested_state": "active",
                    "target_generation": 1,
                },
                "retained_bundle_id": "bundle.1",
            }
        ]
    ).decode("utf-8")
    # Wait timing is presentation only and never crosses the boundary.
    assert observed["snapshots_json"] == snapshots
    # Within the period nothing is read again.
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 1
    assert cclc.calls == ["owner_identity", "arm", "gripper", "owner_identity"]


def test_a_provider_domain_component_completes_the_lifecycle_observation() -> None:
    # A model-free I/O provider domain carries the same common `resources`
    # (its sealed logical outputs). Its component is read like a controller
    # one, so the observation is complete rather than permanently
    # `incomplete` (which left the runtime_health alarm domain unreconciled
    # and closed admission).
    core = _ComponentConditionCore(["arm", "station_io"])
    cclc = _FakeCCLC()
    provider = _component_snapshot("station_io", "none")
    provider["domains"] = [
        {
            "active_control_configuration": None,
            "control_domain_id": "domain.io",
            "control_domain_revision": "rev.io",
            "kind": "io_provider",
            "provider": {},
            "provider_runtime_epoch": "epoch.io",
            "resources": ["resource.do0"],
        }
    ]
    cclc.snapshots = {
        "arm": _component_snapshot("arm", "none"),
        "station_io": provider,
    }
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 1
    observed = core.observed[0]
    assert observed["unavailable_component_ids"] == []
    rows = json.loads(observed["snapshots_json"])
    assert [row["domains"] for row in rows] == [
        [{"control_domain_id": "domain.arm", "resources": ["resource.arm"]}],
        [{"control_domain_id": "domain.io", "resources": ["resource.do0"]}],
    ]
    assert core.observation_pending is False


def test_component_conditions_need_no_cclc_without_components() -> None:
    core = _ComponentConditionCore([])
    cclc = _FakeCCLC()
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    _condition_pump(core, None)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert cclc.calls == []
    assert core.observed == []
    assert core.observed_empty == []
    assert core.marked_unknown == 0


def test_component_conditions_reconcile_a_scope_that_became_empty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Regression: an Apply to a configuration without controlled components
    # removes the CCLC service. Observing the empty scope through CCLC found
    # the owner unavailable and marked the source unknown, so the removed
    # components' conditions stayed active. The empty configuration is now
    # observed by the core alone, fenced to its identity, without CCLC.
    clock = [100.0]
    monkeypatch.setattr(
        "manyforge_headless_api.execution.time.monotonic", lambda: clock[0]
    )
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.snapshots = {"arm": _component_snapshot("arm", "emergency_stop")}
    pump = _condition_pump(core, cclc)
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 1
    core.source_registered = True

    # Apply to the empty configuration: the CCLC owner is gone.
    core.component_ids = []
    core.configuration = ("bundle.empty", "graph.empty")
    cclc.epochs = [HeadlessApiError("cclc_owner_unavailable", "socket absent")]
    calls = list(cclc.calls)
    clock[0] += 5.0
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.observed_empty == [
        {"graph_revision": "graph.empty", "retained_bundle_id": "bundle.empty"}
    ]
    assert len(core.observed) == 1
    assert core.marked_unknown == 0
    assert cclc.calls == calls
    assert not pump._component_conditions_failing  # noqa: SLF001
    # An accepted observation waits the full period.
    clock[0] += 0.2
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed_empty) == 1

    # A configuration changed under the read (or no fresh publication)
    # completed nothing: the read repeats at the pump cadence.
    core.empty_status = "scope_changed"
    clock[0] += 5.0
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    core.empty_status = "configuration_unavailable"
    clock[0] += 0.11
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed_empty) == 3
    assert core.marked_unknown == 0
    assert cclc.calls == calls


def test_registered_source_with_components_and_unreachable_cclc_stays_unknown() -> None:
    # Only an empty configuration is observed without CCLC: with components in
    # scope an unreachable owner keeps the conditions at unknown quality.
    core = _ComponentConditionCore(["arm"])
    core.source_registered = True
    cclc = _FakeCCLC()
    cclc.epochs = [HeadlessApiError("cclc_owner_unavailable", "socket absent")]
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.marked_unknown == 1
    assert core.observed == []
    assert core.observed_empty == []


def test_component_condition_scope_without_registration_flag_is_refused() -> None:
    core = _ComponentConditionCore([])
    core.component_lifecycle_condition_scope = (  # type: ignore[method-assign]
        lambda: {"component_ids": []}
    )
    cclc = _FakeCCLC()
    pump = _condition_pump(core, cclc)
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert pump._component_conditions_failing  # noqa: SLF001
    assert cclc.calls == []
    assert core.observed == []
    assert core.marked_unknown == 0


def test_unreachable_cclc_keeps_component_conditions_at_unknown_quality() -> None:
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.epochs = [HeadlessApiError("cclc_owner_unavailable", "socket absent")]
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.marked_unknown == 1
    assert core.observed == []

    # Losing the owner in the middle of the read is the same communication loss.
    cclc = _FakeCCLC()
    cclc.snapshots = {"arm": HeadlessApiError("cclc_owner_unavailable", "lost")}
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.marked_unknown == 2
    assert core.observed == []


def test_component_conditions_reconcile_promptly_after_an_apply_replaced_cclc(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Regression: an Apply that replaces the CCLC owner makes it briefly
    # unreachable, and unknown quality holds the Alarm Lifecycle Owner
    # unreconciled (motion admission refused). The next read must follow at
    # the pump cadence, not one whole condition period after the owner is
    # back; only an accepted observation waits the full period.
    clock = [100.0]
    monkeypatch.setattr(
        "manyforge_headless_api.execution.time.monotonic", lambda: clock[0]
    )
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.epochs = [HeadlessApiError("cclc_owner_unavailable", "socket absent")]
    cclc.snapshots = {"arm": _component_snapshot("arm", "none")}
    pump = _condition_pump(core, cclc)
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.marked_unknown == 1

    cclc.epochs = ["cclc.epoch.2"]
    clock[0] += 0.05
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.observed == []
    clock[0] += 0.06
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert [value["cclc_owner_epoch"] for value in core.observed] == ["cclc.epoch.2"]

    clock[0] += 0.1
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 1

    # A scope that changed under the read completed nothing either.
    core.observe_component_lifecycle_conditions = (  # type: ignore[method-assign]
        lambda **kwargs: core.observed.append(kwargs) or {"status": "scope_changed"}
    )
    clock[0] += 5.0
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    clock[0] += 0.11
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert len(core.observed) == 3


def _clocked_condition_pump(
    monkeypatch: pytest.MonkeyPatch, core: _ComponentConditionCore, cclc: _FakeCCLC
) -> tuple[ExecutionEventPump, list[float]]:
    clock = [100.0]
    monkeypatch.setattr(
        "manyforge_headless_api.execution.time.monotonic", lambda: clock[0]
    )
    return (
        ExecutionEventPump(  # type: ignore[arg-type]
            core, _PagedExecution(), cclc=cclc, component_condition_poll_seconds=1.0
        ),
        clock,
    )


def _pump_iteration(pump: ExecutionEventPump) -> None:
    # The order of one pump iteration: refresh, then the lifecycle read.
    pump._refresh_alarm_sources()  # noqa: SLF001
    pump._observe_component_lifecycle_conditions()  # noqa: SLF001


def test_a_pending_lifecycle_observation_is_read_on_the_next_iteration(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Spec 530 §7.1: an Apply is respected without polling. The registry swap
    # marks the lifecycle source unknown and the core reports the pending
    # observation through the refresh the pump offers each iteration; the
    # next read does not wait out the period started under the old
    # configuration.
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.snapshots = {"arm": _component_snapshot("arm", "none")}
    pump, clock = _clocked_condition_pump(monkeypatch, core, cclc)
    _pump_iteration(pump)
    assert len(core.observed) == 1

    # A complete observation was accepted: the ordinary period applies.
    clock[0] += 0.3
    _pump_iteration(pump)
    assert len(core.observed) == 1

    # The registry swap (by any reader's refresh) sets the flag.
    core.observation_pending = True
    clock[0] += 0.05
    _pump_iteration(pump)
    assert len(core.observed) == 2
    # Accepted (the fake clears the flag like the core): the period applies
    # again from this read.
    clock[0] += 0.5
    _pump_iteration(pump)
    assert len(core.observed) == 2
    clock[0] += 0.51
    _pump_iteration(pump)
    assert len(core.observed) == 3


def test_a_pending_lifecycle_observation_still_respects_the_retry_floor(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.snapshots = {"arm": _component_snapshot("arm", "none")}
    pump, clock = _clocked_condition_pump(monkeypatch, core, cclc)
    core.observation_pending = True
    core.observe_status = "scope_changed"
    _pump_iteration(pump)
    assert len(core.observed) == 1
    clock[0] += 0.05
    _pump_iteration(pump)
    assert len(core.observed) == 1
    clock[0] += 0.06
    _pump_iteration(pump)
    assert len(core.observed) == 2


def test_an_incomplete_lifecycle_observation_is_retried_at_the_floor(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # An observation with an unread component leaves quality unknown, so it
    # is not the period's accepted observation: retried at 0.1 s until a
    # complete one is accepted, which then waits the full period.
    core = _ComponentConditionCore(["arm", "gripper"])
    cclc = _FakeCCLC()
    cclc.snapshots = {
        "arm": _component_snapshot("arm", "none"),
        "gripper": HeadlessApiError("component_lifecycle_snapshot_unavailable", "no row"),
    }
    pump, clock = _clocked_condition_pump(monkeypatch, core, cclc)
    _pump_iteration(pump)
    assert [value["unavailable_component_ids"] for value in core.observed] == [
        ["gripper"]
    ]
    clock[0] += 0.05
    _pump_iteration(pump)
    assert len(core.observed) == 1
    clock[0] += 0.06
    _pump_iteration(pump)
    assert len(core.observed) == 2

    cclc.snapshots["gripper"] = _component_snapshot("gripper", "none")
    clock[0] += 0.11
    _pump_iteration(pump)
    assert len(core.observed) == 3
    assert core.observed[-1]["unavailable_component_ids"] == []
    clock[0] += 0.11
    _pump_iteration(pump)
    clock[0] += 0.5
    _pump_iteration(pump)
    assert len(core.observed) == 3
    clock[0] += 0.4
    _pump_iteration(pump)
    assert len(core.observed) == 4


def test_component_conditions_skip_a_read_that_spans_two_cclc_owners() -> None:
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    cclc.epochs = ["cclc.epoch.1", "cclc.epoch.2"]
    cclc.snapshots = {"arm": _component_snapshot("arm", "none")}
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    assert core.observed == []
    assert core.marked_unknown == 0


def test_component_conditions_forward_an_open_incident_without_its_timing() -> None:
    core = _ComponentConditionCore(["arm"])
    cclc = _FakeCCLC()
    snapshot = _component_snapshot("arm", "provider_lost")
    snapshot["lifecycle_progress"]["phase"] = "blocked"
    snapshot["lifecycle_progress"]["incident"] = {
        "acknowledged": False,
        "cause_class": "provider_lost",
        "detected_at_ns": 1_759_900_000_000,
        "incident_id": "incident.arm.1",
        "summary": "Controller lost",
        "vendor_codes": ["exit=137"],
    }
    cclc.snapshots = {"arm": snapshot}
    _condition_pump(core, cclc)._observe_component_lifecycle_conditions()  # noqa: SLF001
    forwarded = json.loads(core.observed[0]["snapshots_json"])
    assert forwarded[0]["lifecycle_progress"]["incident"] == {
        "cause_class": "provider_lost",
        "incident_id": "incident.arm.1",
        "summary": "Controller lost",
        "vendor_codes": ["exit=137"],
    }


def _cclc_incident_event(sequence: int, incident_id: str = "incident.arm.1") -> dict[str, Any]:
    kind = "component_incident" if sequence % 2 else "component_incident_acknowledged"
    return {
        "event_kind": kind,
        "event_sequence": sequence,
        "incident_id": incident_id,
        "occurred_at_ns": 1_000 + sequence,
        "payload": {"event_kind": kind, "incident_id": incident_id},
    }


class _IncidentCore(_RecordingCore):
    def __init__(self) -> None:
        super().__init__()
        self.recorded: list[tuple[str, dict[str, Any]]] = []
        self.keys: set[tuple[str, str]] = set()
        self.reject_next = False

    def record_cclc_incident_event(
        self, *, cclc_owner_epoch: str, event_json: str
    ) -> dict[str, Any]:
        event = json.loads(event_json)
        if self.reject_next:
            self.reject_next = False
            return {"event_id": None, "status": "rejected"}
        key = (event["incident_id"], event["event_kind"])
        if key in self.keys:
            return {"event_id": None, "status": "duplicate"}
        self.keys.add(key)
        self.recorded.append((cclc_owner_epoch, event))
        return {"event_id": f"event-{len(self.recorded)}", "status": "recorded"}


class _IncidentCCLC(_FakeCCLC):
    def __init__(self, events: list[dict[str, Any]]) -> None:
        super().__init__()
        self.events = events
        self.reads: list[tuple[int, int]] = []

    def incident_events(self, after_sequence: int, limit: int) -> dict[str, Any]:
        self.reads.append((after_sequence, limit))
        page = [row for row in self.events if row["event_sequence"] > after_sequence][:limit]
        return {
            "events": page,
            "through_sequence": page[-1]["event_sequence"] if page else after_sequence,
        }


def test_cclc_incident_events_are_recorded_once_in_order_at_a_bounded_cadence() -> None:
    core = _IncidentCore()
    cclc = _IncidentCCLC([_cclc_incident_event(1), _cclc_incident_event(2)])
    pump = _condition_pump(core, cclc)
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert [event["event_sequence"] for _epoch, event in core.recorded] == [1, 2]
    assert {epoch for epoch, _event in core.recorded} == {"cclc.epoch.1"}
    assert core.recorded[0][1] == _cclc_incident_event(1)
    # Within the period nothing is read again; afterwards only newer events.
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads == [(0, 64)]
    pump._next_incident_event_poll_at = float("-inf")  # noqa: SLF001
    cclc.events.append(_cclc_incident_event(3, "incident.arm.2"))
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads[-1] == (2, 64)
    assert [event["event_sequence"] for _epoch, event in core.recorded] == [1, 2, 3]


def test_cclc_incident_events_survive_a_lost_cursor_without_duplicates() -> None:
    core = _IncidentCore()
    events = [_cclc_incident_event(1), _cclc_incident_event(2)]
    _condition_pump(core, _IncidentCCLC(events))._record_cclc_incident_events()  # noqa: SLF001
    # A restarted reader (fresh pump, cursor zero) replays: the core answers
    # duplicates and nothing is recorded twice.
    restarted = _IncidentCCLC(events)
    _condition_pump(core, restarted)._record_cclc_incident_events()  # noqa: SLF001
    assert restarted.reads == [(0, 64)]
    assert len(core.recorded) == 2


def test_cclc_incident_events_retry_a_rejected_event_and_follow_the_owner_epoch() -> None:
    core = _IncidentCore()
    cclc = _IncidentCCLC([_cclc_incident_event(1), _cclc_incident_event(2)])
    pump = _condition_pump(core, cclc)
    core.reject_next = True
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert core.recorded == []
    pump._next_incident_event_poll_at = float("-inf")  # noqa: SLF001
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads == [(0, 64), (0, 64)]
    assert len(core.recorded) == 2
    # A new CCLC owner restarts the cursor; recorded events stay deduplicated.
    cclc.epochs = ["cclc.epoch.2"]
    cclc.events = [_cclc_incident_event(1, "incident.arm.9")]
    pump._next_incident_event_poll_at = float("-inf")  # noqa: SLF001
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads[-1] == (0, 64)
    assert core.recorded[-1] == ("cclc.epoch.2", _cclc_incident_event(1, "incident.arm.9"))


def test_cclc_incident_events_continue_immediately_after_a_full_page() -> None:
    core = _IncidentCore()
    cclc = _IncidentCCLC(
        [_cclc_incident_event(index, f"incident.{index}") for index in range(1, 66)]
    )
    pump = _condition_pump(core, cclc)
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert len(core.recorded) == 64
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads == [(0, 64), (64, 64)]
    assert len(core.recorded) == 65


def test_cclc_incident_events_wait_quietly_for_an_unreachable_owner() -> None:
    core = _IncidentCore()
    cclc = _IncidentCCLC([_cclc_incident_event(1)])
    cclc.epochs = [HeadlessApiError("cclc_owner_unavailable", "socket absent")]
    pump = _condition_pump(core, cclc)
    pump._record_cclc_incident_events()  # noqa: SLF001
    assert cclc.reads == []
    assert core.recorded == []
    assert not pump._incident_events_failing  # noqa: SLF001


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
        "api_version": "manyforge.execution_command_response.v1",
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
            "api_version": "manyforge.execution_command_response.v1",
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
        "expected_latch_id",
        "expected_stop_certificate_id",
        "expected_stop_owner_generation",
        "reason",
    }


def test_recovery_assessment_reason_union_is_the_frozen_eighteen() -> None:
    assert len(RETRY_ELIGIBILITY_REASONS) == 18
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
        for _ in range(100):
            if execution.identities >= 2:
                break
            threading.Event().wait(0.01)
        assert execution.identities >= 2
        changed.clear()
        execution.ready = True
        assert changed.wait(timeout=1.0)
    finally:
        pump.shutdown()
    assert pump.last_error is None
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
        def __init__(self) -> None:
            super().__init__()
            self.owner_epoch = epoch

        def events(self, _owner_epoch: str, _after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
            raise failed.value

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


def test_execution_client_projects_global_rule_recovery_offers_and_variant(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    from manyforge_headless_api.models import GlobalIoRuleRecoveryIntentRequest

    client = ExecutionCommandClient(tmp_path)
    selector = {
        "context": {
            "owner_epoch": "kernel.epoch.1",
            "catalog_generation": "4",
            "active_publication_generation": 2,
            "bundle": "bundle.1",
            "graph": "graph.1",
        },
        "rule_id": "door_open",
        "expected_rule_registration_id": "9",
    }
    offer = {
        "selector": selector,
        "current_condition": None,
        "post_latch_disposition": "await_operator_ack",
        "latch_cause": "source_loss",
    }
    offers: list[Any] = [offer]
    commands: list[dict[str, Any]] = []
    result: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        commands.append(command)
        if command["command"] == "get_stop_fault":
            return {
                "api_version": "manyforge.execution_command_response.v1",
                "command": "get_stop_fault",
                "ok": True,
                "stop_fault": None,
                "global_rule_recovery_offers": offers,
            }
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "recover_stop_fault",
            "ok": True,
            "result": result,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    monkeypatch.setattr(client, "_exchange_control", exchange)
    assert client.stop_fault()["global_rule_recovery_offers"] == [offer]
    for patch in (
        {"current_condition": 0},
        {"latch_cause": "unknown"},
        {"post_latch_disposition": "auto_clear"},
        {"selector": {**selector, "expected_rule_registration_id": 9}},
        # SX-2216: Core emits only a positive registration nonce.
        {"selector": {**selector, "expected_rule_registration_id": "0"}},
        {"selector": {**selector, "context": {**selector["context"], "catalog_generation": "04"}}},
        {"extra": True},
    ):
        offers[:] = [{**offer, **patch}]
        with pytest.raises(HeadlessApiError):
            client.stop_fault()

    request = GlobalIoRuleRecoveryIntentRequest.model_validate(
        {
            "kind": "global_io_rule",
            "request_context": {
                "request_id": "request.rule-recovery.1",
                "request_owner_epoch": "execution.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
            },
            "selector": selector,
            "reason": None,
        }
    )
    result.update(
        request_id="request.rule-recovery.1",
        replay_status="accepted_new",
        accepted=True,
        context=selector["context"],
        rule={"rule_id": "door_open", "state": "clear", "triggering_acquisition": None},
        error=None,
    )
    assert client.recover_stop_fault(request)["result"]["accepted"] is True
    # The variant keeps its explicit null reason; Stop bytes are untouched.
    assert commands[-1]["request"] == request.model_dump(mode="json")
    assert commands[-1]["request"]["reason"] is None
    result["rule"] = {**result["rule"], "rule_id": "other"}
    with pytest.raises(HeadlessApiError):
        client.recover_stop_fault(request)
    result.update(accepted=False, rule=None, error="global_rule_condition_true")
    assert client.recover_stop_fault(request)["result"]["accepted"] is False
    # SX-2216: the result proves the exact requested selector context.
    result.update(
        accepted=True,
        rule={"rule_id": "door_open", "state": "clear", "triggering_acquisition": None},
        error=None,
    )
    for field, value in (
        ("owner_epoch", "kernel.epoch.2"),
        ("catalog_generation", "5"),
        ("active_publication_generation", 3),
        ("bundle", "bundle.2"),
        ("graph", "graph.2"),
    ):
        result["context"] = {**selector["context"], field: value}
        with pytest.raises(HeadlessApiError):
            client.recover_stop_fault(request)
    # A replay of a historical request keeps its original context even after
    # the current Applied context (the offers) moved on.
    offers[:] = [{**offer, "selector": {
        **selector, "context": {**selector["context"], "active_publication_generation": 3}}}]
    assert client.stop_fault()["global_rule_recovery_offers"] == offers
    result.update(context=selector["context"], replay_status="replayed_exact")
    assert client.recover_stop_fault(request)["result"]["replay_status"] == "replayed_exact"
    with pytest.raises(ValueError):
        GlobalIoRuleRecoveryIntentRequest.model_validate({
            **request.model_dump(mode="json"),
            "selector": {**selector, "expected_rule_registration_id": "0"},
        })


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
    execution.append(_rule_state_event(epoch, 3, "latched"))
    for _ in range(200):
        if published and published[-1] == [{"rule_id": "door", "state": "latched", "keys": []}]:
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
    assert published[-1] == [{"rule_id": "door", "state": "latched", "keys": []}]
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
