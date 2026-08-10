from __future__ import annotations

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
from manyforge_headless_api.models import OperationIntentRequest
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


def _page(owner_epoch: str, events: list[dict[str, Any]]) -> dict[str, Any]:
    return {
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


def test_execution_client_validates_event_cursor_and_owner(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.1"
    response = _page(owner_epoch, [_event(owner_epoch, 1)])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
    assert page["next_after_sequence"] == 1

    response["event_page"]["next_after_sequence"] = 2
    with pytest.raises(HeadlessApiError) as invalid:
        client.events(owner_epoch, 0)
    assert invalid.value.code == "execution_command_response_invalid"

    response = _page(owner_epoch, [])
    response["event_page"]["has_more"] = True
    with pytest.raises(HeadlessApiError) as stalled:
        client.events(owner_epoch, 0)
    assert stalled.value.code == "execution_command_response_invalid"


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
        "caller_options": [
            {"option_id": "command_duration_s", "value_s": 0.5}
        ],
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
        "schema_version": (
            "manyforge.direct_control_capability_command_preview.v1"
        ),
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
    response = client.preview_capability_command(
        request, trusted_principal_id=None
    )

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
    assert local_response["preview_receipt"]["source_kind"] == (
        "component_local"
    )

    def stale_schema(command: dict[str, Any]) -> dict[str, Any]:
        response = _command_preview_response(command["operation"])
        response["preview_receipt"]["command_schema_revision"] = (
            "sha256:stale"
        )
        return response

    monkeypatch.setattr(client, "_exchange", stale_schema)
    with pytest.raises(HeadlessApiError) as mismatch:
        client.preview_capability_command(request, trusted_principal_id=None)
    assert mismatch.value.code == "execution_command_response_identity_mismatch"


class _RecordingCore:
    def __init__(self) -> None:
        self.ingested: list[dict[str, Any]] = []
        self.completed = threading.Event()
        self.unknown = threading.Event()

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        self.ingested.append(kwargs)
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **_kwargs: Any) -> dict[str, Any]:
        self.completed.set()
        return {"status": "complete"}

    def mark_operation_event_source_unknown(self, **_kwargs: Any) -> dict[str, Any]:
        self.unknown.set()
        return {"status": "marked_unknown"}


class _PagedExecution:
    def __init__(self) -> None:
        self.owner_epoch = "execution-host.epoch.pump"
        self.fail = False

    def status(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "status",
            "ok": True,
            "status": {"owner_epoch": self.owner_epoch},
        }

    def events(
        self, owner_epoch: str, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.fail:
            raise HeadlessApiError("execution_command_unavailable", "host lost")
        assert owner_epoch == self.owner_epoch
        assert limit == 1
        events = [] if after_sequence else [_event(owner_epoch, 1)]
        return {
            "events": events,
            "has_more": False,
            "next_after_sequence": after_sequence or 1,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
        }


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
