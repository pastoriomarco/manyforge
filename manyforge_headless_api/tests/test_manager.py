from __future__ import annotations

import json
from pathlib import Path
import socket
import threading
import time

import pytest
import rfc8785

from manyforge_headless_api.manager import ManagerCommandClient, MANAGER_RESPONSE_VERSION
from manyforge_headless_api.models import (
    ManagerApplyRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
)
from manyforge_headless_api.native import HeadlessApiError


def test_manager_transport_accepts_bounded_cold_apply_timeout(tmp_path: Path) -> None:
    ManagerCommandClient(tmp_path, timeout_seconds=12.0)
    with pytest.raises(ValueError, match=r"\(0, 60\]"):
        ManagerCommandClient(tmp_path, timeout_seconds=60.1)


def _prepared_activation() -> dict[str, object]:
    return {
        "schema_version": "manyforge.prepared_activation_input.v6",
        "origin": {"kind": "scenario", "scenario_id": "scenario.1",
                   "source_document_digest": "sha256:" + "1" * 64},
        "captured_input_digest": "sha256:" + "d" * 64,
        "deployment_artifact_id": "deployment.1",
        "deployment_revision": "sha256:" + "a" * 64,
        "retained_bundle_id": "sha256:" + "2" * 64,
        "graph_revision": "sha256:" + "3" * 64,
        "program_id": "program.1",
        "program_revision": "sha256:" + "4" * 64,
        "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
        "node_environment_digest": "sha256:" + "5" * 64,
        "assistant_mode": "optional",
        "prepared_activation_id": "sha256:" + "e" * 64,
    }


def _request() -> ManagerApplyRequest:
    return ManagerApplyRequest.model_validate(
        {
            "request_id": "request.1",
            "request_owner_epoch": "client.epoch.1",
            "source_label": "test",
            "correlation_id": "correlation.1",
            "deployment_artifact_id": "deployment.1",
            "deployment_revision": "sha256:" + "a" * 64,
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "reject_if_affected_busy": True,
            "prepared_activation": _prepared_activation(),
            "expected_admission_generation": 7,
            "candidate_receipt": {
                "schema_version": "manyforge.manager_deployment_candidate_receipt.v2",
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "a" * 64,
                "prepared_activation_id": "sha256:" + "e" * 64,
                "candidate_retained_bundle_id": "bundle.2",
                "candidate_graph_revision": "graph.2",
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "expected_admission_generation": 7,
                "manager_owner_epoch": "client.epoch.1",
                "impact_digest": "sha256:" + "c" * 64,
                "destructive_impact": False,
                "receipt_digest": "sha256:" + "c" * 64,
            },
        }
    )


def _scene_request() -> SceneTransactionRequest:
    scene_id = "sha256:" + "d" * 64
    return SceneTransactionRequest.model_validate(
        {
            "request_context": {
                "request_id": "scene-request.1",
                "request_owner_epoch": scene_id,
                "source_label": "behavior",
                "correlation_id": None,
            },
            "expected_projection_revision": {
                "retained_bundle_id": "sha256:" + "b" * 64,
                "graph_revision": "sha256:" + "c" * 64,
                "scene_revision": {
                    "scene_id": scene_id,
                    "revision_id": 1,
                    "content_hash": "sha256:" + "a" * 64,
                },
            },
            "expected_admission_generation": 7,
            "operation": {
                "kind": "remove_object",
                "scene_resource_id": "part.1",
            },
        }
    )


def _pose_query() -> SceneResourcePoseQuery:
    return SceneResourcePoseQuery.model_validate(
        {
            "api_version": "manyforge.scene_resource_pose_query.v1",
            "expected_projection_revision": {
                "retained_bundle_id": "sha256:" + "b" * 64,
                "graph_revision": "sha256:" + "c" * 64,
                "scene_revision": {
                    "scene_id": "sha256:" + "d" * 64,
                    "revision_id": 1,
                    "content_hash": "sha256:" + "a" * 64,
                },
            },
            "scene_resource_ids": ["part.1"],
        }
    )


