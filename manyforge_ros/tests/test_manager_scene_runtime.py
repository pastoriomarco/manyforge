from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import threading

import pytest
import rfc8785

from manyforge_ros.manager_scene_runtime import (
    MANAGER_COMMAND_VERSION,
    MANAGER_RESPONSE_VERSION,
    ManagerSceneRuntimeClient,
    PoseObservationIntent,
    SceneRuntimeManagerError,
    SceneRuntimeOwnerSnapshot,
)


def _identity(runtime_generation: int = 7) -> dict[str, object]:
    return {
        "deployment_generation": 7,
        "graph_revision": "sha256:" + "b" * 64,
        "program_generation": 0,
        "retained_bundle_id": "sha256:" + "a" * 64,
        "runtime_generation": runtime_generation,
    }


def _owner_response() -> dict[str, object]:
    return {
        "api_version": MANAGER_RESPONSE_VERSION,
        "cell_id": "cell.ur10",
        "initialization_generation": 7,
        "initialization_state": "open",
        "ok": True,
        "runtime_identity": _identity(),
        "scene_runtime_owner_epoch": "scene-runtime-owner.1",
    }


def _open_response() -> dict[str, object]:
    return {
        "api_version": MANAGER_RESPONSE_VERSION,
        "cell_id": "cell.ur10",
        "ok": True,
        "runtime_identity": _identity(),
        "source_epoch": "orin-observer.1",
        "source_epoch_order": 1,
        "source_id": "isaac.observer",
    }


def _observe_response() -> dict[str, object]:
    return {
        "api_version": MANAGER_RESPONSE_VERSION,
        "disposition": "observed_only",
        "fresh": True,
        "initialization_generation": 7,
        "object_id": "ur10e",
        "ok": True,
        "policy_id": "viewer.measured_robot",
        "scene_runtime_source_generation": 3,
        "source_epoch": "orin-observer.1",
        "source_epoch_order": 1,
        "source_id": "isaac.observer",
        "source_sequence": 1,
    }


def test_client_exchanges_exact_canonical_owner_open_and_observe_packets(
    tmp_path: Path,
) -> None:
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o600)
    server.listen(3)
    responses = [_owner_response(), _open_response(), _observe_response()]
    received: list[bytes] = []
    server_failure: list[BaseException] = []

    def serve() -> None:
        try:
            for response in responses:
                connection, _address = server.accept()
                with connection:
                    received.append(connection.recv(132 * 1024))
                    connection.send(rfc8785.dumps(response))
        except BaseException as exc:  # pragma: no cover - surfaced below
            server_failure.append(exc)

    worker = threading.Thread(target=serve)
    worker.start()
    payload = {
        "api_version": "manyforge.ros_pose_observation.v1",
        "kind": "joint_state",
        "names": ["shoulder_pan_joint"],
        "positions": [0.25],
    }
    try:
        client = ManagerSceneRuntimeClient(tmp_path)
        owner = client.read_owner()
        session = client.open_observer(
            owner,
            source_id="isaac.observer",
            source_epoch="orin-observer.1",
        )
        intent = PoseObservationIntent(
            object_id="ur10e",
            policy_id="viewer.measured_robot",
            lifetime_ns=1_000_000_000,
            quality="good",
            eligible=False,
            payload=payload,
        )
        payload["positions"][0] = 9.0
        result = client.observe(session, source_sequence=1, intent=intent)
    finally:
        server.close()
        worker.join(timeout=2.0)

    assert not worker.is_alive()
    assert server_failure == []
    assert owner == SceneRuntimeOwnerSnapshot(
        cell_id="cell.ur10",
        owner_epoch="scene-runtime-owner.1",
        runtime_identity=owner.runtime_identity,
        initialization_generation=7,
        initialization_state="open",
    )
    assert session.source_epoch_order == 1
    assert result.disposition == "observed_only"
    assert result.fresh is True
    assert result.scene_runtime_source_generation == 3
    assert len(received) == 3
    decoded = [json.loads(packet) for packet in received]
    assert all(
        packet == rfc8785.dumps(value)
        for packet, value in zip(received, decoded)
    )
    assert decoded[0] == {
        "api_version": MANAGER_COMMAND_VERSION,
        "command": "scene_runtime_owner_read",
    }
    assert decoded[1]["expected_runtime_identity"] == _identity()
    assert decoded[2]["source_sequence"] == 1
    assert decoded[2]["payload"]["positions"] == [0.25]
    assert set(decoded[2]) == {
        "api_version",
        "command",
        "eligible",
        "expected_runtime_identity",
        "lifetime_ns",
        "object_id",
        "payload",
        "policy_id",
        "quality",
        "source_epoch",
        "source_id",
        "source_sequence",
    }


