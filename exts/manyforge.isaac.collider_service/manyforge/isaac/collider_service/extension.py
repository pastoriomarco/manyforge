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
import hashlib
import json
import os
import queue
import sys
import threading
import tempfile
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse

import carb
import carb.settings
import omni.ext
import omni.kit.app

from .ros_contract import (
    ColliderQuery,
    MeshTransferRequest,
    parse_mesh_transfer_request,
    parse_query,
    request_id_from_text,
)


EXTENSION_ID = "manyforge.isaac.collider_service"
DEFAULT_TIMEOUT_S = 30.0
EXTENSION_BUILD_MARKER = "manyforge-temp-descendant-mesh-recook-2026-05-11"
MESH_TRANSFER_STATUS_VERSION = "manyforge.isaac.mesh_transfer_status.v0.1"
MESH_TRANSFER_CHUNK_VERSION = "manyforge.isaac.mesh_transfer_chunk.v0.1"


@dataclass
class _QueuedRequest:
    query: ColliderQuery
    future: concurrent.futures.Future[dict[str, Any]]


@dataclass
class _QueuedMeshTransfer:
    request: MeshTransferRequest
    future: concurrent.futures.Future[dict[str, Any]]


class ManyForgeIsaacColliderServiceExtension(omni.ext.IExt):
    """Native Isaac extension wrapper around the collider probe library."""

    def on_startup(self, ext_id: str) -> None:
        self._ext_id = ext_id
        self._settings = carb.settings.get_settings()
        self._request_queue: "queue.Queue[_QueuedRequest]" = queue.Queue()
        self._mesh_transfer_queue: "queue.Queue[_QueuedMeshTransfer]" = queue.Queue()
        self._ros_ready = False
        self._shutdown = False
        self._node = None
        self._executor = None
        self._spin_thread: threading.Thread | None = None
        self._trigger_service = None
        self._request_subscription = None
        self._response_publisher = None
        self._mesh_transfer_subscription = None
        self._mesh_status_publisher = None
        self._mesh_chunk_publisher = None
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
        from std_msgs.msg import String, UInt8MultiArray
        from std_srvs.srv import Trigger

        self._rclpy = rclpy
        self._String = String
        self._UInt8MultiArray = UInt8MultiArray
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
        mesh_transfer_request_topic_name = self._setting(
            "mesh_transfer_request_topic_name",
            "/manyforge/isaac_mesh_transfer/request",
        )
        mesh_transfer_status_topic_name = self._setting(
            "mesh_transfer_status_topic_name",
            "/manyforge/isaac_mesh_transfer/status",
        )
        mesh_transfer_chunk_topic_name = self._setting(
            "mesh_transfer_chunk_topic_name",
            "/manyforge/isaac_mesh_transfer/chunk",
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
        self._mesh_transfer_subscription = self._node.create_subscription(
            String,
            mesh_transfer_request_topic_name,
            self._on_mesh_transfer_request,
            10,
        )
        self._mesh_status_publisher = self._node.create_publisher(
            String,
            mesh_transfer_status_topic_name,
            10,
        )
        self._mesh_chunk_publisher = self._node.create_publisher(
            UInt8MultiArray,
            mesh_transfer_chunk_topic_name,
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
            f"response={response_topic_name}, "
            f"mesh_request={mesh_transfer_request_topic_name}, "
            f"mesh_status={mesh_transfer_status_topic_name}, "
            f"mesh_chunk={mesh_transfer_chunk_topic_name}"
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

    def _on_mesh_transfer_request(self, message: Any) -> None:
        try:
            request = parse_mesh_transfer_request(message.data)
            payload = self._run_mesh_transfer_sync(request)
            self._publish_mesh_transfer_status(
                {
                    "version": MESH_TRANSFER_STATUS_VERSION,
                    "request_id": payload["request_id"],
                    "phase": "started",
                    "success": True,
                    "filename": payload["filename"],
                    "total_bytes": payload["total_bytes"],
                    "chunk_count": payload["chunk_count"],
                    "sha256": payload["sha256"],
                    "mesh_resource_uri": payload["mesh_resource_uri"],
                    "source_root": payload.get("source_root"),
                    "mesh_source": payload.get("mesh_source"),
                    "mesh_source_prim_path": payload.get("mesh_source_prim_path"),
                }
            )
            self._publish_mesh_transfer_chunks(payload)
            self._publish_mesh_transfer_status(
                {
                    "version": MESH_TRANSFER_STATUS_VERSION,
                    "request_id": payload["request_id"],
                    "phase": "completed",
                    "success": True,
                    "filename": payload["filename"],
                    "total_bytes": payload["total_bytes"],
                    "chunk_count": payload["chunk_count"],
                    "sha256": payload["sha256"],
                    "mesh_resource_uri": payload["mesh_resource_uri"],
                    "source_root": payload.get("source_root"),
                    "mesh_source": payload.get("mesh_source"),
                    "mesh_source_prim_path": payload.get("mesh_source_prim_path"),
                }
            )
        except Exception as exc:
            request_id = request_id_from_text(message.data)
            self._publish_mesh_transfer_status(
                {
                    "version": MESH_TRANSFER_STATUS_VERSION,
                    "request_id": request_id,
                    "phase": "error",
                    "success": False,
                    "error": str(exc),
                }
            )

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

    def _run_mesh_transfer_sync(self, request: MeshTransferRequest) -> dict[str, Any]:
        future: concurrent.futures.Future[dict[str, Any]] = concurrent.futures.Future()
        self._mesh_transfer_queue.put(_QueuedMeshTransfer(request=request, future=future))
        timeout_s = self._request_timeout_s()
        try:
            return future.result(timeout=timeout_s)
        except concurrent.futures.TimeoutError as exc:
            future.cancel()
            raise TimeoutError(
                f"Mesh transfer request timed out after {timeout_s:.3f} seconds"
            ) from exc

    def _on_update(self, _event: Any) -> None:
        processed = 0
        while processed < 4:
            kind = ""
            try:
                item = self._request_queue.get_nowait()
            except queue.Empty:
                try:
                    item = self._mesh_transfer_queue.get_nowait()
                    kind = "mesh_transfer"
                except queue.Empty:
                    return
            else:
                kind = "query"
            processed += 1
            if item.future.cancelled():
                continue
            try:
                if kind == "query":
                    item.future.set_result(self._capture_payload(item.query))
                else:
                    item.future.set_result(self._capture_mesh_transfer(item.request))
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
            mesh_export_dir=query.mesh_export_dir,
            mesh_source_preference=query.mesh_source_preference,
        )
        report = self._probe_lib.capture_probe_report(options)
        payload = self._probe_lib._profile_payload(report, query.output_profile)
        payload["request_id"] = query.request_id
        payload["manyforge_extension_build_marker"] = EXTENSION_BUILD_MARKER
        payload["manyforge_probe_module_path"] = str(
            Path(getattr(self._probe_lib, "__file__", "")).resolve()
        )
        return self._probe_lib._json_safe(payload)

    def _capture_mesh_transfer(self, request: MeshTransferRequest) -> dict[str, Any]:
        request_id = request.request_id or f"manyforge-mesh-{uuid.uuid4()}"
        mesh_resource_uri = request.mesh_resource_uri
        source_root = request.source_root
        mesh_source = None
        mesh_source_prim_path = None

        if source_root:
            export_dir = request.mesh_export_dir or tempfile.mkdtemp(
                prefix="manyforge-isaac-mesh-transfer-"
            )
            options = self._probe_lib.ProbeOptions(
                include_subtree=(source_root,),
                exclude_subtree=(),
                max_colliders=None,
                cooked_convex=True,
                include_cooked_samples=False,
                include_hidden=request.include_hidden,
                purpose=tuple(request.purpose),
                probe_prim=(),
                mesh_export_dir=export_dir,
                mesh_source_preference=request.mesh_source_preference,
            )
            report = self._probe_lib.capture_probe_report(options)
            profile = self._probe_lib._profile_payload(report, "llm_scene_authoring")
            object_by_root = {
                item.get("source_root"): item
                for item in profile.get("objects", [])
                if isinstance(item, dict)
            }
            scene_object = object_by_root.get(source_root)
            if scene_object is None:
                raise ValueError(f"No Isaac scene object found for source_root={source_root!r}")
            recommendation = scene_object.get("recommended_manyforge_shape") or {}
            shape = recommendation.get("shape") or {}
            mesh_resource_uri = shape.get("mesh_resource_uri")
            if not mesh_resource_uri:
                raise ValueError(
                    "Selected Isaac object did not produce a transferable mesh; "
                    f"recommended shape was {shape.get('type')!r}"
                )
            mesh_source = recommendation.get("mesh_source")
            mesh_source_prim_path = recommendation.get("mesh_source_prim_path")

        if not mesh_resource_uri:
            raise ValueError("No mesh_resource_uri was selected for transfer")

        mesh_path = _local_path_from_mesh_uri(mesh_resource_uri)
        mesh_bytes = mesh_path.read_bytes()
        digest = hashlib.sha256(mesh_bytes).hexdigest()
        chunk_size = request.chunk_size
        chunk_count = max(1, (len(mesh_bytes) + chunk_size - 1) // chunk_size)

        return {
            "request_id": request_id,
            "filename": mesh_path.name,
            "mesh_resource_uri": mesh_resource_uri,
            "source_root": source_root,
            "mesh_source": mesh_source,
            "mesh_source_prim_path": mesh_source_prim_path,
            "data": mesh_bytes,
            "total_bytes": len(mesh_bytes),
            "chunk_size": chunk_size,
            "chunk_count": chunk_count,
            "sha256": digest,
        }

    def _publish_mesh_transfer_status(self, payload: dict[str, Any]) -> None:
        message = self._String()
        message.data = json.dumps(payload, allow_nan=False, sort_keys=True)
        self._mesh_status_publisher.publish(message)

    def _publish_mesh_transfer_chunks(self, payload: dict[str, Any]) -> None:
        data = payload["data"]
        chunk_size = payload["chunk_size"]
        chunk_count = payload["chunk_count"]
        for index in range(chunk_count):
            offset = index * chunk_size
            chunk = data[offset : offset + chunk_size]
            header = {
                "version": MESH_TRANSFER_CHUNK_VERSION,
                "request_id": payload["request_id"],
                "chunk_index": index,
                "chunk_count": chunk_count,
                "offset": offset,
                "data_size": len(chunk),
                "total_bytes": payload["total_bytes"],
                "sha256": payload["sha256"],
                "payload_sha256": hashlib.sha256(chunk).hexdigest(),
                "filename": payload["filename"],
                "mesh_resource_uri": payload["mesh_resource_uri"],
                "source_root": payload.get("source_root"),
                "mesh_source": payload.get("mesh_source"),
                "mesh_source_prim_path": payload.get("mesh_source_prim_path"),
            }
            packet = _encode_mesh_chunk(header, chunk)
            message = self._UInt8MultiArray()
            message.data = list(packet)
            self._mesh_chunk_publisher.publish(message)

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


def _local_path_from_mesh_uri(mesh_resource_uri: str) -> Path:
    parsed = urlparse(mesh_resource_uri)
    if parsed.scheme in {"", "file"}:
        if parsed.scheme == "file" and parsed.netloc not in {"", "localhost"}:
            raise ValueError(
                "Only local file:// mesh_resource_uri values are transferable over ROS"
            )
        raw_path = parsed.path if parsed.scheme else mesh_resource_uri
        path = Path(unquote(raw_path)).expanduser()
        if not path.is_file():
            raise FileNotFoundError(f"Mesh resource does not exist: {path}")
        return path
    raise ValueError(
        "Only local file:// or plain-path mesh_resource_uri values are supported "
        f"for ROS transfer, got {mesh_resource_uri!r}"
    )


def _encode_mesh_chunk(header: dict[str, Any], payload: bytes) -> bytes:
    header_bytes = json.dumps(
        header,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    if len(header_bytes) > 65535:
        raise ValueError("Mesh chunk header is unexpectedly large")
    return len(header_bytes).to_bytes(4, "big") + header_bytes + payload
