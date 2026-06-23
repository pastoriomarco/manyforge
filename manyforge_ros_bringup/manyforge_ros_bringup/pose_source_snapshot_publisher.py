"""Publish configured ROS object-pose snapshots to Composer.

The node is intentionally narrow:

* it reads ``pose_sources`` from the loaded deployment artifact;
* it polls enabled ``simulation_interfaces`` sources and samples supported ROS
  topic sources;
* it posts observe-only latest-wins snapshots to Composer;
* it never mutates the planning scene directly.

Composer owns the policy-gated apply step (when-idle, before-planning,
between-nodes).  This process is only the ROS/simulator ingestion surface.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import rclpy
from rclpy.node import Node

from manyforge_behavior.deployment import Deployment
from manyforge_ros.pose_source_sidecar import (
    Detection3DArrayTopicPoseSource,
    MeasuredRobotJointStateTracker,
    PoseArrayTopicPoseSource,
    PoseStampedTopicPoseSource,
    SimulationInterfacesPoseSource,
    TfPoseSource,
)
from manyforge_ros.scene_mirror_probe import SimulationSceneMirrorClient


@dataclass
class _RuntimeSource:
    source: Any
    next_poll_s: float
    measured_robot: MeasuredRobotJointStateTracker | None = None
    consecutive_post_failures: int = 0


class PoseSourceSnapshotPublisher(Node):
    """Poll simulator object poses and publish snapshots to Composer."""

    def __init__(self) -> None:
        super().__init__("manyforge_pose_source_snapshot_publisher")
        self.declare_parameter("composer_url", "http://127.0.0.1:9000")
        self.declare_parameter(
            "deployment_path",
            "/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
        )
        self.declare_parameter("service_timeout_s", 2.0)
        self.declare_parameter("publish_timeout_s", 1.0)
        self.declare_parameter("retry_log_period_s", 5.0)

        self.composer_url = (
            str(self.get_parameter("composer_url").value).strip().rstrip("/")
        )
        self.deployment_path = Path(str(self.get_parameter("deployment_path").value))
        self.service_timeout_s = max(
            0.05, float(self.get_parameter("service_timeout_s").value)
        )
        self.publish_timeout_s = max(
            0.05, float(self.get_parameter("publish_timeout_s").value)
        )
        self.retry_log_period_s = max(
            1.0, float(self.get_parameter("retry_log_period_s").value)
        )
        self._last_error_log_s = 0.0
        self._stop_event = threading.Event()

        self._sources = self._load_sources()

    def stop(self) -> None:
        self._stop_event.set()

    def run(self) -> None:
        if not self._sources:
            self.get_logger().info(
                "No enabled runtime pose sources declared in "
                f"{self.deployment_path}; snapshot publisher is idle."
            )
            while rclpy.ok() and not self._stop_event.wait(1.0):
                pass
            return

        self.get_logger().info(
            f"Publishing {len(self._sources)} pose source(s) to Composer at "
            f"{self.composer_url}"
        )
        while rclpy.ok() and not self._stop_event.is_set():
            rclpy.spin_once(self, timeout_sec=0.0)
            now = time.monotonic()
            next_due_s = None
            for runtime in self._sources:
                if now + 1e-9 < runtime.next_poll_s:
                    next_due_s = runtime.next_poll_s if next_due_s is None else min(
                        next_due_s, runtime.next_poll_s
                    )
                    continue
                self._poll_and_publish(runtime)
                runtime.next_poll_s = now + runtime.source.period_s
                next_due_s = runtime.next_poll_s if next_due_s is None else min(
                    next_due_s, runtime.next_poll_s
                )

            sleep_s = 0.05
            if next_due_s is not None:
                sleep_s = min(0.25, max(0.0, next_due_s - time.monotonic()))
            self._stop_event.wait(sleep_s)

    def _load_sources(self) -> list[_RuntimeSource]:
        try:
            deployment = Deployment(str(self.deployment_path))
        except Exception as exc:
            self.get_logger().error(
                f"Cannot load deployment for pose-source publisher: {exc}"
            )
            return []

        client: SimulationSceneMirrorClient | None = None
        tf_buffer: Any | None = None
        tf_listener: Any | None = None
        runtime_sources: list[_RuntimeSource] = []
        for source in deployment.pose_sources.values():
            if not source.enabled:
                continue
            try:
                if source.kind == "simulation_interfaces":
                    if client is None:
                        client = SimulationSceneMirrorClient(
                            self,
                            service_timeout_s=self.service_timeout_s,
                        )
                    runtime_source = SimulationInterfacesPoseSource.from_deployment_config(
                        client=client,
                        source_config=source,
                    )
                elif source.kind == "pose_stamped_topic":
                    runtime_source = PoseStampedTopicPoseSource.from_deployment_config(
                        node=self,
                        source_config=source,
                    )
                elif source.kind == "pose_array_topic":
                    runtime_source = PoseArrayTopicPoseSource.from_deployment_config(
                        node=self,
                        source_config=source,
                    )
                elif source.kind == "detection3d_array_topic":
                    runtime_source = (
                        Detection3DArrayTopicPoseSource.from_deployment_config(
                            node=self,
                            source_config=source,
                        )
                    )
                elif source.kind == "tf":
                    if tf_buffer is None:
                        from tf2_ros import Buffer, TransformListener

                        tf_buffer = Buffer()
                        tf_listener = TransformListener(tf_buffer, self)
                    runtime_source = TfPoseSource.from_deployment_config(
                        node=self,
                        source_config=source,
                        buffer=tf_buffer,
                        listener=tf_listener,
                    )
                else:
                    self.get_logger().warning(
                        "Ignoring unsupported pose source kind "
                        f"'{source.kind}' for source '{source.source_id}'"
                    )
                    continue
                measured_robot = None
                measured_robot_config = getattr(source, "measured_robot", None)
                if measured_robot_config is not None:
                    measured_robot = MeasuredRobotJointStateTracker(
                        node=self,
                        pose_source_id=source.source_id,
                        joint_state_topic=measured_robot_config.joint_state_topic,
                    )
                runtime_sources.append(
                    _RuntimeSource(
                        source=runtime_source,
                        next_poll_s=0.0,
                        measured_robot=measured_robot,
                    )
                )
            except Exception as exc:
                self.get_logger().error(
                    "Ignoring invalid pose source "
                    f"'{getattr(source, 'source_id', '<unknown>')}': {exc}"
                )
        return runtime_sources

    def _poll_and_publish(self, runtime: _RuntimeSource) -> None:
        source_id = runtime.source.pose_source_id
        try:
            result = (
                runtime.source.poll_once()
                if hasattr(runtime.source, "poll_once")
                else None
            )
            if runtime.measured_robot is not None:
                update = runtime.measured_robot.latest_update()
                if update is not None:
                    runtime.source.state.apply_measured_robot_update(update)
            snapshot = runtime.source.state.snapshot()
            self._post_snapshot(source_id, snapshot.to_dict())
            if runtime.consecutive_post_failures:
                self.get_logger().info(
                    f"pose source '{source_id}' recovered after "
                    f"{runtime.consecutive_post_failures} post failure(s)"
                )
            runtime.consecutive_post_failures = 0
            if result is not None and result.source_errors:
                self.get_logger().warning(
                    f"pose source '{source_id}' polled with source errors: "
                    f"{'; '.join(result.source_errors)}"
                )
        except Exception as exc:
            runtime.consecutive_post_failures += 1
            self._log_retry_limited(
                f"pose source '{source_id}' publish failed "
                f"({runtime.consecutive_post_failures} consecutive): {exc}"
            )

    def _post_snapshot(self, source_id: str, payload: dict[str, Any]) -> None:
        quoted = urllib.parse.quote(source_id, safe="")
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        request = urllib.request.Request(
            f"{self.composer_url}/api/scene/pose-sources/{quoted}/snapshot",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request,
                timeout=self.publish_timeout_s,
            ) as response:
                if response.status < 200 or response.status >= 300:
                    raise RuntimeError(f"HTTP {response.status}")
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc

    def _log_retry_limited(self, message: str) -> None:
        now = time.monotonic()
        if now - self._last_error_log_s < self.retry_log_period_s:
            return
        self._last_error_log_s = now
        self.get_logger().warning(message)


def main(args: list[str] | None = None) -> int:
    rclpy.init(args=args)
    node = PoseSourceSnapshotPublisher()
    try:
        node.run()
        return 0
    except KeyboardInterrupt:
        return 0
    finally:
        node.stop()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
