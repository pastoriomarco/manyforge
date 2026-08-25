from __future__ import annotations

import os
from pathlib import Path
import socket
import threading

import rclpy

from manyforge_cclc_msgs.srv import CCLCOpenRequest
from manyforge_ros_bringup.cclc_open_request_node import CCLCOpenRequestNode
from manyforge_ros_bringup.cclc_open_request_transport import (
    CCLC_ROS_SOCKET_FILE,
    MAX_CCLC_REQUEST_BYTES,
)


def test_ros_callback_returns_only_the_exact_owner_response(tmp_path: Path) -> None:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(tmp_path / CCLC_ROS_SOCKET_FILE))
    os.chmod(tmp_path / CCLC_ROS_SOCKET_FILE, 0o600)
    listener.listen(1)
    request_json = (
        '{"api_version":"manyforge.cclc_open_request.v1",'
        '"command":"component_snapshot","payload":{}}'
    )
    response_json = (
        '{"api_version":"manyforge.cclc_open_response.v1",'
        '"command":"component_snapshot","error":null,"ok":true,'
        '"result":null}'
    )

    def serve() -> None:
        try:
            connection, _address = listener.accept()
            with connection:
                assert connection.recv(MAX_CCLC_REQUEST_BYTES) == request_json.encode()
                connection.send(response_json.encode())
        finally:
            listener.close()

    thread = threading.Thread(target=serve)
    thread.start()
    rclpy.init()
    node = CCLCOpenRequestNode(run_root=tmp_path)
    try:
        request = CCLCOpenRequest.Request()
        request.canonical_request_json = request_json
        response = node._on_request(request, CCLCOpenRequest.Response())
        assert response.completed
        assert response.canonical_response_json == response_json
        assert response.error_code == ""
        assert response.error_message == ""
    finally:
        node.destroy_node()
        rclpy.shutdown()
        thread.join(timeout=2)
    assert not thread.is_alive()
