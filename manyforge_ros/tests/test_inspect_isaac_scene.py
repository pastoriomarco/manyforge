from __future__ import annotations

import json
import threading

import pytest


def test_inspect_isaac_scene_topic_rpc_returns_correlated_report():
    rclpy = pytest.importorskip("rclpy")
    from rclpy.executors import MultiThreadedExecutor
    from std_msgs.msg import String

    from manyforge_ros.inspect_isaac_scene import (
        InspectIsaacSceneClient,
        IsaacSceneQuery,
    )

    request_topic = "/test_manyforge/isaac_collider_query/request"
    response_topic = "/test_manyforge/isaac_collider_query/response"
    received_requests: list[dict[str, object]] = []

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    server_node = rclpy.create_node("manyforge_fake_isaac_collider_service")
    client_node = rclpy.create_node("manyforge_inspect_isaac_scene_client")
    response_publisher = server_node.create_publisher(String, response_topic, 10)

    def on_request(message: String) -> None:
        request = json.loads(message.data)
        received_requests.append(request)
        response = String()
        response.data = json.dumps(
            {
                "version": "manyforge.isaac.collider_report.v0.1",
                "request_id": request["request_id"],
                "success": True,
                "summary": {
                    "collider_count": 2,
                    "object_count": 1,
                    "error_count": 0,
                },
                "objects": [
                    {
                        "source_root": "/World/soup_can",
                        "collider_count": 2,
                    }
                ],
            }
        )
        response_publisher.publish(response)

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
        query = IsaacSceneQuery(
            include_subtrees=("/World/soup_can",),
            request_id="test-request-1",
        )
        client = InspectIsaacSceneClient(
            client_node,
            request_topic=request_topic,
            response_topic=response_topic,
            timeout_s=2.0,
        )
        response = client.inspect(query)

        assert response["request_id"] == "test-request-1"
        assert response["success"] is True
        assert response["summary"]["collider_count"] == 2
        assert response["objects"][0]["source_root"] == "/World/soup_can"
        assert received_requests == [query.to_request_payload()]
    finally:
        server_node.destroy_subscription(request_subscription)
        server_node.destroy_publisher(response_publisher)
        executor.shutdown()
        server_node.destroy_node()
        client_node.destroy_node()
        rclpy.shutdown()


@pytest.mark.parametrize(
    "payload,error_fragment",
    [
        (
            {"version": "v0", "request_id": "id", "success": True, "summary": {}},
            "Unexpected Isaac collider report version",
        ),
        (
            {
                "version": "manyforge.isaac.collider_report.v0.1",
                "request_id": "other",
                "success": True,
                "summary": {},
            },
            "Response request_id does not match",
        ),
        (
            {
                "version": "manyforge.isaac.collider_report.v0.1",
                "request_id": "id",
                "success": "true",
                "summary": {},
            },
            "success field must be boolean",
        ),
        (
            {
                "version": "manyforge.isaac.collider_report.v0.1",
                "request_id": "id",
                "success": False,
                "error": "boom",
                "summary": {},
            },
            "boom",
        ),
        (
            {
                "version": "manyforge.isaac.collider_report.v0.1",
                "request_id": "id",
                "success": True,
                "summary": {"collider_count": 1, "object_count": 1},
            },
            "summary.error_count must be an integer",
        ),
    ],
)
def test_validate_inspection_response_rejects_invalid_payloads(
    payload: dict[str, object],
    error_fragment: str,
) -> None:
    from manyforge_ros.inspect_isaac_scene import (
        IsaacSceneInspectionError,
        validate_inspection_response,
    )

    with pytest.raises(IsaacSceneInspectionError, match=error_fragment):
        validate_inspection_response(payload, "id")
