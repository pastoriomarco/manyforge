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
    ManagerApplyAcknowledgementContext,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
)
from manyforge_headless_api.native import HeadlessApiError


# Formed keys (class 3 HTTP client keys) of the requests below.
_APPLY_KEY = "c0" + "01" * 15
_SCENE_KEY = "c0" + "02" * 15
_ACK_KEY = "c0" + "03" * 15
_NEW_KEY = "c0" + "04" * 15
_STATUS_KEY = "c0" + "05" * 15
_OTHER_KEY = "c0" + "0f" * 15


def test_manager_transport_accepts_bounded_cold_apply_timeout(tmp_path: Path) -> None:
    ManagerCommandClient(tmp_path, timeout_seconds=12.0)
    with pytest.raises(ValueError, match=r"\(0, 60\]"):
        ManagerCommandClient(tmp_path, timeout_seconds=60.1)


def _prepared_activation() -> dict[str, object]:
    return {
        "schema_version": "manyforge.prepared_activation_input.v8",
        "origin": {"kind": "scenario", "scenario_id": "scenario.1",
                   "source_document_digest": "sha256:" + "1" * 64,
                   "project_path": "/projects/scenario.1.scenario"},
        "captured_input_digest": "sha256:" + "d" * 64,
        "deployment_revision": "sha256:" + "a" * 64,
        "retained_bundle_id": "sha256:" + "2" * 64,
        "graph_revision": "sha256:" + "3" * 64,
        "program_revision": "sha256:" + "4" * 64,
        "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
        "node_environment_digest": "sha256:" + "5" * 64,
        "assistant_mode": "optional",
        "prepared_activation_id": "sha256:" + "e" * 64,
        "scene_meshes": [],
    }


