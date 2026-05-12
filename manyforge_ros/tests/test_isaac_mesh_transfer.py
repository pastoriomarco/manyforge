from __future__ import annotations

import hashlib
import json
import threading
from pathlib import Path

import pytest


def test_mesh_chunk_packet_round_trip() -> None:
    from manyforge_ros.isaac_mesh_transfer import (
        CHUNK_VERSION,
        decode_mesh_chunk_packet,
        encode_mesh_chunk_packet,
    )

    payload = b"o cube\nv 0 0 0\n"
    header = {
        "version": CHUNK_VERSION,
        "request_id": "mesh-1",
        "chunk_index": 0,
        "chunk_count": 1,
        "offset": 0,
        "data_size": len(payload),
        "total_bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "filename": "mesh.obj",
    }

    decoded_header, decoded_payload = decode_mesh_chunk_packet(
        encode_mesh_chunk_packet(header, payload)
    )

    assert decoded_header == header
    assert decoded_payload == payload


def test_isaac_mesh_transfer_client_receives_chunked_file(tmp_path: Path) -> None:
    rclpy = pytest.importorskip("rclpy")
    from rclpy.executors import MultiThreadedExecutor
    from std_msgs.msg import String, UInt8MultiArray

    from manyforge_ros.isaac_mesh_transfer import (
        CHUNK_VERSION,
        IsaacMeshTransferClient,
        IsaacMeshTransferRequest,
        encode_mesh_chunk_packet,
    )

    request_topic = "/test_manyforge/isaac_mesh_transfer/request"
    status_topic = "/test_manyforge/isaac_mesh_transfer/status"
    chunk_topic = "/test_manyforge/isaac_mesh_transfer/chunk"
    mesh_bytes = b"o soup\nv 0 0 0\nv 1 0 0\n"
    mesh_sha = hashlib.sha256(mesh_bytes).hexdigest()
    received_requests: list[dict[str, object]] = []

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_fake_isaac_mesh_transfer_service")
    client_node = rclpy.create_node("manyforge_isaac_mesh_transfer_client_test")
    status_publisher = server_node.create_publisher(String, status_topic, 10)
    chunk_publisher = server_node.create_publisher(UInt8MultiArray, chunk_topic, 10)

    def publish_status(payload: dict[str, object]) -> None:
        status = String()
        status.data = json.dumps(payload)
        status_publisher.publish(status)

    def on_request(message: String) -> None:
        request = json.loads(message.data)
        received_requests.append(request)
        request_id = request["request_id"]
        publish_status(
            {
                "version": "manyforge.isaac.mesh_transfer_status.v0.1",
                "request_id": request_id,
                "phase": "started",
                "success": True,
                "filename": "soup.obj",
                "total_bytes": len(mesh_bytes),
                "chunk_count": 2,
                "sha256": mesh_sha,
            }
        )
        chunks = [mesh_bytes[:10], mesh_bytes[10:]]
        for index, payload in enumerate(chunks):
            header = {
                "version": CHUNK_VERSION,
                "request_id": request_id,
                "chunk_index": index,
                "chunk_count": 2,
                "offset": sum(len(item) for item in chunks[:index]),
                "data_size": len(payload),
                "total_bytes": len(mesh_bytes),
                "sha256": mesh_sha,
                "payload_sha256": hashlib.sha256(payload).hexdigest(),
                "filename": "soup.obj",
                "mesh_resource_uri": "file:///tmp/soup.obj",
            }
            chunk = UInt8MultiArray()
            chunk.data = list(encode_mesh_chunk_packet(header, payload))
            chunk_publisher.publish(chunk)
        publish_status(
            {
                "version": "manyforge.isaac.mesh_transfer_status.v0.1",
                "request_id": request_id,
                "phase": "completed",
                "success": True,
                "filename": "soup.obj",
                "total_bytes": len(mesh_bytes),
                "chunk_count": 2,
                "sha256": mesh_sha,
            }
        )

    request_subscription = server_node.create_subscription(
        String,
        request_topic,
        on_request,
        10,
    )

    executor.add_node(server_node)
    executor.add_node(client_node)
    thread = threading.Thread(target=executor.spin, daemon=True)
    thread.start()

    try:
        request = IsaacMeshTransferRequest(
            mesh_resource_uri="file:///tmp/soup.obj",
            request_id="mesh-request-1",
        )
        client = IsaacMeshTransferClient(
            client_node,
            request_topic=request_topic,
            status_topic=status_topic,
            chunk_topic=chunk_topic,
            timeout_s=2.0,
        )
        result = client.transfer(request, output_dir=tmp_path)

        output_path = Path(result["output_path"])
        assert output_path.name == "soup.obj"
        assert output_path.read_bytes() == mesh_bytes
        assert result["sha256"] == mesh_sha
        assert received_requests == [request.to_request_payload()]
    finally:
        server_node.destroy_subscription(request_subscription)
        server_node.destroy_publisher(status_publisher)
        server_node.destroy_publisher(chunk_publisher)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()
