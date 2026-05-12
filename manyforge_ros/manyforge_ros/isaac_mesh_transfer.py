"""Client and CLI for transferring Isaac mesh artifacts over standard ROS topics."""

from __future__ import annotations

import argparse
import hashlib
import json
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


REQUEST_VERSION = "manyforge.isaac.mesh_transfer_request.v0.1"
STATUS_VERSION = "manyforge.isaac.mesh_transfer_status.v0.1"
CHUNK_VERSION = "manyforge.isaac.mesh_transfer_chunk.v0.1"
DEFAULT_REQUEST_TOPIC = "/manyforge/isaac_mesh_transfer/request"
DEFAULT_STATUS_TOPIC = "/manyforge/isaac_mesh_transfer/status"
DEFAULT_CHUNK_TOPIC = "/manyforge/isaac_mesh_transfer/chunk"


@dataclass(frozen=True)
class IsaacMeshTransferRequest:
    mesh_resource_uri: str | None = None
    source_root: str | None = None
    mesh_export_dir: str | None = None
    mesh_source_preference: str = "auto"
    include_hidden: bool = True
    purpose: tuple[str, ...] = ("all",)
    chunk_size: int = 262144
    request_id: str = field(default_factory=lambda: f"manyforge-mesh-{uuid.uuid4()}")

    def to_request_payload(self) -> dict[str, Any]:
        if bool(self.mesh_resource_uri) == bool(self.source_root):
            raise ValueError("Provide exactly one of mesh_resource_uri or source_root")
        payload: dict[str, Any] = {
            "version": REQUEST_VERSION,
            "request_id": self.request_id,
            "mesh_source_preference": self.mesh_source_preference,
            "include_hidden": self.include_hidden,
            "purpose": list(self.purpose),
            "chunk_size": self.chunk_size,
        }
        if self.mesh_resource_uri:
            payload["mesh_resource_uri"] = self.mesh_resource_uri
        if self.source_root:
            payload["source_root"] = self.source_root
        if self.mesh_export_dir:
            payload["mesh_export_dir"] = self.mesh_export_dir
        return payload


class IsaacMeshTransferError(RuntimeError):
    """Raised when an Isaac mesh transfer cannot complete correctly."""


class IsaacMeshTransferClient:
    """ROS topic client for the Isaac mesh-transfer protocol."""

    def __init__(
        self,
        node: object,
        *,
        request_topic: str = DEFAULT_REQUEST_TOPIC,
        status_topic: str = DEFAULT_STATUS_TOPIC,
        chunk_topic: str = DEFAULT_CHUNK_TOPIC,
        timeout_s: float = 30.0,
    ) -> None:
        self.node = node
        self.request_topic = request_topic
        self.status_topic = status_topic
        self.chunk_topic = chunk_topic
        self.timeout_s = timeout_s

        self._rclpy = _import_module("rclpy")
        messages = _import_module("std_msgs.msg")
        self._String = messages.String
        self._UInt8MultiArray = messages.UInt8MultiArray

    def transfer(
        self,
        request: IsaacMeshTransferRequest,
        *,
        output_dir: str | Path | None = None,
        output_path: str | Path | None = None,
    ) -> dict[str, Any]:
        if self.timeout_s <= 0:
            raise ValueError("timeout_s must be > 0")
        if output_dir is not None and output_path is not None:
            raise ValueError("Use output_dir or output_path, not both")

        statuses: list[dict[str, Any]] = []
        chunks: dict[int, tuple[dict[str, Any], bytes]] = {}
        parse_errors: list[str] = []
        completed_status: dict[str, Any] | None = None
        failed_status: dict[str, Any] | None = None

        def on_status(message: object) -> None:
            nonlocal completed_status, failed_status
            try:
                payload = json.loads(message.data)
            except Exception as exc:
                parse_errors.append(f"Status is not valid JSON: {exc}")
                return
            if not isinstance(payload, dict):
                parse_errors.append("Status JSON must be an object")
                return
            if payload.get("request_id") != request.request_id:
                return
            statuses.append(payload)
            if payload.get("success") is False or payload.get("phase") == "error":
                failed_status = payload
            elif payload.get("phase") == "completed":
                completed_status = payload

        def on_chunk(message: object) -> None:
            try:
                header, payload = decode_mesh_chunk_packet(bytes(message.data))
            except Exception as exc:
                parse_errors.append(f"Chunk is invalid: {exc}")
                return
            if header.get("request_id") != request.request_id:
                return
            chunk_index = header.get("chunk_index")
            if isinstance(chunk_index, int):
                chunks[chunk_index] = (header, payload)

        status_subscription = self.node.create_subscription(
            self._String,
            self.status_topic,
            on_status,
            10,
        )
        chunk_subscription = self.node.create_subscription(
            self._UInt8MultiArray,
            self.chunk_topic,
            on_chunk,
            10,
        )
        publisher = self.node.create_publisher(self._String, self.request_topic, 10)

        try:
            self._wait_for_request_subscriber(publisher)
            message = self._String()
            message.data = json.dumps(request.to_request_payload(), allow_nan=False)
            publisher.publish(message)

            deadline = time.monotonic() + self.timeout_s
            while time.monotonic() < deadline:
                if failed_status is not None:
                    raise IsaacMeshTransferError(
                        failed_status.get("error", "Isaac mesh transfer failed")
                    )
                if completed_status is not None:
                    expected_chunks = completed_status.get("chunk_count")
                    if isinstance(expected_chunks, int) and len(chunks) >= expected_chunks:
                        break
                self._spin_once(deadline)
            else:
                if parse_errors:
                    raise IsaacMeshTransferError(parse_errors[-1])
                raise TimeoutError(
                    "Timed out waiting for Isaac mesh transfer response "
                    f"with request_id={request.request_id!r}"
                )

            assert completed_status is not None
            assembled = _assemble_chunks(chunks, completed_status)
            target_path = _resolve_output_path(
                completed_status,
                output_dir=output_dir,
                output_path=output_path,
            )
            if target_path is not None:
                target_path.parent.mkdir(parents=True, exist_ok=True)
                target_path.write_bytes(assembled)
            return {
                **completed_status,
                "status_messages": statuses,
                "output_path": str(target_path) if target_path else None,
            }
        finally:
            self.node.destroy_publisher(publisher)
            self.node.destroy_subscription(status_subscription)
            self.node.destroy_subscription(chunk_subscription)

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


