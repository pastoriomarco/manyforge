"""Demand-driven PointCloud2 to cell-frame MFOB bridge of a point-cloud source.

The process of one `pointcloud_source` component (adapter family `pointcloud`),
started by the execution host at Prepare with the component's settings. While
the adapter has switched forwarding on (`forwarding`, latched Bool) and a
Composer viewer wants the cloud, it subscribes to the PointCloud2 topic, bounds
each cloud and posts it to the Composer
(`/api/scene/pointcloud/observations`). Display only.

For the adapter it publishes `watching` (latched Bool: subscribed to the cloud
topic) and `received` (the header of every received cloud), which give the
component's `pointcloud` stream freshness. The Composer base URL comes from
`MANYFORGE_COMPOSER_URL` and the owner id from `MANYFORGE_POINTCLOUD_OWNER_ID`
(default `POINTCLOUD_DEFAULT_OWNER_ID`), the same configuration the Composer
reads.
"""

from __future__ import annotations

import json
import math
import os
import struct
import threading
import time
from typing import Final
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen
import uuid

import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import PointCloud2, PointField
from std_msgs.msg import Bool, Header

from manyforge_headless_api.pointcloud_observation import (
    POINTCLOUD_DEFAULT_OWNER_ID,
    POINTCLOUD_MEDIA_TYPE,
    POINTCLOUD_RECEIPT_SCHEMA,
    POINTCLOUD_STATUS_SCHEMA,
    encode_pointcloud_observation,
)


_MAX_STATUS_BYTES: Final = 64 * 1024
_MAX_RECEIPT_BYTES: Final = 16 * 1024
_MAX_SOURCE_BYTES: Final = 64 * 1024 * 1024
_COMPOSER_URL_ENV: Final = "MANYFORGE_COMPOSER_URL"
_OWNER_ID_ENV: Final = "MANYFORGE_POINTCLOUD_OWNER_ID"
_LATCHED: Final = QoSProfile(
    history=HistoryPolicy.KEEP_LAST,
    depth=1,
    reliability=ReliabilityPolicy.RELIABLE,
    durability=DurabilityPolicy.TRANSIENT_LOCAL,
)


def _bounded_text(value: object, label: str, maximum: int) -> str:
    if type(value) is not str or value != value.strip() or not value:
        raise ValueError(f"{label} must be a non-empty string")
    if len(value.encode("utf-8", errors="strict")) > maximum:
        raise ValueError(f"{label} exceeds its {maximum}-byte bound")
    return value


def _composer_url(value: str) -> str:
    normalized = _bounded_text(value, "composer_base_url", 2_048).rstrip("/")
    parsed = urlsplit(normalized)
    if (
        parsed.scheme not in {"http", "https"}
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("composer_base_url must be one HTTP(S) origin/path")
    return normalized


def _observation_identity(value: object, label: str, maximum: int) -> str:
    normalized = _bounded_text(value, label, maximum)
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-"
    if (
        normalized[0]
        not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        or any(character not in allowed for character in normalized)
    ):
        raise ValueError(f"{label} must be one bounded observation identity")
    return normalized


def _field_map(message: PointCloud2) -> dict[str, PointField]:
    result: dict[str, PointField] = {}
    for field in message.fields:
        if field.name in result:
            raise ValueError(f"PointCloud2 repeats field {field.name!r}")
        result[field.name] = field
    return result


def _xyz_field(fields: dict[str, PointField], name: str, point_step: int) -> PointField:
    field = fields.get(name)
    if (
        field is None
        or field.datatype != PointField.FLOAT32
        or field.count != 1
        or field.offset < 0
        or field.offset + 4 > point_step
    ):
        raise ValueError(f"PointCloud2 {name!r} must be one contained FLOAT32")
    return field


def _rgb_reader(
    fields: dict[str, PointField],
    point_step: int,
    big_endian: bool,
):
    individual = tuple(fields.get(name) for name in ("r", "g", "b"))
    if any(field is not None for field in individual) and not all(
        field is not None for field in individual
    ):
        raise ValueError("PointCloud2 must provide all or none of r/g/b")
    if all(field is not None for field in individual):
        channels = individual  # type: ignore[assignment]
        if any(
            field.datatype != PointField.UINT8
            or field.count != 1
            or field.offset < 0
            or field.offset + 1 > point_step
            for field in channels
        ):
            raise ValueError("PointCloud2 r/g/b fields must be contained UINT8")

        def read_channels(data: memoryview, offset: int) -> tuple[int, int, int]:
            return tuple(  # type: ignore[return-value]
                data[offset + field.offset] for field in channels
            )

        return read_channels
    packed = fields.get("rgb") or fields.get("rgba")
    if packed is None:
        return None
    if (
        packed.datatype not in {PointField.FLOAT32, PointField.UINT32}
        or packed.count != 1
        or packed.offset < 0
        or packed.offset + 4 > point_step
    ):
        raise ValueError("PointCloud2 rgb/rgba must be one contained 32-bit field")
    byte_order = "big" if big_endian else "little"

    def read_packed(data: memoryview, offset: int) -> tuple[int, int, int]:
        value = int.from_bytes(
            data[offset + packed.offset : offset + packed.offset + 4],
            byteorder=byte_order,
            signed=False,
        )
        return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)

    return read_packed


