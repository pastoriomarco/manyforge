from __future__ import annotations

import json
from pathlib import Path
import socket
import threading

import pytest

from manyforge_headless_api.manager import ManagerCommandClient
from manyforge_headless_api.models import ManagerApplyRequest
from manyforge_headless_api.native import HeadlessApiError


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
        }
    )


def test_manager_client_uses_one_canonical_bounded_seqpacket(tmp_path: Path) -> None:
    socket_path = tmp_path / "manager-command.sock"
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.bind(str(socket_path))
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
        "api_version",
        "command",
        "correlation_id",
        "deployment_artifact_id",
        "deployment_revision",
        "expected_current",
        "reject_if_affected_busy",
        "request_id",
        "request_owner_epoch",
        "source_label",
    }
    assert decoded["source_label"] == "test"
    assert decoded["correlation_id"] == "correlation.1"


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