def encode_mesh_chunk_packet(header: dict[str, Any], payload: bytes) -> bytes:
    header_bytes = json.dumps(
        header,
        allow_nan=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    if len(header_bytes) > 65535:
        raise ValueError("Mesh chunk header is unexpectedly large")
    return len(header_bytes).to_bytes(4, "big") + header_bytes + payload


def decode_mesh_chunk_packet(packet: bytes) -> tuple[dict[str, Any], bytes]:
    if len(packet) < 4:
        raise ValueError("chunk packet is too short")
    header_size = int.from_bytes(packet[:4], "big")
    if header_size <= 0:
        raise ValueError("chunk packet header is empty")
    if len(packet) < 4 + header_size:
        raise ValueError("chunk packet is shorter than declared header")
    header = json.loads(packet[4 : 4 + header_size].decode("utf-8"))
    if not isinstance(header, dict):
        raise ValueError("chunk header must be a JSON object")
    if header.get("version") != CHUNK_VERSION:
        raise ValueError(f"Unexpected chunk version: {header.get('version')!r}")
    payload = packet[4 + header_size :]
    expected_size = header.get("data_size")
    if expected_size != len(payload):
        raise ValueError(
            f"chunk payload size mismatch: {expected_size!r} != {len(payload)}"
        )
    expected_sha = header.get("payload_sha256")
    actual_sha = hashlib.sha256(payload).hexdigest()
    if expected_sha != actual_sha:
        raise ValueError("chunk payload sha256 mismatch")
    return header, payload


def _assemble_chunks(
    chunks: dict[int, tuple[dict[str, Any], bytes]],
    completed_status: dict[str, Any],
) -> bytes:
    chunk_count = completed_status.get("chunk_count")
    if not isinstance(chunk_count, int) or chunk_count <= 0:
        raise IsaacMeshTransferError("completed status has invalid chunk_count")
    missing = [index for index in range(chunk_count) if index not in chunks]
    if missing:
        raise IsaacMeshTransferError(f"missing mesh transfer chunk(s): {missing}")
    assembled = b"".join(chunks[index][1] for index in range(chunk_count))
    expected_size = completed_status.get("total_bytes")
    if expected_size != len(assembled):
        raise IsaacMeshTransferError(
            f"assembled mesh size mismatch: {expected_size!r} != {len(assembled)}"
        )
    expected_sha = completed_status.get("sha256")
    actual_sha = hashlib.sha256(assembled).hexdigest()
    if expected_sha != actual_sha:
        raise IsaacMeshTransferError("assembled mesh sha256 mismatch")
    return assembled


def _resolve_output_path(
    completed_status: dict[str, Any],
    *,
    output_dir: str | Path | None,
    output_path: str | Path | None,
) -> Path | None:
    if output_path is not None:
        return Path(output_path)
    if output_dir is not None:
        filename = Path(str(completed_status.get("filename") or "isaac_mesh.obj")).name
        return Path(output_dir) / filename
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--mesh-resource-uri")
    source.add_argument("--source-root")
    parser.add_argument("--mesh-export-dir")
    parser.add_argument(
        "--mesh-source-preference",
        choices=["auto", "original", "collider", "primitive"],
        default="auto",
    )
    parser.add_argument("--include-hidden", action="store_true", default=True)
    parser.add_argument(
        "--purpose",
        action="append",
        default=["all"],
        choices=["default", "render", "proxy", "guide", "all"],
    )
    parser.add_argument("--chunk-size", type=int, default=262144)
    parser.add_argument("--request-topic", default=DEFAULT_REQUEST_TOPIC)
    parser.add_argument("--status-topic", default=DEFAULT_STATUS_TOPIC)
    parser.add_argument("--chunk-topic", default=DEFAULT_CHUNK_TOPIC)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--node-name", default="manyforge_isaac_mesh_transfer")
    parser.add_argument("--output-dir", default=".")
    parser.add_argument("--output-path")
    args = parser.parse_args()

    rclpy = _import_module("rclpy")
    rclpy.init(args=None)
    node = rclpy.create_node(args.node_name)
    try:
        request = IsaacMeshTransferRequest(
            mesh_resource_uri=args.mesh_resource_uri,
            source_root=args.source_root,
            mesh_export_dir=args.mesh_export_dir,
            mesh_source_preference=args.mesh_source_preference,
            include_hidden=args.include_hidden,
            purpose=tuple(args.purpose),
            chunk_size=args.chunk_size,
        )
        client = IsaacMeshTransferClient(
            node,
            request_topic=args.request_topic,
            status_topic=args.status_topic,
            chunk_topic=args.chunk_topic,
            timeout_s=args.timeout,
        )
        response = client.transfer(
            request,
            output_dir=None if args.output_path else args.output_dir,
            output_path=args.output_path,
        )
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