def pointcloud2_samples(
    message: PointCloud2,
    *,
    expected_frame_id: str,
    max_points: int,
) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]] | None]:
    """Validate, boundedly sample, and compact one PointCloud2 message."""
    if message.header.frame_id != expected_frame_id:
        raise ValueError("PointCloud2 frame does not match the configured cell frame")
    if type(max_points) is not int or max_points < 1 or max_points > 200_000:
        raise ValueError("max_points is outside the qualified range")
    width = int(message.width)
    height = int(message.height)
    point_step = int(message.point_step)
    row_step = int(message.row_step)
    if (
        width < 1
        or height < 1
        or point_step < 12
        or point_step > 256
        or row_step < width * point_step
        or len(message.data) != row_step * height
        or len(message.data) > _MAX_SOURCE_BYTES
    ):
        raise ValueError("PointCloud2 dimensions or storage shape are invalid")
    total = width * height
    if total > 20_000_000:
        raise ValueError("PointCloud2 source point bound is exceeded")
    fields = _field_map(message)
    xyz = tuple(_xyz_field(fields, name, point_step) for name in ("x", "y", "z"))
    color_reader = _rgb_reader(fields, point_step, bool(message.is_bigendian))
    endian = ">" if message.is_bigendian else "<"
    coordinate = struct.Struct(f"{endian}f")
    stride = max(1, math.ceil(total / max_points))
    data = memoryview(message.data)
    points: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] | None = (
        [] if color_reader is not None else None
    )
    for linear_index in range(0, total, stride):
        row, column = divmod(linear_index, width)
        offset = row * row_step + column * point_step
        sample = tuple(
            coordinate.unpack_from(data, offset + field.offset)[0]
            for field in xyz
        )
        if not all(math.isfinite(value) for value in sample):
            continue
        points.append(sample)  # type: ignore[arg-type]
        if colors is not None and color_reader is not None:
            colors.append(color_reader(data, offset))
        if len(points) >= max_points:
            break
    if not points:
        raise ValueError("PointCloud2 contains no finite bounded samples")
    return points, colors


