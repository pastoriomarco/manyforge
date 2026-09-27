from __future__ import annotations

import copy
import hashlib
import threading
from pathlib import Path
from typing import Any

import pytest
import rfc8785

from manyforge_headless_api.execution import (
    ExecutionCommandClient,
    ExecutionEventPump,
)
from manyforge_headless_api.models import (
    FullStopIntentRequest,
    OperationIntentRequest,
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
        "admission_generation": 1,
        "api_version": "manyforge.execution_command_response.v1",
        "command": "events",
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

    monkeypatch.setattr(client, "_exchange", exchange)
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

    monkeypatch.setattr(client, "_exchange", exchange)
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
                        "tcp_frame_role_id": "tcp",
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
        self.completed = threading.Event()
        self.unknown = threading.Event()

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        self.ingested.append(kwargs)
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **kwargs: Any) -> dict[str, Any]:
        self.completed_calls.append(kwargs)
        self.completed.set()
        return {"status": "complete"}

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
        raise AssertionError("idle owner must not be queried for events")

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
        if self.fail:
            self.retired = True
        page = super().events(owner_epoch, after_sequence, limit=limit)
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


def test_event_pump_records_progress_without_rebuilding_control_projection() -> None:
    core = _RecordingCore()
    execution = _StreamingExecution()
    wakes: list[None] = []
    execution.append(
        _progress_event(execution.owner_epoch, 1, state="executing", progress=0.1)
    )
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.set_change_observer(lambda: wakes.append(None))
    pump.start()
    for _ in range(100):
        if len(core.ingested) >= 1 and wakes:
            break
        threading.Event().wait(0.01)
    first_wake_count = len(wakes)

    execution.append(
        _progress_event(execution.owner_epoch, 2, state="executing", progress=0.8)
    )
    for _ in range(100):
        if len(core.ingested) >= 2:
            break
        threading.Event().wait(0.01)
    threading.Event().wait(0.03)

    assert len(core.ingested) == 2
    assert len(wakes) == first_wake_count == 1

    execution.append(
        _progress_event(execution.owner_epoch, 3, state="completed", progress=1.0)
    )
    for _ in range(100):
        if len(core.ingested) >= 3 and len(wakes) > first_wake_count:
            break
        threading.Event().wait(0.01)
    pump.shutdown()

    assert len(core.ingested) == 3
    assert len(wakes) == first_wake_count + 1


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


def test_event_pump_reconciles_typed_idle_owner_without_querying_events() -> None:
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
    assert execution.events_calls == 0
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
    assert execution.events_calls == 0
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
    assert execution.events_calls == 0
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
    assert execution.events_calls == 0


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
