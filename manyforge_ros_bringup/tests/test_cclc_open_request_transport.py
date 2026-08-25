from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path
import socket
import stat
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


def test_run_root_symlink_is_rejected(tmp_path: Path) -> None:
    real = tmp_path / "real"
    real.mkdir()
    alias = tmp_path / "alias"
    alias.symlink_to(real, target_is_directory=True)
    with pytest.raises(CCLCTransportError, match="real directory"):
        CCLCOpenUnixClient(alias)


def test_unsafe_or_replaced_run_root_is_rejected(tmp_path: Path) -> None:
    unsafe = tmp_path / "unsafe"
    unsafe.mkdir(mode=0o700)
    unsafe.chmod(0o720)
    with pytest.raises(CCLCTransportError, match="safe pre-existing"):
        CCLCOpenUnixClient(unsafe)

    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    client = CCLCOpenUnixClient(run_root)
    run_root.rename(tmp_path / "retained-run")
    run_root.mkdir(mode=0o700)
    with pytest.raises(CCLCTransportError, match="identity changed"):
        client.exchange("{}")


def test_client_pins_run_root_through_socket_use(tmp_path: Path) -> None:
    run_root = tmp_path / "run"
    run_root.mkdir(mode=0o700)
    socket_path = run_root / CCLC_ROS_SOCKET_FILE
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(socket_path))
    socket_path.chmod(0o600)
    client = CCLCOpenUnixClient(run_root)

    try:
        with pytest.raises(CCLCTransportError, match="changed during use"):
            with client._socket_address() as address:  # noqa: SLF001
                retained_root = tmp_path / "retained-run"
                run_root.rename(retained_root)
                run_root.mkdir(mode=0o700)
                assert stat.S_ISSOCK(os.stat(address).st_mode)
    finally:
        listener.close()
