"""PointCloud2 cell-frame compaction, demand-bridge source and launch coverage."""

import importlib.util
from pathlib import Path
import struct

import pytest
from sensor_msgs.msg import PointCloud2, PointField

from manyforge_ros_bringup.pointcloud_observer_bridge import (
    pointcloud2_samples,
)


def _field(name: str, offset: int, datatype: int) -> PointField:
    return PointField(name=name, offset=offset, datatype=datatype, count=1)


def _cloud(*, frame_id: str = "universe") -> PointCloud2:
    message = PointCloud2()
    message.header.frame_id = frame_id
    message.height = 1
    message.width = 3
    message.fields = [
        _field("x", 0, PointField.FLOAT32),
        _field("y", 4, PointField.FLOAT32),
        _field("z", 8, PointField.FLOAT32),
        _field("rgb", 12, PointField.UINT32),
    ]
    message.is_bigendian = False
    message.point_step = 16
    message.row_step = 48
    message.data = b"".join(
        (
            struct.pack("<fffI", 1.0, 2.0, 3.0, 0x00FF8000),
            struct.pack("<fffI", float("nan"), 2.0, 3.0, 0x00000000),
            struct.pack("<fffI", 4.0, 5.0, 6.0, 0x000040FF),
        )
    )
    message.is_dense = False
    return message


def test_pointcloud2_compaction_is_finite_bounded_and_rgb8_exact() -> None:
    points, colors = pointcloud2_samples(
        _cloud(),
        expected_frame_id="universe",
        max_points=3,
    )

    assert points == [(1.0, 2.0, 3.0), (4.0, 5.0, 6.0)]
    assert colors == [(255, 128, 0), (0, 64, 255)]


def test_pointcloud2_compaction_rejects_cross_frame_and_bad_storage() -> None:
    with pytest.raises(ValueError, match="configured cell frame"):
        pointcloud2_samples(
            _cloud(frame_id="camera"),
            expected_frame_id="universe",
            max_points=3,
        )
    malformed = _cloud()
    malformed.row_step = 47
    with pytest.raises(ValueError, match="storage shape"):
        pointcloud2_samples(
            malformed,
            expected_frame_id="universe",
            max_points=3,
        )
    partial_color = _cloud()
    partial_color.fields = partial_color.fields[:3] + [
        _field("r", 12, PointField.UINT8)
    ]
    with pytest.raises(ValueError, match="all or none"):
        pointcloud2_samples(
            partial_color,
            expected_frame_id="universe",
            max_points=3,
        )


def test_bridge_source_keeps_qos_demand_and_worker_bounds_explicit() -> None:
    source = Path(__file__).resolve().parents[1] / (
        "manyforge_ros_bringup/pointcloud_observer_bridge.py"
    )
    text = source.read_text(encoding="utf-8")

    for required in (
        "ReliabilityPolicy.BEST_EFFORT",
        "DurabilityPolicy.VOLATILE",
        "HistoryPolicy.KEEP_LAST",
        "depth=1",
        "self.destroy_subscription",
        "self._pending",
        "_MAX_SOURCE_BYTES",
        "memoryview(message.data)",
        "urlopen(request, timeout=self._http_timeout_s)",
        "POINTCLOUD_STATUS_SCHEMA",
        "POINTCLOUD_RECEIPT_SCHEMA",
        "POINTCLOUD_DEFAULT_OWNER_ID",
        "MANYFORGE_COMPOSER_URL",
        "DurabilityPolicy.TRANSIENT_LOCAL",
        '"forwarding"',
        '"watching"',
        '"received"',
    ):
        assert required in text
    assert "DemoSupervisor" not in text
    # Vendor-neutral: no simulator frame or Composer address baked in.
    for absent in ("universe", "isaac", "127.0.0.1"):
        assert absent not in text


def test_launch_runs_the_bridge_with_the_component_settings() -> None:
    pytest.importorskip("launch_ros")
    spec = importlib.util.spec_from_file_location(
        "pointcloud_source_launch",
        Path(__file__).resolve().parents[1] / "launch/pointcloud_source.launch.py",
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    kwargs = module.node_kwargs(
        "/manyforge/cloud", "/camera/points", "world", "cell", "5000", "0.2"
    )
    assert kwargs["executable"] == "pointcloud_observer_bridge"
    assert kwargs["namespace"] == "/manyforge/cloud"
    assert kwargs["parameters"] == [{
        "topic": "/camera/points",
        "source_frame_id": "world",
        "cell_id": "cell",
        "max_points": 5000,
        "minimum_period_s": 0.2,
    }]
    with pytest.raises(ValueError, match="topic"):
        module.node_kwargs("/ns", "camera/points", "world", "cell", "1", "0.1")
    with pytest.raises(ValueError, match="source_frame_id"):
        module.node_kwargs("/ns", "/camera/points", "", "cell", "1", "0.1")
