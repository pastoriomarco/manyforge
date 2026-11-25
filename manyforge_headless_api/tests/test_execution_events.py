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

from manyforge_headless_api.tests.paged_event_stream import FollowedEventSource
from manyforge_headless_api.execution import (
    ExecutionCommandClient,
    ExecutionEventPump,
)
from manyforge_headless_api.models import (
    FullStopIntentRequest,
    OperationIntentRequest,
    StopIntentRequest,
    RecoveryPreflightConfirmation,
    RuntimeScaleControlRequest,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.tests.fake_gateway import fake_client


def _unanswered(command: dict[str, Any]) -> dict[str, Any]:
    raise AssertionError(f"unexpected host call {command['command']}")


def _answer(client: ExecutionCommandClient, answer: Any) -> None:
    """The fake gateway's next answers."""

    client._gateway.answer = answer  # noqa: SLF001


def test_recover_enable_preflight_and_confirmation_forward_exact_native_values(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
    commands = []
    preflight = {"snapshot_id": "native-snapshot", "components": [
        {"component_id": "machine", "cause": {"code": "estop", "message": "E-stop",
         "vendor_code": "C2", "clearable": True, "operator_action": "recover", "concerns": ["arm"]},
         "stop": {"code": "estop", "message": "E-stop", "since_ns": 1,
                  "since_utc_ns": "1795122990477663500", "vendor_code": "C2"}, "eligibility": "recover"},
        {"component_id": "arm", "owner_id": "machine", "cause": None, "eligibility": "ready"},
    ]}
    def exchange(command):
        commands.append(command)
        if command["command"] == "component_recovery_preflight":
            return {"ok": True, "command": command["command"], "preflight": preflight}
        return {"ok": True, "command": "component_action", "accepted": {"action": "enable"}}
    _answer(client, exchange)
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
    client = fake_client(_unanswered)
    _answer(client, lambda command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native-snapshot", "components": [component]},
    })
    with pytest.raises(HeadlessApiError) as exc:
        client.component_recovery_preflight("arm")
    assert exc.value.code == "execution_command_response_invalid"


def test_recovery_preflight_keeps_simultaneous_fault_and_stop(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
    row = {"component_id": "arm", "eligibility": "recover", "cause": {
        "code": "communication", "message": "Fault", "clearable": True,
        "operator_action": "recover", "concerns": [],
    }, "stop": {"code": "estop", "message": "Stopped", "since_ns": 3, "vendor_code": "C2"}}
    _answer(client, lambda _command: {
        "ok": True, "command": "component_recovery_preflight",
        "preflight": {"snapshot_id": "native", "components": [row]},
    })
    assert client.component_recovery_preflight("arm")["preflight"]["components"] == [row]


def test_recovery_preflight_rejects_stop_cause_without_stop_evidence(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
    row = {"component_id": "arm", "eligibility": "recover", "cause": {
        "code": "estop", "message": "Stopped", "clearable": True,
        "operator_action": "recover", "concerns": [],
    }}
    _answer(client, lambda _command: {
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


def test_execution_client_uses_exact_focused_projection_commands(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
    commands: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        commands.append(command)
        return {
            "api_version": "manyforge.execution_command_response.v6",
            "command": command["command"],
            "ok": True,
        }

    _answer(client, exchange)
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


def test_execution_client_sends_runtime_scale_control_with_its_request(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
    captured: list[dict[str, Any]] = []

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.append(command)
        return {"ok": True, "command": "set_runtime_scale_control", "accepted": {}}

    # The core sends it beside Stop on the control socket.
    _answer(client, exchange)
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


def test_execution_client_projects_scoped_and_cell_wide_stop_commands(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
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

    _answer(client, control_exchange)
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


def test_execution_client_verifies_complete_motion_preview_receipt(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return _preview_response(command["operation"])

    _answer(client, exchange)
    response = client.preview_motion(request, trusted_principal_id=None)

    assert captured["command"] == "preview_motion"
    assert client._gateway.calls[-1][2] is None  # noqa: SLF001 - no principal
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

    _answer(client, malformed)
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

    _answer(client, undeclared)
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

    _answer(client, reordered_source)
    with pytest.raises(HeadlessApiError) as mismatched_source:
        client.preview_motion(request, trusted_principal_id=None)
    assert mismatched_source.value.code == "execution_command_response_invalid"

    def unsafe_collision_revision(command: dict[str, Any]) -> dict[str, Any]:
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["collision_world_revision"]["revision_id"] = 2**53
        return response

    _answer(client, unsafe_collision_revision)
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
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    latest = {
        "scene_id": "sha256:" + "9" * 64,
        "revision_id": 40,
        "content_hash": "sha256:" + "3" * 64,
    }

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        return _with_scene_revision(_preview_response(command["operation"]), latest)

    _answer(client, exchange)
    # The intent names no Scene revision; the owner plans on the latest one.
    response = client.preview_motion(request, trusted_principal_id=None)
    assert response["preview_receipt"]["scene_revision"] == latest


def test_capability_command_preview_receipt_names_no_scene_revision(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
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

    _answer(client, exchange)
    with pytest.raises(HeadlessApiError) as undeclared:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert undeclared.value.code == "execution_command_response_invalid"


def test_execution_client_verifies_capability_command_preview_receipt(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = fake_client(_unanswered)
    request = _direct_command_request()
    captured: dict[str, Any] = {}

    def exchange(command: dict[str, Any]) -> dict[str, Any]:
        captured.update(command)
        return _command_preview_response(command["operation"])

    _answer(client, exchange)
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

    _answer(client, wrong_target)
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

    _answer(client, component_local)
    local_response = client.preview_capability_command(
        request, trusted_principal_id=None
    )
    assert local_response["preview_receipt"]["source_kind"] == ("component_local")


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


class _PagedExecution(FollowedEventSource):
    # The pump reads the subscription only.

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


class _IdleExecution(FollowedEventSource):
    # The pump reads the subscription only.

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
    client = fake_client(_unanswered)
    commands = []
    rows = [{"component_id": "robot", "entry": None,
             "registration_generation": 1, "sequence": 0}]
    def exchange(command):
        commands.append(command)
        return {"ok": True, "command": "submit", "accepted": {}, "active_tcp": rows}
    _answer(client, exchange)
    assert client.submit(request, trusted_principal_id="operator.1")["active_tcp"] == rows
    assert commands[0]["operation"]["participants"][0]["payload"] == payload
    # The core forms the attribution from the principal.
    assert client._gateway.calls[0][2] == "operator.1"  # noqa: SLF001


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
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    for factory in (_preview_response, _v2_preview_response):
        _answer(client, lambda command: factory(command["operation"]))
        assert client.preview_motion(request, trusted_principal_id=None)["ok"] is True
    def tampered(command):
        response = _v2_preview_response(command["operation"])
        response["preview_receipt"]["path_preview"]["samples"][1]["positions_rad"] = [0.3]
        return response
    _answer(client, tampered)
    with pytest.raises(HeadlessApiError, match="digest"):
        client.preview_motion(request, trusted_principal_id=None)


def test_motion_preview_v4_tcp_snapshot_is_closed_and_digest_bound(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
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
        _answer(client, response_with(source))
        assert client.preview_motion(request, trusted_principal_id=None)["preview_receipt"]["resolved_tcp"]["source"] == source

    payload = request.model_dump(mode="json")
    payload["participants"][0]["payload"]["tcp_endpoint"] = {
        "kind": "entry",
        "target": {"component_id": "robot", "command_target_id": "arm"},
        "frame": {"component_id": "robot", "frame_id": "flange"},
    }
    _answer(client, response_with("explicit_entry"))
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
    _answer(client, tampered)
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
    client = fake_client(_unanswered)
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
    _answer(client, exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(_direct_motion_request(), trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_v4_rejects_fallback_for_pose(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
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
    _answer(client, exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_rejects_old_receipt_without_tcp_snapshot(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
    def exchange(command):
        response = _preview_response(command["operation"])
        receipt = response["preview_receipt"]
        receipt["schema_version"] = "manyforge.direct_control_motion_preview.v3"
        del receipt["resolved_tcp"]
        return response
    _answer(client, exchange)
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
    client = fake_client(_unanswered)
    def exchange(command):
        response = _v2_preview_response(command["operation"])
        mutation(response["preview_receipt"]["path_preview"])
        return response
    _answer(client, exchange)
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
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    error = {"code": "validation_collision", "message": "collision", "details": {}}
    response = {"api_version": "manyforge.execution_command_response.v6", "ok": False,
                "error": error, "diagnostic_preview": _bounded_diagnostic()}
    _answer(client, lambda _command: copy.deepcopy(response))
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
    client = fake_client(_unanswered)
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
    _answer(client, lambda _command: copy.deepcopy(failure))
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
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    diagnostic = _bounded_diagnostic()
    diagnostic["pairs"][0]["body_b"] = "scene/" + "a" * (48 * 1024) + "/shape/0"
    failure = {"api_version": "manyforge.execution_command_response.v6", "ok": False,
               "error": {"code": "validation_collision", "message": "collision", "details": {}},
               "diagnostic_preview": diagnostic}
    _answer(client, lambda _command: copy.deepcopy(failure))
    result = client.preview_motion(request, trusted_principal_id=None)
    assert result == {key: value for key, value in failure.items() if key != "diagnostic_preview"}


def test_motion_preview_rejects_unsupported_schema_and_oversized_v2(monkeypatch, tmp_path):
    client = fake_client(_unanswered)
    request = _direct_motion_request()
    def exchange(command):
        response = _v2_preview_response(command["operation"])
        response["preview_receipt"]["path_preview"]["pairs"][0]["body_b"] = "scene/" + "a" * (48 * 1024) + "/shape/0"
        return response
    _answer(client, exchange)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"
    def unknown(command):
        response = _preview_response(command["operation"])
        response["preview_receipt"]["schema_version"] = "manyforge.direct_control_motion_preview.v2"
        return response
    _answer(client, unknown)
    with pytest.raises(HeadlessApiError) as error:
        client.preview_motion(request, trusted_principal_id=None)
    assert error.value.code == "execution_command_response_invalid"


def test_motion_preview_accepts_unmapped_raw_body_identity():
    from manyforge_headless_api.execution import _motion_path_preview_valid
    path = _bounded_motion_path()
    path["pairs"][0]["body_a"] = "custom raw body identity"
    path["samples"][1]["contacts"][0]["disposition"] = "unknown"
    assert _motion_path_preview_valid(path)


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
    pump._observe_runtime_fault_snapshot(faults)
    assert pump.journal_disk_signals == (True, False)
    fault["queue_full"] = False
    fault["error"]["code"] = "operation_journal_storage_failure"
    pump._observe_runtime_fault_snapshot(faults)
    assert pump.journal_disk_signals == (False, True)
    faults["operation_journal_write_fault"] = None
    pump._observe_runtime_fault_snapshot(faults)
    assert pump.journal_disk_signals == (False, False)


def test_a_core_failure_is_the_same_typed_error_and_one_warning(caplog) -> None:
    from manyforge_headless_api.tests.fake_gateway import GatewayError

    def lost(_command: dict[str, Any]) -> dict[str, Any]:
        raise GatewayError(
            "execution_owner_unavailable", "native execution transport failed",
            {"peer_socket": "execution-control.sock"}, "headless_api",
        )

    client = fake_client(lost)
    with caplog.at_level(logging.WARNING, logger="manyforge_headless_api.execution"):
        with pytest.raises(HeadlessApiError) as failed:
            client.halt(
                FullStopIntentRequest.model_validate(
                    {"request_context": {"request_key": _HEADLESS_KEY, "source_label": "disk"}}
                ),
                trusted_principal_id=None,
            )
    assert failed.value.code == "execution_owner_unavailable"
    assert str(failed.value) == "native execution transport failed"
    assert failed.value.details == {"peer_socket": "execution-control.sock"}
    assert failed.value.producer == "headless_api"
    warnings = [record for record in caplog.records if record.levelno == logging.WARNING]
    assert len(warnings) == 1
    assert "command=halt code=execution_owner_unavailable" in warnings[0].getMessage()