def test_manager_client_uses_one_canonical_bounded_seqpacket(tmp_path: Path) -> None:
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    socket_path.chmod(0o600)
    server.listen(1)
    received: list[bytes] = []
    finished = threading.Event()

    def serve() -> None:
        connection, _address = server.accept()
        with connection:
            received.append(connection.recv(16 * 1024))
            connection.send(
                b'{"api_version":"manyforge.manager_command_response.v1",'
                b'"error":{"code":"kernel_owner_unavailable",'
                b'"message":"returns at I6"},"ok":false,'
                b'"request_id":"request.1",'
                b'"request_owner_epoch":"client.epoch.1"}'
            )
        finished.set()

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        response = ManagerCommandClient(tmp_path).apply(_request())
        assert response["ok"] is False
        assert finished.wait(1.0)
    finally:
        server.close()
        worker.join(timeout=1.0)
    assert not worker.is_alive()
    decoded = json.loads(received[0])
    assert received[0] == json.dumps(
        decoded,
        allow_nan=False,
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
    ).encode()
    assert decoded["command"] == "apply"
    assert set(decoded) == {
        "accepted_impact_digest",
        "api_version",
        "command",
        "candidate_receipt",
        "correlation_id",
        "deployment_artifact_id",
        "deployment_revision",
        "admission_context",
        "expected_current",
        "prepared_activation",
        "reject_if_affected_busy",
        "request_id",
        "request_owner_epoch",
        "source_label",
    }
    assert decoded["source_label"] == "test"
    assert decoded["correlation_id"] == "correlation.1"
    assert decoded["prepared_activation"] == _prepared_activation()
    assert decoded["admission_context"] == {
        "kind": "direct",
        "expected_admission_generation": 7,
    }


def test_manager_client_default_wait_covers_a_bounded_durable_mutation(
    tmp_path: Path,
) -> None:
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    socket_path.chmod(0o600)
    server.listen(1)

    def serve() -> None:
        connection, _address = server.accept()
        with connection:
            connection.recv(16 * 1024)
            # This exceeds the former 250 ms health-probe-sized default. A
            # durable mutation may already have committed when its reply is
            # sent, so the production client must wait for the bounded result.
            time.sleep(0.3)
            connection.send(
                b'{"api_version":"manyforge.manager_command_response.v1",'
                b'"error":null,"ok":true,"request_id":"request.1",'
                b'"request_owner_epoch":"client.epoch.1"}'
            )

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        response = ManagerCommandClient(tmp_path).apply(_request())
        assert response["ok"] is True
    finally:
        server.close()
        worker.join(timeout=1.0)
    assert not worker.is_alive()