def _request() -> ManagerApplyRequest:
    return ManagerApplyRequest.model_validate(
        {
            "request_key": _APPLY_KEY,
            "source_label": "test",
            "deployment_revision": "sha256:" + "a" * 64,
            "expected_current": {
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
            },
            "reject_if_affected_busy": True,
            "prepared_activation": _prepared_activation(),
            "expected_admission_generation": 7,
            "candidate_receipt": {
                "schema_version": "manyforge.manager_deployment_candidate_receipt.v3",
                "deployment_revision": "sha256:" + "a" * 64,
                "prepared_activation_id": "sha256:" + "e" * 64,
                "candidate_retained_bundle_id": "bundle.2",
                "candidate_graph_revision": "graph.2",
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "expected_admission_generation": 7,
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
                "request_key": _SCENE_KEY,
                "source_label": "behavior",
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


def test_manager_command_waits_for_its_reply_or_the_closed_connection(
    tmp_path: Path,
) -> None:
    # OD-298 (a): only the connection is limited; a reply later than the
    # limit still arrives, and a connection closed without a reply is the
    # Manager not answering.
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    socket_path.chmod(0o600)
    server.listen(2)

    def serve() -> None:
        connection, _address = server.accept()
        with connection:
            connection.recv(16 * 1024)
            time.sleep(0.5)
            connection.send(
                b'{"api_version":"manyforge.manager_command_response.v3",'
                b'"error":{"code":"kernel_owner_unavailable",'
                b'"message":"Apply is unavailable"},"ok":false,'
                b'"request_key":"' + _APPLY_KEY.encode() + b'"}'
            )
        connection, _address = server.accept()
        with connection:
            connection.recv(16 * 1024)

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        client = ManagerCommandClient(tmp_path, timeout_seconds=0.2)
        assert client.apply(_request())["ok"] is False
        with pytest.raises(HeadlessApiError) as captured:
            client.apply(_request())
        assert captured.value.code == "manager_command_unavailable"
    finally:
        server.close()
        worker.join(timeout=2.0)
    assert not worker.is_alive()


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
                b'{"api_version":"manyforge.manager_command_response.v3",'
                b'"error":{"code":"kernel_owner_unavailable",'
                b'"message":"Apply is unavailable"},"ok":false,'
                b'"request_key":"' + _APPLY_KEY.encode() + b'"}'
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
        "deployment_revision",
        "admission_context",
        "expected_current",
        "prepared_activation",
        "scene_initialization",
        "reject_if_affected_busy",
        "request_key",
        "source_label",
    }
    assert decoded["source_label"] == "test"
    assert decoded["request_key"] == _APPLY_KEY
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
                b'{"api_version":"manyforge.manager_command_response.v3",'
                b'"error":null,"ok":true,"request_key":"' + _APPLY_KEY.encode() + b'"}'
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


def test_manager_client_does_not_compare_the_response_echo(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # Each exchange is its own connection: the echoed identity is not compared.
    client = ManagerCommandClient(tmp_path)
    response = {
        "api_version": "manyforge.manager_command_response.v3",
        "ok": False,
        "request_key": _OTHER_KEY,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: response)

    assert client.apply(_request()) == response


def test_manager_client_observes_apply_without_replaying_submission(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    commands: list[dict[str, object]] = []

    def exchange(command: dict[str, object]) -> dict[str, object]:
        commands.append(command)
        return _status_response()

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply(3, 3)

    assert response["state"] == "pending"
    assert response["phase"] == "accepted"
    # Observation is one read of the durable identity: it submits nothing, so a
    # reconnecting client can never repeat teardown, startup or publication.
    assert commands == [{
        "api_version": "manyforge.manager_command.v2",
        "apply_generation": 3,
        "apply_id": 3,
        "command": "apply_status",
    }]


def _status_response() -> dict[str, object]:
    return {
        "api_version": "manyforge.manager_command_response.v3",
        "apply_generation": 3,
        "apply_id": 3,
        "candidate_projection_revision": None,
        "error": None,
        "failure": None,
        "last_progress": {
            "phase": "accepted",
            "record_digest": "sha256:" + "f" * 64,
        },
        "ok": True,
        "phase": "accepted",
        "scene_initialization": "preserve",
        "prepared_activation": {
            **{
                field: _prepared_activation()[field]
                for field in (
                    "captured_input_digest",
                    "deployment_revision",
                    "prepared_activation_id",
                )
            },
            "program_binding_id": _prepared_activation()["program_outcome"][
                "program_binding_id"
            ],
        },
        "request_key": _STATUS_KEY,
        "state": "pending",
        "failure_acknowledgement": "unavailable",
    }


def _failed_status_response(
    *, failure_acknowledgement: str
) -> dict[str, object]:
    response = _status_response()
    response.update(
        {
            "state": "failed",
            "failure_acknowledgement": failure_acknowledgement,
            "failure": {
                "cause_code": "manager_apply_preflight_refused",
                "cause_details": {},
                "code": "manager_apply_preflight_refused",
                "effect_boundary": "before_effect",
                "effect_disposition": "known",
                "message": "Apply was refused before effect.",
                "phase": "accepted",
                "producer": "component_manager",
            },
        }
    )
    return response


def test_manager_client_sends_one_exact_apply_acknowledgement(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    commands: list[dict[str, object]] = []

    def exchange(command: dict[str, object]) -> dict[str, object]:
        commands.append(command)
        return _failed_status_response(failure_acknowledgement="acknowledged")

    monkeypatch.setattr(client, "_exchange", exchange)
    context = ManagerApplyAcknowledgementContext.model_validate({
        "request_key": _ACK_KEY,
        "source_label": "manyforge_composer",
    })
    response = client.acknowledge_apply(3, 3, context)

    assert response["state"] == "failed"
    assert response["failure_acknowledgement"] == "acknowledged"
    assert commands == [{
        "api_version": "manyforge.manager_command.v2",
        "apply_generation": 3,
        "apply_id": 3,
        "command": "apply_acknowledge",
        "request_context": {
            "request_key": _ACK_KEY,
            "source_label": "manyforge_composer",
        },
    }]


def test_manager_client_sends_one_exact_apply_release_for_new(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    commands: list[dict[str, object]] = []
    released = {
        "api_version": MANAGER_RESPONSE_VERSION, "error": None, "ok": True,
        "released": {"apply_id": 3, "apply_generation": 3},
    }

    def exchange(command: dict[str, object]) -> dict[str, object]:
        commands.append(command)
        return released

    monkeypatch.setattr(client, "_exchange", exchange)
    context = ManagerApplyAcknowledgementContext.model_validate({
        "request_key": _NEW_KEY, "source_label": "manyforge_composer",
    })
    assert client.release_failed_apply_for_new(context) == released
    assert commands == [{
        "api_version": "manyforge.manager_command.v2",
        "command": "apply_release_for_new",
        "request_context": {
            "request_key": _NEW_KEY,
            "source_label": "manyforge_composer",
        },
    }]
    monkeypatch.setattr(client, "_exchange", lambda _command: {**released, "released": None})
    assert client.release_failed_apply_for_new(context)["released"] is None
    monkeypatch.setattr(client, "_exchange", lambda _command: {**released, "released": 3})
    with pytest.raises(HeadlessApiError) as captured:
        client.release_failed_apply_for_new(context)
    assert captured.value.code == "manager_command_response_invalid"
    # The released Apply is named by its integer Apply number (1.16.1).
    monkeypatch.setattr(client, "_exchange", lambda _command: {
        **released, "released": {"apply_id": "3", "apply_generation": 3}})
    with pytest.raises(HeadlessApiError) as captured:
        client.release_failed_apply_for_new(context)
    assert captured.value.code == "manager_command_response_invalid"
    with pytest.raises(HeadlessApiError) as captured:
        client.observe_apply("3", 3)  # type: ignore[arg-type]
    assert captured.value.code == "manager_apply_status_invalid"


def test_apply_acknowledgement_preserves_pre_attempt_refusal_details(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    refusal = {
        "api_version": MANAGER_RESPONSE_VERSION,
        "ok": False,
        "error": {
            "code": "stale_generation", "message": "The predecessor changed.",
            "details": {"effect": "none"}, "producer": "kernel",
        },
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: refusal)
    context = ManagerApplyAcknowledgementContext.model_validate({
        "request_key": _ACK_KEY, "source_label": "manyforge_composer",
    })
    assert client.acknowledge_apply(3, 3, context) == refusal


def test_manager_client_rejects_malformed_acknowledgement_state(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        response = _failed_status_response(failure_acknowledgement="unavailable")
        response["failure_acknowledgement"] = "maybe"
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    with pytest.raises(HeadlessApiError) as captured:
        client.acknowledge_apply(
            3,
            3,
            ManagerApplyAcknowledgementContext.model_validate({
                "request_key": _ACK_KEY,
                "source_label": "manyforge_composer",
            }),
        )
    assert captured.value.code == "manager_command_response_invalid"


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
        client.observe_apply(3, 3)
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
        client.observe_apply(3, 3)
    assert captured.value.code == "manager_command_response_invalid"


def test_manager_client_reports_an_unknown_operation_status_as_a_typed_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)

    def exchange(_command: dict[str, object]) -> dict[str, object]:
        return {
            "api_version": "manyforge.manager_command_response.v3",
            "error": {
                "code": "apply_status_not_found",
                "message": "Manager Apply identity is unknown",
            },
            "ok": False,
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply(3, 3)
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
    response = client.observe_apply(3, 3)
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
                "cause_code": "planned_maintenance_refused",
                "cause_details": {},
                "producer": "component_manager",
                "effect_boundary": "before_effect",
            },
        }

    monkeypatch.setattr(client, "_exchange", exchange)
    response = client.observe_apply(3, 3)
    assert response["phase"] == "accepted"
    assert response["failure"]["code"] == "planned_maintenance_refused"
    assert response["failure"]["producer"] == "component_manager"


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
                        "api_version": "manyforge.manager_command_response.v3",
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
                        "request_key": _SCENE_KEY,
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
            },
        }
    )
    try:
        response = ManagerCommandClient(tmp_path).commit_scene_transaction(
            request, trusted_principal_id="operator.7", mesh_content_digest=None
        )
        assert response["projection_status"] == "committed"
    finally:
        server.close()
        worker.join(timeout=1.0)
    assert not worker.is_alive()
    command = json.loads(received[0])
    assert received[0] == rfc8785.dumps(command)
    assert set(command) == {"api_version", "command", "transaction"}
    assert command["command"] == "scene_commit"
    assert command["transaction"]["attribution"] == {
        "assurance": "externally_authenticated",
        "principal_id": "operator.7",
        "transport": "http",
    }
    assert "support_contacts" not in command["transaction"]["operation"]


def test_scene_body_carries_a_captured_mesh_digest_only_when_given(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An API scene call's captured mesh digest reaches the Manager (OD-335)."""

    request = _scene_request()
    request = SceneTransactionRequest.model_validate({
        **request.model_dump(mode="json"),
        "operation": {
            "kind": "add_object",
            "scene_resource_id": "part.1",
            "shape": {"type": "mesh", "mesh_resource_uri": "meshes/part.stl",
                      "mesh_scale": {"x_m": 1.0, "y_m": 1.0, "z_m": 1.0}},
            "pose": {
                "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.01},
                "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
            },
            "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
        },
    })
    client = ManagerCommandClient(tmp_path)
    commands = []

    def exchange(command):
        commands.append(command)
        raise HeadlessApiError("stop", "stop")

    monkeypatch.setattr(client, "_exchange", exchange)
    digest = "sha256:" + "7" * 64
    for mesh_content_digest in (digest, None):
        with pytest.raises(HeadlessApiError):
            client.commit_scene_transaction(
                request, trusted_principal_id=None, mesh_content_digest=mesh_content_digest)
    with pytest.raises(HeadlessApiError):
        client.scene_transaction_status(request)
    shapes = [command["transaction"]["operation"]["shape"] for command in commands]
    assert [shape.get("mesh_content_digest") for shape in shapes] == [digest, None, None]
    assert "mesh_content_digest" in shapes[0]
    assert all("mesh_content_digest" not in shape for shape in shapes[1:])


def test_scene_client_rejects_an_inconsistent_owner_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    valid_failure = {
        "api_version": "manyforge.manager_command_response.v3",
        "committed_projection_revision": None,
        "error": {"code": "object_not_found", "details": {}, "message": "absent"},
        "ok": False,
        "projection_status": "not_attempted",
        "replay_status": "accepted_new",
        "request_key": _OTHER_KEY,
        "transaction_sequence": None,
        "update_status": None,
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: valid_failure)
    # Each exchange is its own connection: the echoed identity is not compared.
    assert (
        client.commit_scene_transaction(_scene_request(), trusted_principal_id=None, mesh_content_digest=None)
        == valid_failure
    )

    valid_failure["projection_status"] = "committed"
    with pytest.raises(HeadlessApiError) as captured:
        client.commit_scene_transaction(_scene_request(), trusted_principal_id=None, mesh_content_digest=None)
    assert captured.value.code == "manager_command_response_invalid"

    scene_id = "sha256:" + "d" * 64
    mismatched_success = {
        "api_version": "manyforge.manager_command_response.v3",
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
        "request_key": _SCENE_KEY,
        "transaction_sequence": 1,
        "update_status": "geometry_changed",
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: mismatched_success)
    assert (
        client.commit_scene_transaction(_scene_request(), trusted_principal_id=None, mesh_content_digest=None)
        == mismatched_success
    )
    mismatched_success["extra"] = None
    with pytest.raises(HeadlessApiError) as captured:
        client.commit_scene_transaction(_scene_request(), trusted_principal_id=None, mesh_content_digest=None)
    assert captured.value.code == "manager_command_response_invalid"


def test_scene_result_lookup_is_read_only_and_validates_the_nested_owner_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    client = ManagerCommandClient(tmp_path)
    request = _scene_request()
    commands = []
    response = {"api_version": "manyforge.manager_command_response.v3", "ok": True,
                "result": None}

    def exchange(command):
        commands.append(command)
        return response

    monkeypatch.setattr(client, "_exchange", exchange)
    assert client.scene_transaction_status(request)["result"] is None
    assert commands[0]["command"] == "scene_transaction_status"
    assert "program_node_evidence" not in commands[0]["transaction"]
    response["result"] = {
        "api_version": "manyforge.manager_command_response.v3",
        "committed_projection_revision": None,
        "error": {"code": "object_not_found", "details": {}, "message": "absent"},
        "ok": False, "projection_status": "not_attempted", "replay_status": "replayed_exact",
        "request_key": request.request_context.request_key,
        "transaction_sequence": None, "update_status": None,
    }
    assert client.scene_transaction_status(request)["result"]["ok"] is False
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
            "api_version": "manyforge.manager_command_response.v3",
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
            "api_version": "manyforge.manager_command.v2",
            "command": "scene_pose_query",
            "expected_projection_revision": (
                query.expected_projection_revision.model_dump()
            ),
            "scene_resource_ids": ["part.1"],
        }
    ]

    expected = query.expected_projection_revision.model_dump()
    # A newer revision of the same Scene is the owner's latest truth.
    newer = exchange({})
    newer["projection_revision"] = {
        **expected,
        "scene_revision": {
            **expected["scene_revision"],
            "revision_id": 2,
            "content_hash": "sha256:" + "f" * 64,
        },
    }
    monkeypatch.setattr(client, "_exchange", lambda _command: newer)
    assert (
        client.resolve_scene_resources(query)["projection_revision"]
        == newer["projection_revision"]
    )

    # The owner refuses a lineage change itself; the client checks the shape.
    corrupt = exchange({})
    corrupt["poses"][0]["extra"] = None
    monkeypatch.setattr(client, "_exchange", lambda _command: corrupt)
    with pytest.raises(HeadlessApiError) as captured:
        client.resolve_scene_resources(query)
    assert captured.value.code == "manager_command_response_invalid"

    rejected = {
        "api_version": "manyforge.manager_command_response.v3",
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


@pytest.mark.parametrize("producer", ["component_manager", "kernel"])
def test_manager_refusal_keeps_its_typed_producer(tmp_path, monkeypatch, producer):
    client = ManagerCommandClient(tmp_path)
    response = {"api_version": "manyforge.manager_command_response.v3", "ok": False,
                "error": {"code": "stale_generation", "details": {"reason": "expected_generation_stale"},
                          "message": "cell-admission request rejected", "producer": producer}}
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError) as caught:
        client.scene_runtime_owner()
    assert caught.value.code == "stale_generation"
    assert caught.value.details == {"reason": "expected_generation_stale"}
    assert caught.value.producer == producer


@pytest.mark.parametrize("error", [
    {"code": "x_refused", "message": "m", "producer": ""},
    {"code": "x_refused", "message": "m", "producer": 3},
    {"code": "x_refused", "message": "m", "extra": "field"},
])
def test_manager_refusal_with_malformed_producer_is_invalid(tmp_path, monkeypatch, error):
    client = ManagerCommandClient(tmp_path)
    response = {"api_version": "manyforge.manager_command_response.v3", "ok": False, "error": error}
    monkeypatch.setattr(client, "_exchange", lambda _: response)
    with pytest.raises(HeadlessApiError) as caught:
        client.scene_runtime_owner()
    assert caught.value.code == "manager_command_response_invalid"


@pytest.mark.parametrize("purpose", ["preserve", "initialize"])
def test_manager_transport_sends_explicit_scene_initialization(tmp_path, monkeypatch, purpose):
    request = _request().model_copy(update={"scene_initialization": purpose})
    commands = []
    client = ManagerCommandClient(tmp_path)
    def exchange(command):
        commands.append(command)
        return {"ok": True, "request_key": request.request_key}
    monkeypatch.setattr(client, "_exchange", exchange)
    client.apply(request)
    assert commands[0]["scene_initialization"] == purpose


def _reference_error() -> dict[str, object]:
    return {
        "code": "program_reference_validation_failed",
        "message": "Program references are invalid for the candidate Deployment",
        "details": {},
        "producer": "component_manager",
        "reference_validation": {
            "diagnostics": [{
                "code": "program_reference_missing",
                "location": {"document": "program", "path": "/nodes/0",
                             "node_instance_id": "node.1", "parameter": "robot"},
                "payload": {"component_id": "missing.1"},
            }],
            "total_count": 2,
            "truncated": True,
        },
    }


def test_manager_apply_preserves_reference_diagnostics(tmp_path: Path, monkeypatch) -> None:
    client = ManagerCommandClient(tmp_path)
    error = _reference_error()
    monkeypatch.setattr(client, "_exchange", lambda _command: {"ok": False, "error": error})
    with pytest.raises(HeadlessApiError) as raised:
        client.apply(_request())
    assert raised.value.code == error["code"]
    assert raised.value.producer == "component_manager"
    assert raised.value.details == error["reference_validation"]


@pytest.mark.parametrize("mutation", [
    "wrong_code", "extra_key", "bad_row", "bad_count", "bad_truncation",
])
def test_manager_apply_rejects_malformed_reference_diagnostics(
    tmp_path: Path, monkeypatch, mutation: str
) -> None:
    import copy
    error = copy.deepcopy(_reference_error())
    reference = error["reference_validation"]
    if mutation == "wrong_code":
        error["code"] = "other_error"
    elif mutation == "extra_key":
        reference["extra"] = True
    elif mutation == "bad_row":
        reference["diagnostics"][0]["location"]["document"] = "deployment"
    elif mutation == "bad_count":
        reference["total_count"] = True
    elif mutation == "bad_truncation":
        reference["truncated"] = False
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: {"ok": False, "error": error})
    with pytest.raises(HeadlessApiError) as raised:
        client.apply(_request())
    assert raised.value.code == "manager_command_response_invalid"


def test_manager_reference_field_is_optional(tmp_path: Path, monkeypatch) -> None:
    error = _reference_error()
    del error["reference_validation"]
    response = {"ok": False, "error": error}
    client = ManagerCommandClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    assert client.apply(_request()) == response


def test_manager_rejects_the_previous_response_version(tmp_path: Path) -> None:
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    path = tmp_path / "manager-command.sock"
    server.bind(str(path))
    path.chmod(0o600)
    server.listen(1)

    def serve() -> None:
        connection, _ = server.accept()
        with connection:
            connection.recv(128 * 1024)
            connection.send(rfc8785.dumps({
                "api_version": "manyforge.manager_command_response.v2",
                "ok": False,
                "error": {"code": "busy", "message": "busy"},
            }))

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        with pytest.raises(HeadlessApiError) as raised:
            ManagerCommandClient(tmp_path).apply(_request())
        assert raised.value.code == "manager_command_response_invalid"
    finally:
        server.close()
        worker.join(timeout=2)
    assert not worker.is_alive()
