from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path
import socket
import threading

import pytest

from manyforge_ros_bringup.cclc_open_request_transport import (
    CCLC_ROS_SOCKET_FILE,
    CCLCOpenUnixClient,
    CCLCTransportError,
    MAX_CCLC_REQUEST_BYTES,
    handle_open_request,
)


def _serve_once(
    root: Path,
    handler: Callable[[bytes], bytes],
) -> threading.Thread:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(root / CCLC_ROS_SOCKET_FILE))
    os.chmod(root / CCLC_ROS_SOCKET_FILE, 0o600)
    listener.listen(1)

    def run() -> None:
        try:
            connection, _address = listener.accept()
            with connection:
                request = connection.recv(MAX_CCLC_REQUEST_BYTES)
                connection.send(handler(request))
        finally:
            listener.close()

    thread = threading.Thread(target=run)
    thread.start()
    return thread


def test_exact_canonical_packet_is_forwarded_without_semantic_projection(
    tmp_path: Path,
) -> None:
    request = (
        '{"api_version":"manyforge.cclc_open_request.v1",'
        '"command":"lifecycle_status","payload":{}}'
    )
    response = (
        '{"api_version":"manyforge.cclc_open_response.v1",'
        '"command":"lifecycle_status","error":null,"ok":true,"result":null}'
    )
    observed: list[bytes] = []
    thread = _serve_once(
        tmp_path,
        lambda payload: observed.append(payload) or response.encode(),
    )
    result = handle_open_request(CCLCOpenUnixClient(tmp_path), request)
    thread.join(timeout=2)

    assert not thread.is_alive()
    assert result.completed
    assert result.canonical_response_json == response
    assert result.error_code == ""
    assert observed == [request.encode()]


def test_transport_rejects_unbounded_and_non_socket_inputs(tmp_path: Path) -> None:
    client = CCLCOpenUnixClient(tmp_path)
    with pytest.raises(CCLCTransportError, match="empty or exceeds"):
        client.exchange("")
    with pytest.raises(CCLCTransportError, match="empty or exceeds"):
        client.exchange("x" * (MAX_CCLC_REQUEST_BYTES + 1))

    (tmp_path / CCLC_ROS_SOCKET_FILE).write_text("not a socket", encoding="utf-8")
    result = handle_open_request(client, "{}")
    assert not result.completed
    assert result.error_code == "cclc_ros_transport_invalid"
    assert result.canonical_response_json == ""


def test_state_root_symlink_is_rejected(tmp_path: Path) -> None:
    real = tmp_path / "real"
    real.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    with pytest.raises(CCLCTransportError, match="real directory"):
        CCLCOpenUnixClient(alias)
