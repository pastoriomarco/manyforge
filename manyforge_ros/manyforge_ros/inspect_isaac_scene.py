"""Client and CLI for the Isaac collider scene-inspection service.

This module is the deterministic tool boundary that a future assistant should
call.  It hides ROS transport details behind a versioned JSON request/response
contract and performs strict response validation before returning data.
"""

from __future__ import annotations

import argparse
import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from typing import Any


QUERY_VERSION = "manyforge.isaac.collider_query.v0.1"
REPORT_VERSION = "manyforge.isaac.collider_report.v0.1"
DEFAULT_REQUEST_TOPIC = "/manyforge/isaac_collider_query/request"
DEFAULT_RESPONSE_TOPIC = "/manyforge/isaac_collider_query/response"


@dataclass(frozen=True)
class IsaacSceneQuery:
    include_subtrees: tuple[str, ...] = ()
    exclude_subtrees: tuple[str, ...] = ()
    probe_prims: tuple[str, ...] = ()
    include_hidden: bool = False
    include_cooked_convex: bool = True
    include_cooked_samples: bool = False
    max_colliders: int | None = None
    purpose: tuple[str, ...] = ()
    output_profile: str = "llm_scene_authoring"
    request_id: str = field(default_factory=lambda: f"manyforge-{uuid.uuid4()}")

    def to_request_payload(self) -> dict[str, Any]:
        payload: dict[str, Any] = {
            "version": QUERY_VERSION,
            "request_id": self.request_id,
            "include_subtrees": list(self.include_subtrees),
            "exclude_subtrees": list(self.exclude_subtrees),
            "probe_prims": list(self.probe_prims),
            "include_hidden": self.include_hidden,
            "include_cooked_convex": self.include_cooked_convex,
            "include_cooked_samples": self.include_cooked_samples,
            "output_profile": self.output_profile,
            "purpose": list(self.purpose),
        }
        if self.max_colliders is not None:
            payload["max_colliders"] = self.max_colliders
        return payload


class IsaacSceneInspectionError(RuntimeError):
    """Raised when the Isaac scene inspection request cannot be completed."""


class InspectIsaacSceneClient:
    """ROS client for the ManyForge Isaac collider service topic RPC."""

    def __init__(
        self,
        node: object,
        *,
        request_topic: str = DEFAULT_REQUEST_TOPIC,
        response_topic: str = DEFAULT_RESPONSE_TOPIC,
        timeout_s: float = 10.0,
    ) -> None:
        self.node = node
        self.request_topic = request_topic
        self.response_topic = response_topic
        self.timeout_s = timeout_s

        self._rclpy = _import_module("rclpy")
        self._String = _import_module("std_msgs.msg").String

    def inspect(self, query: IsaacSceneQuery) -> dict[str, Any]:
        if self.timeout_s <= 0:
            raise ValueError("timeout_s must be > 0")

        responses: list[dict[str, Any]] = []
        parse_errors: list[str] = []

        def on_response(message: object) -> None:
            try:
                payload = json.loads(message.data)
            except Exception as exc:
                parse_errors.append(f"Response is not valid JSON: {exc}")
                return
            if not isinstance(payload, dict):
                parse_errors.append("Response JSON must be an object")
                return
            if payload.get("request_id") == query.request_id:
                responses.append(payload)

        subscription = self.node.create_subscription(
            self._String,
            self.response_topic,
            on_response,
            10,
        )
        publisher = self.node.create_publisher(self._String, self.request_topic, 10)

        try:
            self._wait_for_request_subscriber(publisher)
            message = self._String()
            message.data = json.dumps(query.to_request_payload(), allow_nan=False)
            publisher.publish(message)

            deadline = time.monotonic() + self.timeout_s
            while time.monotonic() < deadline and not responses:
                self._spin_once(deadline)

            if responses:
                return validate_inspection_response(responses[0], query.request_id)
            if parse_errors:
                raise IsaacSceneInspectionError(parse_errors[-1])
            raise TimeoutError(
                "Timed out waiting for Isaac collider response "
                f"with request_id={query.request_id!r}"
            )
        finally:
            self.node.destroy_publisher(publisher)
            self.node.destroy_subscription(subscription)

    def _wait_for_request_subscriber(self, publisher: object) -> None:
        deadline = time.monotonic() + self.timeout_s
        while time.monotonic() < deadline:
            if publisher.get_subscription_count() > 0:
                return
            self._spin_once(deadline)
        raise TimeoutError(
            f"Timed out waiting for subscriber on request topic {self.request_topic}"
        )

    def _spin_once(self, deadline: float) -> None:
        executor = getattr(self.node, "executor", None)
        if executor is None or not getattr(executor, "is_spinning", False):
            self._rclpy.spin_once(
                self.node,
                timeout_sec=max(0.0, min(0.1, deadline - time.monotonic())),
            )
        else:
            time.sleep(0.01)


