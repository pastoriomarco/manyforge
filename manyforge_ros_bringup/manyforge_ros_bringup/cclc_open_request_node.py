"""ROS 2 service adapter for the sole ROS-independent CCLC owner."""

from __future__ import annotations

from pathlib import Path
from typing import Sequence

import rclpy
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node

from manyforge_cclc_msgs.srv import CCLCOpenRequest

from .cclc_open_request_transport import (
    CCLCOpenUnixClient,
    handle_open_request,
)


class CCLCOpenRequestNode(Node):
    """Bounded request/status service with no client-side transaction state."""

    def __init__(self, *, run_root: Path | None = None) -> None:
        super().__init__("manyforge_cclc_open_request")
        if run_root is None:
            self.declare_parameter("run_root", "")
            configured = self.get_parameter("run_root").value
            if not isinstance(configured, str) or not configured:
                raise RuntimeError("run_root must name the active CCLC run root")
            run_root = Path(configured)
        self._client = CCLCOpenUnixClient(run_root)
        self._service = self.create_service(
            CCLCOpenRequest,
            "~/open_request",
            self._on_request,
            callback_group=ReentrantCallbackGroup(),
        )

    def _on_request(
        self,
        request: CCLCOpenRequest.Request,
        response: CCLCOpenRequest.Response,
    ) -> CCLCOpenRequest.Response:
        result = handle_open_request(self._client, request.canonical_request_json)
        response.completed = result.completed
        response.canonical_response_json = result.canonical_response_json
        response.error_code = result.error_code
        response.error_message = result.error_message
        return response


def main(args: Sequence[str] | None = None) -> int:
    rclpy.init(args=args)
    node: CCLCOpenRequestNode | None = None
    executor: MultiThreadedExecutor | None = None
    try:
        node = CCLCOpenRequestNode()
        executor = MultiThreadedExecutor(num_threads=4)
        executor.add_node(node)
        executor.spin()
        return 0
    finally:
        if executor is not None:
            executor.shutdown()
        if node is not None:
            node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