class PointCloudObserverBridge(Node):
    """Create the raw ROS subscription only while forwarding and demanded."""

    def __init__(self) -> None:
        super().__init__("pointcloud_observer_bridge")
        # The component's settings: no defaults here, the host passes them.
        self.declare_parameter("topic", "")
        self.declare_parameter("source_frame_id", "")
        self.declare_parameter("cell_id", "")
        self.declare_parameter("max_points", 0)
        self.declare_parameter("minimum_period_s", 0.0)
        # How often the Composer is asked whether a viewer wants the cloud: a
        # viewer opening sees the cloud within half a second.
        self.declare_parameter("demand_poll_period_s", 0.5)
        # Bound of one Composer request: a local service answers in
        # milliseconds; a slower one skips the frame instead of piling up.
        self.declare_parameter("http_timeout_s", 1.0)

        self._topic = _bounded_text(
            self.get_parameter("topic").value,
            "topic",
            1_024,
        )
        if not self._topic.startswith("/"):
            raise ValueError("topic must be absolute")
        self._source_frame = _bounded_text(
            self.get_parameter("source_frame_id").value,
            "source_frame_id",
            256,
        )
        self._cell_id = _observation_identity(
            self.get_parameter("cell_id").value,
            "cell_id",
            256,
        )
        composer_url = os.environ.get(_COMPOSER_URL_ENV)
        if not composer_url:
            raise ValueError(f"{_COMPOSER_URL_ENV} must name the Composer base URL")
        self._base_url = _composer_url(composer_url)
        self._owner_id = _observation_identity(
            os.environ.get(_OWNER_ID_ENV, POINTCLOUD_DEFAULT_OWNER_ID),
            _OWNER_ID_ENV,
            128,
        )
        self._max_points = int(self.get_parameter("max_points").value)
        if self._max_points < 1 or self._max_points > 200_000:
            raise ValueError("max_points must be in [1, 200000]")
        self._minimum_period_s = float(
            self.get_parameter("minimum_period_s").value
        )
        self._poll_period_s = float(
            self.get_parameter("demand_poll_period_s").value
        )
        self._http_timeout_s = float(self.get_parameter("http_timeout_s").value)
        for label, value, minimum, maximum in (
            ("minimum_period_s", self._minimum_period_s, 0.0, 10.0),
            ("demand_poll_period_s", self._poll_period_s, 0.1, 10.0),
            ("http_timeout_s", self._http_timeout_s, 0.05, 5.0),
        ):
            if not math.isfinite(value) or value < minimum or value > maximum:
                raise ValueError(f"{label} is outside its qualified bound")

        self._owner_epoch = f"pointcloud.{uuid.uuid4().hex}"
        self._stream_generation = 0
        self._subscription = None
        self._last_admitted_ns = 0
        self._condition = threading.Condition()
        self._pending: tuple[PointCloud2, int] | None = None
        self._stopping = False
        self._received = 0
        self._replaced = 0
        self._published = 0
        self._duplicates = 0
        self._suppressed = 0
        self._rejected = 0
        self._http_failures = 0
        self._forwarding = False
        self._watching_publisher = self.create_publisher(Bool, "watching", _LATCHED)
        self._received_publisher = self.create_publisher(Header, "received", 1)
        self._watching_publisher.publish(Bool(data=False))
        self._forwarding_subscription = self.create_subscription(
            Bool, "forwarding", self._receive_forwarding, _LATCHED
        )
        self._worker = threading.Thread(
            target=self._worker_loop,
            name="manyforge-pointcloud-observer",
            daemon=True,
        )
        self._worker.start()
        self._poll_timer = self.create_timer(self._poll_period_s, self._poll_demand)
        self.get_logger().info(
            f"point-cloud demand bridge ready for {self._topic} in {self._source_frame}"
        )

    def _read_status(self) -> dict[str, object]:
        request = Request(f"{self._base_url}/api/scene/pointcloud/status")
        with urlopen(request, timeout=self._http_timeout_s) as response:
            payload = response.read(_MAX_STATUS_BYTES + 1)
            if len(payload) > _MAX_STATUS_BYTES:
                raise RuntimeError("point-cloud status response is oversized")
            if response.status != 200:
                raise RuntimeError(f"point-cloud status returned HTTP {response.status}")
        value = json.loads(payload.decode("utf-8", errors="strict"))
        if (
            type(value) is not dict
            or value.get("schema_version") != POINTCLOUD_STATUS_SCHEMA
            or value.get("expected_owner_id") != self._owner_id
            or type(value.get("demand_active")) is not bool
        ):
            raise RuntimeError("point-cloud demand response is invalid")
        return value

    def _receive_forwarding(self, message: Bool) -> None:
        self._forwarding = bool(message.data)
        if not self._forwarding:
            self._detach()

    def _detach(self) -> None:
        if self._subscription is None:
            return
        self.destroy_subscription(self._subscription)
        self._subscription = None
        with self._condition:
            self._pending = None
        self._watching_publisher.publish(Bool(data=False))
        self.get_logger().info("point-cloud demand detached")

    def _poll_demand(self) -> None:
        if not self._forwarding:
            return
        try:
            demanded = self._read_status()["demand_active"] is True
        except Exception as error:
            self._http_failures += 1
            demanded = False
            if self._http_failures == 1 or self._http_failures % 20 == 0:
                self.get_logger().warning(f"point-cloud demand unavailable: {error}")
        else:
            self._http_failures = 0
        if demanded and self._subscription is None:
            with self._condition:
                self._stream_generation += 1
                stream_generation = self._stream_generation
            self._subscription = self.create_subscription(
                PointCloud2,
                self._topic,
                self._receive_cloud,
                QoSProfile(
                    history=HistoryPolicy.KEEP_LAST,
                    depth=1,
                    reliability=ReliabilityPolicy.BEST_EFFORT,
                    durability=DurabilityPolicy.VOLATILE,
                ),
            )
            self._watching_publisher.publish(Bool(data=True))
            self.get_logger().info(
                f"point-cloud demand {stream_generation} attached"
            )
        elif not demanded:
            self._detach()

    def _receive_cloud(self, message: PointCloud2) -> None:
        self._received_publisher.publish(message.header)
        now_ns = time.monotonic_ns()
        minimum_ns = int(self._minimum_period_s * 1_000_000_000)
        if now_ns - self._last_admitted_ns < minimum_ns:
            return
        self._last_admitted_ns = now_ns
        with self._condition:
            self._received += 1
            if self._pending is not None:
                self._replaced += 1
            self._pending = (message, self._stream_generation)
            self._condition.notify()

    def _post_packet(self, packet: bytes) -> str:
        request = Request(
            f"{self._base_url}/api/scene/pointcloud/observations",
            data=packet,
            method="POST",
            headers={"Content-Type": POINTCLOUD_MEDIA_TYPE},
        )
        with urlopen(request, timeout=self._http_timeout_s) as response:
            payload = response.read(_MAX_RECEIPT_BYTES + 1)
            if len(payload) > _MAX_RECEIPT_BYTES:
                raise RuntimeError("point-cloud receipt is oversized")
            if response.status not in {200, 202}:
                raise RuntimeError(f"point-cloud ingress returned HTTP {response.status}")
        value = json.loads(payload.decode("utf-8", errors="strict"))
        disposition = value.get("disposition") if type(value) is dict else None
        if (
            type(value) is not dict
            or value.get("schema_version")
            != POINTCLOUD_RECEIPT_SCHEMA
            or disposition not in {"accepted", "duplicate", "suppressed_no_observer"}
        ):
            raise RuntimeError("point-cloud ingress receipt is invalid")
        if (response.status == 202) != (disposition == "suppressed_no_observer"):
            raise RuntimeError("point-cloud ingress receipt status is inconsistent")
        return disposition

    def _worker_loop(self) -> None:
        worker_generation = -1
        sequence = 0
        while True:
            with self._condition:
                self._condition.wait_for(
                    lambda: self._stopping or self._pending is not None
                )
                if self._stopping:
                    return
                pending = self._pending
                self._pending = None
            if pending is None:
                continue
            message, stream_generation = pending
            try:
                points, colors = pointcloud2_samples(
                    message,
                    expected_frame_id=self._source_frame,
                    max_points=self._max_points,
                )
                with self._condition:
                    if stream_generation != self._stream_generation:
                        continue
                if worker_generation != stream_generation:
                    worker_generation = stream_generation
                    sequence = 0
                sequence += 1
                timestamp_ns = (
                    int(message.header.stamp.sec) * 1_000_000_000
                    + int(message.header.stamp.nanosec)
                )
                packet = encode_pointcloud_observation(
                    owner_id=self._owner_id,
                    owner_epoch=self._owner_epoch,
                    cell_id=self._cell_id,
                    stream_generation=stream_generation,
                    sequence=sequence,
                    timestamp_ns=timestamp_ns,
                    points=points,
                    colors_rgb8=colors,
                    max_points=self._max_points,
                )
                with self._condition:
                    if stream_generation != self._stream_generation:
                        continue
                disposition = self._post_packet(packet)
                if disposition == "accepted":
                    self._published += 1
                elif disposition == "duplicate":
                    self._duplicates += 1
                else:
                    self._suppressed += 1
            except (HTTPError, URLError, TimeoutError, ValueError, RuntimeError) as error:
                self._rejected += 1
                if self._rejected == 1 or self._rejected % 20 == 0:
                    self.get_logger().warning(f"point-cloud frame rejected: {error}")

    def destroy_node(self) -> bool:
        if self._subscription is not None:
            self.destroy_subscription(self._subscription)
            self._subscription = None
        with self._condition:
            self._stopping = True
            self._pending = None
            self._condition.notify_all()
        self._worker.join(timeout=self._http_timeout_s + 1.0)
        if self._worker.is_alive():
            self.get_logger().error("point-cloud worker did not stop within its bound")
        self.get_logger().info(
            "point-cloud bridge stopped: "
            f"received={self._received} replaced={self._replaced} "
            f"published={self._published} duplicates={self._duplicates} "
            f"suppressed={self._suppressed} rejected={self._rejected}"
        )
        return super().destroy_node()


def main(args: list[str] | None = None) -> None:
    rclpy.init(args=args)
    node: PointCloudObserverBridge | None = None
    try:
        node = PointCloudObserverBridge()
        rclpy.spin(node)
    finally:
        if node is not None:
            node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