def validate_inspection_response(
    payload: dict[str, Any],
    expected_request_id: str,
) -> dict[str, Any]:
    if payload.get("version") != REPORT_VERSION:
        raise IsaacSceneInspectionError(
            f"Unexpected Isaac collider report version: {payload.get('version')!r}"
        )
    if payload.get("request_id") != expected_request_id:
        raise IsaacSceneInspectionError(
            "Response request_id does not match request: "
            f"{payload.get('request_id')!r} != {expected_request_id!r}"
        )
    if not isinstance(payload.get("success"), bool):
        raise IsaacSceneInspectionError("Response success field must be boolean")
    if not payload["success"]:
        raise IsaacSceneInspectionError(
            f"Isaac collider inspection failed: {payload.get('error', 'unknown error')}"
        )
    summary = payload.get("summary")
    if not isinstance(summary, dict):
        raise IsaacSceneInspectionError("Response summary must be an object")
    for key in ("collider_count", "object_count", "error_count"):
        if key not in summary or isinstance(summary[key], bool) or not isinstance(
            summary[key],
            int,
        ):
            raise IsaacSceneInspectionError(f"Response summary.{key} must be an integer")
    objects = payload.get("objects")
    if objects is not None and not isinstance(objects, list):
        raise IsaacSceneInspectionError("Response objects field must be a list")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--include-subtree",
        action="append",
        default=[],
        help="Isaac prim subtree to include; may be repeated.",
    )
    parser.add_argument(
        "--exclude-subtree",
        action="append",
        default=[],
        help="Isaac prim subtree to exclude; may be repeated.",
    )
    parser.add_argument(
        "--probe-prim",
        action="append",
        default=[],
        help="Non-collider prim to inspect for transform/bounds; may be repeated.",
    )
    parser.add_argument("--include-hidden", action="store_true")
    parser.add_argument(
        "--no-cooked-convex",
        action="store_false",
        dest="include_cooked_convex",
    )
    parser.set_defaults(include_cooked_convex=True)
    parser.add_argument("--include-cooked-samples", action="store_true")
    parser.add_argument("--max-colliders", type=int, default=None)
    parser.add_argument(
        "--purpose",
        action="append",
        default=[],
        choices=["default", "render", "proxy", "guide", "all"],
    )
    parser.add_argument(
        "--output-profile",
        default="llm_scene_authoring",
        choices=["llm_scene_authoring", "raw_inventory"],
    )
    parser.add_argument("--request-topic", default=DEFAULT_REQUEST_TOPIC)
    parser.add_argument("--response-topic", default=DEFAULT_RESPONSE_TOPIC)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--node-name", default="manyforge_inspect_isaac_scene")
    args = parser.parse_args()

    rclpy = _import_module("rclpy")
    rclpy.init(args=None)
    node = rclpy.create_node(args.node_name)
    try:
        query = IsaacSceneQuery(
            include_subtrees=tuple(args.include_subtree),
            exclude_subtrees=tuple(args.exclude_subtree),
            probe_prims=tuple(args.probe_prim),
            include_hidden=args.include_hidden,
            include_cooked_convex=args.include_cooked_convex,
            include_cooked_samples=args.include_cooked_samples,
            max_colliders=args.max_colliders,
            purpose=tuple(args.purpose),
            output_profile=args.output_profile,
        )
        client = InspectIsaacSceneClient(
            node,
            request_topic=args.request_topic,
            response_topic=args.response_topic,
            timeout_s=args.timeout,
        )
        response = client.inspect(query)
        print(json.dumps(_json_safe(response), indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        print(json.dumps({"success": False, "error": str(exc)}, sort_keys=True))
        return 2
    finally:
        node.destroy_node()
        rclpy.shutdown()


def _json_safe(value: Any) -> Any:
    if hasattr(value, "__dataclass_fields__"):
        return _json_safe(asdict(value))
    if isinstance(value, dict):
        return {str(key): _json_safe(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_safe(item) for item in value]
    return value


def _import_module(module_name: str) -> Any:
    import importlib

    return importlib.import_module(module_name)


if __name__ == "__main__":
    raise SystemExit(main())