def test_manager_client_rejects_a_response_for_another_request(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(
        client,
        "_exchange",
        lambda _command: {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": False,
            "request_id": "request.other",
            "request_owner_epoch": "client.epoch.1",
        },
    )

    with pytest.raises(
        HeadlessApiError,
        match="does not match the submitted request",
    ) as captured:
        client.apply(_request())

    assert captured.value.code == "manager_command_response_identity_mismatch"


def test_manager_client_observes_apply_without_replaying_submission(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    commands: list[dict[str, object]] = []

    def exchange(command: dict[str, object]) -> dict[str, object]:
        commands.append(command)
        return _status_response()

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply("apply.3", 3)

    assert response["state"] == "pending"
    assert response["phase"] == "accepted"
    # Observation is one read of the durable identity: it submits nothing, so a
    # reconnecting client can never repeat teardown, startup or publication.
    assert commands == [{
        "api_version": "manyforge.manager_command.v1",
        "apply_generation": 3,
        "apply_id": "apply.3",
        "command": "apply_status",
    }]


def _status_response() -> dict[str, object]:
    return {
        "api_version": "manyforge.manager_command_response.v1",
        "apply_generation": 3,
        "apply_id": "apply.3",
        "candidate_projection_revision": None,
        "error": None,
        "failure": None,
        "last_progress": {
            "phase": "accepted",
            "record_digest": "sha256:" + "f" * 64,
        },
        "ok": True,
        "phase": "accepted",
        "prepared_activation": {
            **{
                field: _prepared_activation()[field]
                for field in (
                    "captured_input_digest",
                    "deployment_artifact_id",
                    "deployment_revision",
                    "prepared_activation_id",
                )
            },
            "program_binding_id": _prepared_activation()["program_outcome"][
                "program_binding_id"
            ],
        },
        "request_id": "request.3",
        "request_owner_epoch": "client.epoch.3",
        "state": "pending",
    }


def test_manager_client_keeps_one_bounded_apply_status_contract(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        response = _status_response()
        response["prepared_activation"] = {
            **_prepared_activation(),
            "unexpected_member": "value",
        }
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as captured:
        client.observe_apply("apply.3", 3)
    assert captured.value.code == "manager_command_response_invalid"


def test_manager_client_rejects_a_status_that_is_not_one_lifecycle(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        response = _status_response()
        # A status may not claim progress the reported phase does not describe.
        response["last_progress"] = {
            "phase": "published",
            "record_digest": "sha256:" + "f" * 64,
        }
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as captured:
        client.observe_apply("apply.3", 3)
    assert captured.value.code == "manager_command_response_invalid"


def test_manager_client_rejects_a_status_for_another_durable_operation(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        response = _status_response()
        response["apply_id"] = "apply.4"
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as captured:
        client.observe_apply("apply.3", 3)
    assert captured.value.code == "manager_command_response_identity_mismatch"


def test_manager_client_reports_an_unknown_operation_status_as_a_typed_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "error": {
                "code": "apply_status_not_found",
                "message": "Manager Apply identity is unknown",
            },
            "ok": False,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply("apply.3", 3)
    assert response["ok"] is False
    assert response["error"]["code"] == "apply_status_not_found"


def test_manager_client_keeps_a_terminal_success_replay_exact(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    projection = {
        "retained_bundle_id": "sha256:" + "b" * 64,
        "graph_revision": "sha256:" + "c" * 64,
        "scene_revision": {
            "scene_id": "sha256:" + "1" * 64,
            "revision_id": 4,
            "content_hash": "sha256:" + "2" * 64,
        },
    }

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        return {
            **_status_response(),
            "candidate_projection_revision": projection,
            "last_progress": {
                "phase": "settled",
                "record_digest": "sha256:" + "3" * 64,
            },
            "phase": "settled",
            "state": "succeeded",
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply("apply.3", 3)
    assert response["state"] == "succeeded"
    assert response["candidate_projection_revision"] == projection


def test_manager_client_keeps_the_actual_terminal_failure_phase(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        return {
            **_status_response(),
            "state": "failed",
            "failure": {
                "code": "planned_maintenance_refused",
                "message": "the cell is not idle",
                "phase": "accepted",
                "effect_disposition": "known",
            },
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply("apply.3", 3)
    assert response["phase"] == "accepted"
    assert response["failure"]["code"] == "planned_maintenance_refused"


def test_scene_client_stamps_attribution_and_uses_one_canonical_packet(
    tmp_path: Path,
) -> None:
    scene_id = "sha256:" + "d" * 64
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    socket_path.chmod(0o600)
    server.listen(1)
    received: list[bytes] = []

    def serve() -> None:
        connection, _address = server.accept()
        with connection:
            received.append(connection.recv(132 * 1024))
            connection.send(
                json.dumps(
                    {
                        "api_version": "manyforge.manager_command_response.v1",
                        "committed_projection_revision": {
                            "retained_bundle_id": "sha256:" + "b" * 64,
                            "graph_revision": "sha256:" + "c" * 64,
                            "scene_revision": {
                                "scene_id": scene_id,
                                "revision_id": 2,
                                "content_hash": "sha256:" + "b" * 64,
                            },
                        },
                        "error": None,
                        "ok": True,
                        "projection_status": "committed",
                        "replay_status": "accepted_new",
                        "request_id": "scene-request.1",
                        "request_owner_epoch": scene_id,
                        "transaction_sequence": 1,
                        "update_status": "geometry_changed",
                    },
                    allow_nan=False,
                    ensure_ascii=True,
                    separators=(",", ":"),
                    sort_keys=True,
                ).encode()
            )

    worker = threading.Thread(target=serve)
    worker.start()
    request = _scene_request()
    request = SceneTransactionRequest.model_validate(
        {
            **request.model_dump(mode="json"),
            "operation": {
                "kind": "add_object",
                "scene_resource_id": "part.1",
                "shape": {
                    "type": "sphere",
                    "sphere_radius_m": 0.01,
                },
                "pose": {
                    "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.01},
                    "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
                },
                "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
                "support_contacts": [
                    {"support_object_id": "table.1"},
                ],
            },
        }
    )
    try:
        response = ManagerCommandClient(tmp_path).commit_scene_transaction(
            request, trusted_principal_id="operator.7"
        )
        assert response["projection_status"] == "committed"
    finally:
        server.close()
        worker.join(timeout=1.0)
    assert not worker.is_alive()
    command = json.loads(received[0])
    assert received[0] == rfc8785.dumps(command)
    assert b'"release_tolerance_m":0.00001' in received[0]
    assert set(command) == {"api_version", "command", "transaction"}
    assert command["command"] == "scene_commit"
    assert command["transaction"]["attribution"] == {
        "assurance": "externally_authenticated",
        "principal_id": "operator.7",
        "transport": "http",
    }
    assert command["transaction"]["operation"]["support_contacts"] == [
        {
            "allow_recontact": False,
            "enforce_max_start_penetration": True,
            "max_contact_penetration_m": 0.0,
            "max_start_penetration_m": 0.0,
            "min_release_progress_m": 0.0,
            "mode": "contact_release",
            "release_evidence_mode": "signed_penetration",
            "release_tolerance_m": 1e-5,
            "require_separation_by_end": True,
            "require_strictly_decreasing_penetration": True,
            "support_object_id": "table.1",
        }
    ]


def test_scene_client_rejects_mismatched_or_inconsistent_owner_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    valid_failure = {
        "api_version": "manyforge.manager_command_response.v1",
        "committed_projection_revision": None,
        "error": {"code": "object_not_found", "details": {}, "message": "absent"},
        "ok": False,
        "projection_status": "not_attempted",
        "replay_status": "accepted_new",
        "request_id": "other-request",
        "request_owner_epoch": "sha256:" + "d" * 64,
        "transaction_sequence": None,
        "update_status": None,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: valid_failure)
    with pytest.raises(HeadlessApiError) as captured:
        client.commit_scene_transaction(
            _scene_request(), trusted_principal_id=None
        )
    assert captured.value.code == "manager_command_response_identity_mismatch"

    valid_failure["request_id"] = "scene-request.1"
    valid_failure["projection_status"] = "committed"
    with pytest.raises(HeadlessApiError) as captured:
        client.commit_scene_transaction(
            _scene_request(), trusted_principal_id=None
        )
    assert captured.value.code == "manager_command_response_invalid"

    scene_id = "sha256:" + "d" * 64
    mismatched_success = {
        "api_version": "manyforge.manager_command_response.v1",
        "committed_projection_revision": {
            "retained_bundle_id": "sha256:" + "e" * 64,
            "graph_revision": "sha256:" + "c" * 64,
            "scene_revision": {
                "scene_id": scene_id,
                "revision_id": 2,
                "content_hash": "sha256:" + "f" * 64,
            },
        },
        "error": None,
        "ok": True,
        "projection_status": "committed",
        "replay_status": "accepted_new",
        "request_id": "scene-request.1",
        "request_owner_epoch": scene_id,
        "transaction_sequence": 1,
        "update_status": "geometry_changed",
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: mismatched_success)
    with pytest.raises(HeadlessApiError) as captured:
        client.commit_scene_transaction(
            _scene_request(), trusted_principal_id=None
        )
    assert captured.value.code == "manager_command_response_identity_mismatch"


def test_scene_result_lookup_is_read_only_and_validates_the_nested_owner_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    request = _scene_request()
    commands = []
    response = {"api_version": "manyforge.manager_command_response.v1", "ok": True,
                "result": None}

    def exchange(command):
        commands.append(command)
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.scene_transaction_status(request)["result"] is None
    assert commands[0]["command"] == "scene_transaction_status"
    assert "program_node_evidence" not in commands[0]["transaction"]
    response["result"] = {
        "api_version": "manyforge.manager_command_response.v1",
        "committed_projection_revision": None,
        "error": {"code": "object_not_found", "details": {}, "message": "absent"},
        "ok": False, "projection_status": "not_attempted", "replay_status": "replayed_exact",
        "request_id": request.request_context.request_id,
        "request_owner_epoch": request.request_context.request_owner_epoch,
        "transaction_sequence": None, "update_status": None,
    }
    assert client.scene_transaction_status(request)["result"]["ok"] is False
    response["result"]["request_id"] = "another-request"
    with pytest.raises(HeadlessApiError, match="does not match"):
        client.scene_transaction_status(request)
    for malformed in ([], 42, {"ok": True}):
        response["result"] = malformed
        with pytest.raises(HeadlessApiError) as captured:
            client.scene_transaction_status(request)
        assert captured.value.code == "manager_command_response_invalid"
    assert all(command["command"] == "scene_transaction_status" for command in commands)


def test_scene_pose_client_fences_and_validates_the_owner_projection(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client = ManagerCommandClient(tmp_path)
    query = _pose_query()
    commands: list[dict[str, object]] = []

    def exchange(command: dict[str, object]) -> dict[str, object]:
        commands.append(command)
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": True,
            "poses": [
                {
                    "scene_resource_id": "part.1",
                    "pose_in_cell": {
                        "position_m": {"x": 0.1, "y": 0.2, "z": 0.3},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ],
            "projection_revision": (
                query.expected_projection_revision.model_dump()
            ),
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.resolve_scene_resources(query)

    assert response["api_version"] == "manyforge.scene_resource_pose_response.v1"
    assert response["poses"][0]["scene_resource_id"] == "part.1"
    assert commands == [
        {
            "api_version": "manyforge.manager_command.v1",
            "command": "scene_pose_query",
            "expected_projection_revision": (
                query.expected_projection_revision.model_dump()
            ),
            "scene_resource_ids": ["part.1"],
        }
    ]

    corrupt = exchange({})
    corrupt["projection_revision"] = {
        **query.expected_projection_revision.model_dump(),
        "graph_revision": "sha256:" + "e" * 64,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: corrupt)
    with pytest.raises(HeadlessApiError) as captured:
        client.resolve_scene_resources(query)
    assert captured.value.code == "manager_command_response_identity_mismatch"

    rejected = {
        "api_version": "manyforge.manager_command_response.v1",
        "error": {
            "code": "missing_reference",
            "details": {"reason": "scene_frame_resolution_ambiguous"},
            "message": "active native projections disagree",
        },
        "ok": False,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: rejected)
    with pytest.raises(HeadlessApiError) as rejected_error:
        client.resolve_scene_resources(query)
    assert rejected_error.value.code == "missing_reference"
    assert rejected_error.value.details == {
        "reason": "scene_frame_resolution_ambiguous"
    }


def _process_stop_request():
    from manyforge_headless_api.models import ComponentProcessStopRequest
    return ComponentProcessStopRequest.model_validate({
        "request_context": {"request_id": "stop.1", "request_owner_epoch": "owner.1",
                            "source_label": "composer", "correlation_id": "close.1"},
        "predecessor_projection_revision": {
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "b" * 64,
            "scene_revision": {"scene_id": "sha256:" + "c" * 64,
                               "revision_id": 1, "content_hash": "sha256:" + "d" * 64}},
        "expected_predecessor_publication_generation": 4,
        "expected_predecessor_public_pointer_digest": "sha256:" + "e" * 64,
        "runtime_unit_id": "host.1", "runtime_epoch": "host.epoch.1",
        "manager_runtime_epoch": "manager.epoch.1", "controllable_component_id": "arm",
        "launch_identity": "launch.1", "leader_pid": 42,
    })


def _process_stop_response(request, state="drained"):
    return {"api_version": "manyforge.manager_command_response.v1", "ok": True, "error": None,
            "result": {"request": request.model_dump(mode="json"), "state": state,
                       "receipt": {"leader_pid": 42, "controllable_component_id": "arm",
                                   "launch_identity": "launch.1", "observed_utc": "2026-10-13T10:00:00Z"}
                                  if state == "drained" else None, "error": None}}


@pytest.mark.parametrize("method,command", [
    ("stop_component_process", "stop_component_process"),
    ("component_process_stop_result", "component_process_stop_result"),
])
def test_process_stop_forwards_exact_request_and_reads_without_resending(tmp_path, monkeypatch, method, command):
    request = _process_stop_request()
    client = ManagerCommandClient(tmp_path)
    observed = []
    response = _process_stop_response(request)
    def exchange(value):
        observed.append(value)
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    assert getattr(client, method)(request) == response["result"]
    assert observed == [{"api_version": "manyforge.manager_command.v1", "command": command,
                         "request": request.model_dump(mode="json")}]


@pytest.mark.parametrize("launch_identity", ['mock\nlite6_robot\nlite6_lite_gripper\n/manyforge/lite6_robot\nlite6_traj_controller\nlite_gripper_controller\n\nsha256:948e121935a4e7d66ca927d1945438e48332cce2bd820f60488fc049bb18d4b1\nsha256:61f01507a85d3a8d6539b8488d9a525dbaf115dca33338dc0d1702168d3b34a4\n', 'mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n'])
@pytest.mark.parametrize("method", ["stop_component_process", "component_process_stop_result"])
def test_process_stop_preserves_actual_multiline_mock_launch_identity(tmp_path, monkeypatch, launch_identity, method):
    from manyforge_headless_api.models import ComponentProcessStopRequest
    body = _process_stop_request().model_dump(mode="json")
    body["launch_identity"] = launch_identity
    body["controllable_component_id"] = launch_identity.split("\n")[1]
    request = ComponentProcessStopRequest.model_validate(body)
    assert len(launch_identity.encode("utf-8")) in (250, 257)
    response = _process_stop_response(request)
    response["result"]["receipt"]["launch_identity"] = launch_identity
    response["result"]["receipt"]["controllable_component_id"] = request.controllable_component_id
    client = ManagerCommandClient(tmp_path)
    calls = []
    def exchange(value):
        calls.append(value)
        return response
    monkeypatch.setattr(client, "_exchange", exchange)
    assert getattr(client, method)(request) == response["result"]
    assert calls[0]["request"]["launch_identity"] == launch_identity
    response["result"]["receipt"]["launch_identity"] += "foreign"
    with pytest.raises(HeadlessApiError, match="result is invalid"):
        getattr(client, method)(request)


def test_process_stop_launch_ceiling_preserves_receipt_bounds_and_nfc():
    from pydantic import ValidationError
    from manyforge_headless_api.models import ComponentProcessStopRequest, ComponentProcessDrainReceipt
    request = _process_stop_request().model_dump(mode="json")
    receipt = _process_stop_response(_process_stop_request())["result"]["receipt"]
    for model, body in [(ComponentProcessStopRequest, request), (ComponentProcessDrainReceipt, receipt)]:
        body["launch_identity"] = "x" * (256 * 1024)
        assert model.model_validate(body).launch_identity == body["launch_identity"]
        for value in ("", "x" * (256 * 1024 + 1), "e\u0301"):
            body["launch_identity"] = value
            with pytest.raises(ValidationError): model.model_validate(body)
        body["launch_identity"] = "valid"
        body["controllable_component_id"] = "x" * 129
        with pytest.raises(ValidationError): model.model_validate(body)
    receipt["controllable_component_id"] = "arm"
    receipt["observed_utc"] = "x" * 129
    with pytest.raises(ValidationError): ComponentProcessDrainReceipt.model_validate(receipt)


@pytest.mark.parametrize("field,value", [
    ("runtime_epoch", "foreign.epoch"), ("manager_runtime_epoch", "foreign.manager"),
    ("launch_identity", "foreign.launch"), ("leader_pid", 43),
    ("expected_predecessor_publication_generation", 5),
])
def test_process_stop_rejects_foreign_echo_without_assuming_drain(tmp_path, monkeypatch, field, value):
    request = _process_stop_request()
    response = _process_stop_response(request, "pending")
    response["result"]["request"][field] = value
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError, match="exact launch request"):
        client.component_process_stop_result(request)


@pytest.mark.parametrize("mutation", ["no_receipt", "wrong_launch", "wrong_pid", "pending_receipt", "missing_error"])
def test_process_stop_requires_exact_positive_drain_receipt(tmp_path, monkeypatch, mutation):
    request = _process_stop_request()
    response = _process_stop_response(request)
    result = response["result"]
    if mutation == "no_receipt": result["receipt"] = None
    elif mutation == "wrong_launch": result["receipt"]["launch_identity"] = "foreign"
    elif mutation == "wrong_pid": result["receipt"]["leader_pid"] = 43
    elif mutation == "pending_receipt": result["state"] = "pending"
    elif mutation == "missing_error": del result["error"]
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError, match="result is invalid"):
        client.component_process_stop_result(request)


@pytest.mark.parametrize("with_result", [False, True])
def test_process_stop_preserves_existing_manager_refusal_envelopes(tmp_path, monkeypatch, with_result):
    request = _process_stop_request()
    response = {"api_version": "manyforge.manager_command_response.v1", "ok": False,
                "error": {"code": "stale_generation", "message": "publication changed", "details": {"expected": "4"}}}
    if with_result: response["result"] = None
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError) as caught:
        client.stop_component_process(request)
    assert caught.value.code == "stale_generation"
    assert caught.value.details == {"expected": "4"}


@pytest.mark.parametrize("field,value", [("leader_pid", True), ("leader_pid", 0),
    ("leader_pid", 2_147_483_648), ("launch_identity", "x" * (256 * 1024 + 1)),
    ("controllable_component_id", "x" * 129), ("launch_identity", ""),
    ("expected_predecessor_publication_generation", 0)])
def test_process_stop_request_preserves_existing_bounds(field, value):
    from pydantic import ValidationError
    from manyforge_headless_api.models import ComponentProcessStopBody
    body = {"request": _process_stop_request().model_dump(mode="json")}
    body["request"][field] = value
    with pytest.raises(ValidationError): ComponentProcessStopBody.model_validate(body)



def test_process_stop_preserves_legacy_two_field_parse_error(tmp_path, monkeypatch):
    request = _process_stop_request()
    client = ManagerCommandClient(tmp_path)
    response = {"api_version": "manyforge.manager_command_response.v1", "ok": False,
                "error": {"code": "manager_command_rejected", "message": "request is malformed"}}
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError) as caught: client.stop_component_process(request)
    assert caught.value.code == "manager_command_rejected" and caught.value.details == {}


def test_process_stop_keeps_existing_error_grammar_without_readiness_caps(tmp_path, monkeypatch):
    request = _process_stop_request()
    response = _process_stop_response(request, "unknown")
    response["result"]["error"] = {"code": "cleanup_unknown", "message": "x" * 1800,
        "details": {str(index): "y" * 300 for index in range(70)}}
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    assert client.component_process_stop_result(request) == response["result"]


@pytest.mark.parametrize("mutation", ["unknown_state", "drained_error", "extra_result", "extra_receipt", "extra_error", "success_error", "missing_outer_error"])
def test_process_stop_rejects_malformed_or_contradictory_results(tmp_path, monkeypatch, mutation):
    request = _process_stop_request(); response = _process_stop_response(request)
    error = {"code": "unknown", "message": "not established", "details": {}}
    if mutation == "unknown_state": response["result"]["state"] = "stopped"
    if mutation == "drained_error": response["result"]["error"] = error
    if mutation == "extra_result": response["result"]["confirmed"] = True
    if mutation == "extra_receipt": response["result"]["receipt"]["runtime_epoch"] = "other"
    if mutation == "extra_error":
        response["result"].update(state="unknown", receipt=None, error={**error, "effect": "none"})
    if mutation == "success_error": response["error"] = error
    if mutation == "missing_outer_error":
        response = {"api_version": MANAGER_RESPONSE_VERSION, "ok": False}
    client = ManagerCommandClient(tmp_path); monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError) as caught: client.component_process_stop_result(request)
    assert caught.value.code == "manager_command_response_invalid"


@pytest.mark.parametrize("correlation", [None, "absent"])
@pytest.mark.parametrize("method", ["stop_component_process", "component_process_stop_result"])
def test_process_stop_omits_no_correlation_at_manager_boundary(tmp_path, monkeypatch, correlation, method):
    from manyforge_headless_api.models import ComponentProcessStopRequest
    raw = _process_stop_request().model_dump(mode="json")
    if correlation == "absent": raw["request_context"].pop("correlation_id")
    else: raw["request_context"]["correlation_id"] = None
    request = ComponentProcessStopRequest.model_validate(raw)
    canonical = request.model_dump(mode="json")
    assert set(canonical["request_context"]) == {"request_id", "request_owner_epoch", "source_label"}
    observed = []
    client = ManagerCommandClient(tmp_path)
    def exchange(command):
        observed.append(command)
        assert "correlation_id" not in command["request"]["request_context"]
        return _process_stop_response(request)
    monkeypatch.setattr(client, "_exchange", exchange)
    assert getattr(client, method)(request)["request"] == canonical
    assert observed == [{"api_version": "manyforge.manager_command.v1", "command": method, "request": canonical}]
