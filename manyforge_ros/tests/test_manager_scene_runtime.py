from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import stat
import threading

import pytest
import rfc8785

from manyforge_ros.manager_scene_runtime import (
    MANAGER_COMMAND_VERSION,
    MANAGER_RESPONSE_VERSION,
    ManagerSceneRuntimeClient,
    PoseObservationIntent,
    PoseObserverBinding,
    SceneRuntimeManagerError,
    SceneRuntimeOwnerSnapshot,
)


def _bindings() -> tuple[PoseObserverBinding, ...]:
    return (
        PoseObserverBinding(
            object_id="ur10e",
            policy_id="viewer.measured_robot",
            eligible=False,
        ),
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
        "bindings": [binding.to_value() for binding in _bindings()],
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
        "scene_application_status": "not_requested",
        "scene_projection_revision": None,
        "scene_request_id": None,
        "scene_runtime_source_generation": 3,
        "scene_update_status": None,
        "source_epoch": "orin-observer.1",
        "source_epoch_order": 1,
        "source_id": "isaac.observer",
        "source_sequence": 1,
    }


def _projection_revision() -> dict[str, object]:
    return {
        "graph_revision": "sha256:" + "b" * 64,
        "retained_bundle_id": "sha256:" + "a" * 64,
        "scene_revision": {
            "content_hash": "sha256:" + "c" * 64,
            "revision_id": 4,
            "scene_id": "sha256:" + "d" * 64,
        },
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
            bindings=_bindings(),
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
    assert result.scene_application_status == "not_requested"
    assert result.scene_projection_revision is None
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
    assert decoded[1]["bindings"] == [
        binding.to_value() for binding in _bindings()
    ]
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


def test_client_parses_and_cross_checks_committed_scene_application(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client = ManagerSceneRuntimeClient(tmp_path)
    binding = PoseObserverBinding(
        object_id="ur10e",
        policy_id="continuous_unattached",
        eligible=True,
    )
    opened = _open_response()
    opened["bindings"] = [binding.to_value()]
    responses = [_owner_response(), opened]
    committed = _observe_response()
    committed.update(
        {
            "disposition": "apply_now",
            "policy_id": "continuous_unattached",
            "scene_application_status": "committed",
            "scene_projection_revision": _projection_revision(),
            "scene_request_id": "scene-runtime-pose-" + "e" * 71,
            "scene_update_status": "geometry_changed",
        }
    )
    responses.append(committed)
    monkeypatch.setattr(client, "_exchange", lambda _command: responses.pop(0))

    owner = client.read_owner()
    session = client.open_observer(
        owner,
        bindings=(binding,),
        source_id="isaac.observer",
        source_epoch="orin-observer.1",
    )
    result = client.observe(
        session,
        source_sequence=1,
        intent=PoseObservationIntent(
            object_id="ur10e",
            policy_id="continuous_unattached",
            lifetime_ns=1_000_000_000,
            quality="good",
            eligible=True,
            payload={"kind": "simulation_entity_pose"},
        ),
    )

    assert result.scene_application_status == "committed"
    assert result.scene_request_id == "scene-runtime-pose-" + "e" * 71
    assert result.scene_update_status == "geometry_changed"
    assert result.scene_projection_revision is not None
    assert result.scene_projection_revision.revision_id == 4

    latched = dict(committed)
    latched.update(
        {
            "disposition": "observed_only",
            "policy_id": "before_planning",
        }
    )
    monkeypatch.setattr(client, "_exchange", lambda _command: latched)
    latched_result = client.observe(
        session,
        source_sequence=1,
        intent=PoseObservationIntent(
            object_id="ur10e",
            policy_id="before_planning",
            lifetime_ns=1_000_000_000,
            quality="good",
            eligible=True,
            payload={"kind": "simulation_entity_pose"},
        ),
    )
    assert latched_result.scene_application_status == "committed"
    assert latched_result.scene_projection_revision is not None

    invalid = dict(committed)
    invalid["scene_update_status"] = None
    monkeypatch.setattr(client, "_exchange", lambda _command: invalid)
    with pytest.raises(SceneRuntimeManagerError) as rejected:
        client.observe(
            session,
            source_sequence=1,
            intent=PoseObservationIntent(
                object_id="ur10e",
                policy_id="continuous_unattached",
                lifetime_ns=1_000_000_000,
                quality="good",
                eligible=True,
                payload={"kind": "simulation_entity_pose"},
            ),
        )
    assert rejected.value.code == "manager_scene_runtime_response_invalid"


def test_client_accepts_freshness_confirmation_with_unchanged_projection(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client = ManagerSceneRuntimeClient(tmp_path)
    binding = PoseObserverBinding(
        object_id="ur10e",
        policy_id="continuous_unattached",
        eligible=True,
    )
    opened = _open_response()
    opened["bindings"] = [binding.to_value()]
    confirmed = _observe_response()
    confirmed.update(
        {
            "disposition": "apply_now",
            "policy_id": "continuous_unattached",
            "scene_application_status": "freshness_confirmed",
            "scene_projection_revision": _projection_revision(),
            "scene_request_id": "scene-runtime-pose-" + "e" * 71,
            "scene_update_status": None,
            "source_sequence": 2,
        }
    )
    responses = [_owner_response(), opened, confirmed]
    monkeypatch.setattr(client, "_exchange", lambda _command: responses.pop(0))
    owner = client.read_owner()
    session = client.open_observer(
        owner,
        bindings=(binding,),
        source_id="isaac.observer",
        source_epoch="orin-observer.1",
    )

    def observe(policy_id: str) -> object:
        return client.observe(
            session,
            source_sequence=2,
            intent=PoseObservationIntent(
                object_id="ur10e",
                policy_id=policy_id,
                lifetime_ns=1_000_000_000,
                quality="good",
                eligible=True,
                payload={"kind": "simulation_entity_pose"},
            ),
        )

    result = observe("continuous_unattached")
    assert result.scene_application_status == "freshness_confirmed"
    assert result.scene_update_status is None
    assert result.scene_projection_revision is not None
    assert result.scene_projection_revision.revision_id == 4

    latched = dict(confirmed)
    latched.update({"disposition": "observed_only", "policy_id": "before_planning"})
    monkeypatch.setattr(client, "_exchange", lambda _command: latched)
    assert observe("before_planning").scene_application_status == (
        "freshness_confirmed"
    )

    for field, value in (
        ("scene_update_status", "freshness_only"),
        ("scene_projection_revision", None),
        ("scene_request_id", None),
    ):
        invalid = dict(confirmed)
        invalid[field] = value
        monkeypatch.setattr(client, "_exchange", lambda _command, r=invalid: r)
        with pytest.raises(SceneRuntimeManagerError) as rejected:
            observe("continuous_unattached")
        assert rejected.value.code == "manager_scene_runtime_response_invalid"

    buffered = dict(confirmed)
    buffered["disposition"] = "buffered"
    monkeypatch.setattr(client, "_exchange", lambda _command: buffered)
    with pytest.raises(SceneRuntimeManagerError) as contradicted:
        observe("continuous_unattached")
    assert contradicted.value.code == "manager_scene_runtime_response_invalid"


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
            bindings=_bindings(),
            source_id="isaac.observer",
            source_epoch="orin-observer.1",
        )
    assert crossed.value.code == (
        "manager_scene_runtime_response_identity_mismatch"
    )


def test_client_rejects_unsafe_or_replaced_manager_run_root(
    tmp_path: Path,
) -> None:
    unsafe = tmp_path / "unsafe"
    unsafe.mkdir(mode=0o700)
    unsafe.chmod(0o720)
    with pytest.raises(SceneRuntimeManagerError, match="safe real"):
        ManagerSceneRuntimeClient(unsafe)

    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    client = ManagerSceneRuntimeClient(run_root)
    run_root.rename(tmp_path / "retained-run")
    run_root.mkdir(mode=0o700)
    with pytest.raises(SceneRuntimeManagerError, match="identity changed"):
        client.read_owner()


def test_client_pins_manager_run_root_through_socket_use(
    tmp_path: Path,
) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    socket_path = run_root / "manager-command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(socket_path))
    socket_path.chmod(0o600)
    client = ManagerSceneRuntimeClient(run_root)

    try:
        with pytest.raises(SceneRuntimeManagerError, match="changed during use"):
            with client._socket_address() as address:  # noqa: SLF001
                retained_root = tmp_path / "retained-run"
                run_root.rename(retained_root)
                run_root.mkdir(mode=0o700)
                assert stat.S_ISSOCK(os.stat(address).st_mode)
    finally:
        listener.close()


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
        bindings=_bindings(),
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