def test_client_surfaces_exact_owner_rejection_code(tmp_path: Path) -> None:
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o600)
    server.listen(1)

    def serve() -> None:
        connection, _address = server.accept()
        with connection:
            connection.recv(4096)
            connection.send(
                rfc8785.dumps(
                    {
                        "api_version": MANAGER_RESPONSE_VERSION,
                        "error": {
                            "code": "scene_runtime_identity_changed",
                            "message": "runtime advanced",
                        },
                        "ok": False,
                    }
                )
            )

    worker = threading.Thread(target=serve)
    worker.start()
    try:
        with pytest.raises(SceneRuntimeManagerError) as raised:
            ManagerSceneRuntimeClient(tmp_path).read_owner()
    finally:
        server.close()
        worker.join(timeout=1.0)
    assert raised.value.code == "scene_runtime_identity_changed"
    assert str(raised.value) == "runtime advanced"


def test_client_rejects_noncanonical_and_duplicate_responses(tmp_path: Path) -> None:
    for index, response in enumerate(
        (
            b'{"api_version": "manyforge.manager_command_response.v1","ok":true}',
            b'{"api_version":"manyforge.manager_command_response.v1",'
            b'"ok":true,"ok":true}',
        )
    ):
        root = tmp_path / str(index)
        root.mkdir()
        socket_path = root / "manager-command.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        server.bind(str(socket_path))
        os.chmod(socket_path, 0o600)
        server.listen(1)

        def serve(packet: bytes = response) -> None:
            connection, _address = server.accept()
            with connection:
                connection.recv(4096)
                connection.send(packet)

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            with pytest.raises(SceneRuntimeManagerError) as raised:
                ManagerSceneRuntimeClient(root).read_owner()
        finally:
            server.close()
            worker.join(timeout=1.0)
        assert raised.value.code == "manager_scene_runtime_response_invalid"


def test_client_rejects_wrong_endpoint_and_response_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with pytest.raises(SceneRuntimeManagerError) as absent:
        ManagerSceneRuntimeClient(tmp_path).read_owner()
    assert absent.value.code == "manager_scene_runtime_unavailable"

    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o660)
    server.listen(1)
    try:
        with pytest.raises(SceneRuntimeManagerError) as wrong_mode:
            ManagerSceneRuntimeClient(tmp_path).read_owner()
    finally:
        server.close()
    assert wrong_mode.value.code == "manager_scene_runtime_transport_invalid"

    os.chmod(socket_path, 0o600)
    client = ManagerSceneRuntimeClient(tmp_path)
    owner_response = _owner_response()
    monkeypatch.setattr(client, "_exchange", lambda _command: owner_response)
    owner = client.read_owner()
    wrong_open = _open_response()
    wrong_open["source_id"] = "another.observer"
    monkeypatch.setattr(client, "_exchange", lambda _command: wrong_open)
    with pytest.raises(SceneRuntimeManagerError) as crossed:
        client.open_observer(
            owner,
            source_id="isaac.observer",
            source_epoch="orin-observer.1",
        )
    assert crossed.value.code == (
        "manager_scene_runtime_response_identity_mismatch"
    )


def test_observation_intent_and_sequence_are_strict_and_bounded(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    with pytest.raises(ValueError, match="lifetime_ns"):
        PoseObservationIntent(
            object_id="robot",
            policy_id="viewer",
            lifetime_ns=60_000_000_001,
            quality="good",
            eligible=False,
            payload={},
        )
    with pytest.raises(TypeError, match="exact bool"):
        PoseObservationIntent(
            object_id="robot",
            policy_id="viewer",
            lifetime_ns=1,
            quality="good",
            eligible=1,  # type: ignore[arg-type]
            payload={},
        )
    with pytest.raises(ValueError, match="byte bound"):
        PoseObservationIntent(
            object_id="robot",
            policy_id="viewer",
            lifetime_ns=1,
            quality="good",
            eligible=False,
            payload={"value": "x" * (64 * 1024)},
        )

    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
    os.chmod(socket_path, 0o600)
    server.listen(1)
    client = ManagerSceneRuntimeClient(tmp_path)
    monkeypatch.setattr(client, "_exchange", lambda _command: _owner_response())
    owner = client.read_owner()
    monkeypatch.setattr(client, "_exchange", lambda _command: _open_response())
    session = client.open_observer(
        owner,
        source_id="isaac.observer",
        source_epoch="orin-observer.1",
    )
    intent = PoseObservationIntent(
        object_id="robot",
        policy_id="viewer",
        lifetime_ns=1,
        quality="unknown",
        eligible=False,
        payload={},
    )
    with pytest.raises(ValueError, match="source_sequence"):
        client.observe(
            session,
            source_sequence=True,  # type: ignore[arg-type]
            intent=intent,
        )
    server.close()
