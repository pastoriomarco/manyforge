"""Isaac Sim extension exposing ManyForge collider reports over ROS 2.

The extension deliberately avoids custom ROS interfaces for the first spike:

- `/manyforge/isaac_collider_report` uses `std_srvs/srv/Trigger` and returns
  the configured default query response in `message`.
- `/manyforge/isaac_collider_query/request` and
  `/manyforge/isaac_collider_query/response` use `std_msgs/msg/String` for
  JSON request/response RPC with an optional `request_id`.

USD/PhysX scene access is executed on the Isaac/Kit update thread. ROS callbacks
only enqueue requests and wait for the main-thread capture result.
"""

from __future__ import annotations

import concurrent.futures
import json
import os
import queue
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import carb
import carb.settings
import omni.ext
import omni.kit.app

from .query import ColliderQuery, parse_query, request_id_from_text


EXTENSION_ID = "manyforge.isaac.collider_service"
DEFAULT_TIMEOUT_S = 30.0


@dataclass
class _QueuedRequest:
    query: ColliderQuery
    future: concurrent.futures.Future[dict[str, Any]]


class ManyForgeIsaacColliderServiceExtension(omni.ext.IExt):
    """Native Isaac extension wrapper around the collider probe library."""

    def on_startup(self, ext_id: str) -> None:
        self._ext_id = ext_id
        self._settings = carb.settings.get_settings()
        self._request_queue: "queue.Queue[_QueuedRequest]" = queue.Queue()
        self._ros_ready = False
        self._shutdown = False
        self._node = None
        self._executor = None
        self._spin_thread: threading.Thread | None = None
        self._trigger_service = None
        self._request_subscription = None
        self._response_publisher = None
        self._update_subscription = None

        try:
            self._probe_lib = self._load_probe_lib()
        except Exception as exc:
            carb.log_error(f"[ManyForge Collider Service] Failed to load probe library: {exc}")
            return

        try:
            self._start_ros()
            self._update_subscription = (
                omni.kit.app.get_app()
                .get_update_event_stream()
                .create_subscription_to_pop(
                    self._on_update,
                    name="manyforge_isaac_collider_service_update",
                )
            )
            self._ros_ready = True
            carb.log_info("[ManyForge Collider Service] Started")
        except Exception as exc:
            carb.log_error(f"[ManyForge Collider Service] Startup failed: {exc}")
            self._stop_ros()

    def on_shutdown(self) -> None:
        self._shutdown = True
        self._update_subscription = None
        self._stop_ros()
        carb.log_info("[ManyForge Collider Service] Stopped")

    def _load_probe_lib(self) -> Any:
        extension_path = Path(__file__).resolve()
        extension_root = extension_path.parents[3]
        search_root = extension_path.parents[5]
        repo_root = search_root
        candidate_dirs = [
            Path(value)
            for value in [
                os.environ.get("MANYFORGE_PROBE_SCRIPTS_DIR"),
                self._setting("probe_scripts_dir", None),
            ]
            if value
        ]
        candidate_dirs.extend(
            [
                extension_path.parent,
                extension_root / "scripts",
                search_root / "scripts",
                repo_root / "scripts",
            ]
        )

        scripts_dir = next(
            (path for path in candidate_dirs if (path / "isaac_collider_probe_lib.py").exists()),
            None,
        )
        if scripts_dir is None:
            formatted = ", ".join(str(path) for path in candidate_dirs)
            raise FileNotFoundError(
                "Could not find isaac_collider_probe_lib.py in candidate paths: "
                f"{formatted}"
            )
        if str(scripts_dir) not in sys.path:
            sys.path.insert(0, str(scripts_dir))

        import isaac_collider_probe_lib

        return isaac_collider_probe_lib

    def _start_ros(self) -> None:
        import rclpy
        from rclpy.executors import MultiThreadedExecutor
        from std_msgs.msg import String
        from std_srvs.srv import Trigger

        self._rclpy = rclpy
        self._String = String
        self._Trigger = Trigger

        if not rclpy.ok():
            rclpy.init(args=None)

        node_name = self._setting("node_name", "manyforge_isaac_collider_service")
        trigger_service_name = self._setting(
            "trigger_service_name",
            "/manyforge/isaac_collider_report",
        )
        request_topic_name = self._setting(
            "request_topic_name",
            "/manyforge/isaac_collider_query/request",
        )
        response_topic_name = self._setting(
            "response_topic_name",
            "/manyforge/isaac_collider_query/response",
        )

        self._node = rclpy.create_node(node_name)
        self._trigger_service = self._node.create_service(
            Trigger,
            trigger_service_name,
            self._on_trigger_request,
        )
        self._request_subscription = self._node.create_subscription(
            String,
            request_topic_name,
            self._on_string_request,
            10,
        )
        self._response_publisher = self._node.create_publisher(
            String,
            response_topic_name,
            10,
        )

        self._executor = MultiThreadedExecutor(num_threads=2)
        self._executor.add_node(self._node)
        self._spin_thread = threading.Thread(
            target=self._executor.spin,
            name="manyforge_isaac_collider_service_ros_spin",
            daemon=True,
        )
        self._spin_thread.start()

        carb.log_info(
            "[ManyForge Collider Service] ROS endpoints: "
            f"service={trigger_service_name}, request={request_topic_name}, "
            f"response={response_topic_name}"
        )

    def _stop_ros(self) -> None:
        if self._executor is not None:
            try:
                self._executor.shutdown()
            except Exception:
                pass
        if self._spin_thread is not None:
            self._spin_thread.join(timeout=2.0)
        if self._node is not None:
            try:
                self._node.destroy_node()
            except Exception:
                pass

        self._node = None
        self._executor = None
        self._spin_thread = None

    def _on_trigger_request(self, _request: Any, response: Any) -> Any:
        query_text = self._setting("default_query", "{}")
        try:
            query = parse_query(query_text)
            payload = self._run_query_sync(query)
            response.success = bool(payload.get("success", False))
            response.message = json.dumps(payload, allow_nan=False, sort_keys=True)
        except Exception as exc:
            response.success = False
            response.message = json.dumps(
                {
                    "version": "manyforge.isaac.collider_response.v0.1",
                    "success": False,
                    "error": str(exc),
                },
                allow_nan=False,
                sort_keys=True,
            )
        return response

    def _on_string_request(self, message: Any) -> None:
        try:
            query = parse_query(message.data)
            payload = self._run_query_sync(query)
        except Exception as exc:
            request_id = request_id_from_text(message.data)
            payload = {
                "version": "manyforge.isaac.collider_response.v0.1",
                "request_id": request_id,
                "success": False,
                "error": str(exc),
            }

        response = self._String()
        response.data = json.dumps(payload, allow_nan=False, sort_keys=True)
        self._response_publisher.publish(response)

    def _run_query_sync(self, query: ColliderQuery) -> dict[str, Any]:
        future: concurrent.futures.Future[dict[str, Any]] = concurrent.futures.Future()
        self._request_queue.put(_QueuedRequest(query=query, future=future))
        timeout_s = self._request_timeout_s()
        try:
            return future.result(timeout=timeout_s)
        except concurrent.futures.TimeoutError as exc:
            future.cancel()
            raise TimeoutError(
                f"Collider query timed out after {timeout_s:.3f} seconds"
            ) from exc

    def _on_update(self, _event: Any) -> None:
        processed = 0
        while processed < 4:
            try:
                item = self._request_queue.get_nowait()
            except queue.Empty:
                return
            processed += 1
            if item.future.cancelled():
                continue
            try:
                item.future.set_result(self._capture_payload(item.query))
            except Exception as exc:
                item.future.set_exception(exc)

    def _capture_payload(self, query: ColliderQuery) -> dict[str, Any]:
        options = self._probe_lib.ProbeOptions(
            include_subtree=query.include_subtrees,
            exclude_subtree=query.exclude_subtrees,
            max_colliders=query.max_colliders,
            cooked_convex=query.include_cooked_convex,
            include_cooked_samples=query.include_cooked_samples,
            include_hidden=query.include_hidden,
            purpose=query.purpose,
            probe_prim=query.probe_prims,
        )
        report = self._probe_lib.capture_probe_report(options)
        payload = self._probe_lib._profile_payload(report, query.output_profile)
        payload["request_id"] = query.request_id
        return self._probe_lib._json_safe(payload)

    def _setting(self, key: str, default: Any) -> Any:
        path = f"/exts/{EXTENSION_ID}/{key}"
        value = self._settings.get(path)
        return default if value is None else value

    def _request_timeout_s(self) -> float:
        raw = self._setting("request_timeout_s", DEFAULT_TIMEOUT_S)
        if isinstance(raw, bool):
            return DEFAULT_TIMEOUT_S
        try:
            timeout = float(raw)
        except (TypeError, ValueError):
            return DEFAULT_TIMEOUT_S
        return timeout if timeout > 0 else DEFAULT_TIMEOUT_S
