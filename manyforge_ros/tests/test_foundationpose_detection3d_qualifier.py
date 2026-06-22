from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest


pytest.importorskip("rclpy")
pytest.importorskip("vision_msgs")


REPO_ROOT = Path(__file__).resolve().parents[2]
QUALIFIER = REPO_ROOT / "scripts" / "ros" / "validate-foundationpose-detection3d-source.py"


def _publisher_code(*, topic: str, covariance: float) -> str:
    return textwrap.dedent(
        f"""
        import time

        import rclpy
        from vision_msgs.msg import Detection3DArray, Detection3D, ObjectHypothesisWithPose

        rclpy.init(args=None)
        node = rclpy.create_node("foundationpose_qualifier_test_publisher")
        publisher = node.create_publisher(Detection3DArray, {topic!r}, 1)
        deadline = time.time() + 6.0
        while time.time() < deadline:
            msg = Detection3DArray()
            msg.header.frame_id = "world"
            msg.header.stamp = node.get_clock().now().to_msg()
            detection = Detection3D()
            detection.header = msg.header
            detection.id = "box-01"
            result = ObjectHypothesisWithPose()
            result.hypothesis.class_id = "mac_and_cheese"
            result.hypothesis.score = 0.92
            result.pose.pose.position.x = 0.3
            result.pose.pose.position.y = 0.1
            result.pose.pose.position.z = 0.2
            result.pose.pose.orientation.w = 1.0
            result.pose.covariance[0] = {covariance!r}
            result.pose.covariance[7] = {covariance!r}
            result.pose.covariance[14] = {covariance!r}
            detection.results.append(result)
            msg.detections.append(detection)
            publisher.publish(msg)
            rclpy.spin_once(node, timeout_sec=0.0)
            time.sleep(0.1)
        node.destroy_node()
        rclpy.shutdown()
        """
    )


def _run_qualifier(*, covariance: float, min_samples: int) -> subprocess.CompletedProcess[str]:
    topic = f"/foundationpose_qualifier_test_{os.getpid()}_{time.monotonic_ns()}"
    publisher = subprocess.Popen(
        [sys.executable, "-c", _publisher_code(topic=topic, covariance=covariance)],
        cwd=REPO_ROOT,
        text=True,
    )
    try:
        result = subprocess.run(
            [
                sys.executable,
                str(QUALIFIER),
                "--topic",
                topic,
                "--object-id",
                "isaac_cheese_box",
                "--class-id",
                "mac_and_cheese",
                "--confidence-min",
                "0.8",
                "--max-position-variance-m2",
                "0.01",
                "--expected-frame-id",
                "world",
                "--min-accepted-samples",
                str(min_samples),
                "--timeout-s",
                "5",
            ],
            cwd=REPO_ROOT,
            text=True,
            capture_output=True,
            timeout=10,
        )
    finally:
        publisher.terminate()
        try:
            publisher.wait(timeout=3)
        except subprocess.TimeoutExpired:
            publisher.kill()
            publisher.wait(timeout=3)
    return result


def test_foundationpose_detection3d_qualifier_accepts_valid_stream():
    result = _run_qualifier(covariance=0.001, min_samples=3)

    assert result.returncode == 0, result.stderr + result.stdout
    payload = json.loads(result.stdout)
    assert payload["passed"] is True
    assert payload["accepted_samples"] == 3
    assert payload["last_pose_frame_id"] == "world"
    assert payload["last_confidence"] == pytest.approx(0.92)
    assert payload["last_position_variance_m2"] == pytest.approx(0.001)
    assert payload["raw_detection"]["matched_messages"] >= 3


def test_foundationpose_detection3d_qualifier_rejects_zero_covariance_by_default():
    result = _run_qualifier(covariance=0.0, min_samples=1)

    assert result.returncode != 0
    payload = json.loads(result.stdout)
    assert payload["passed"] is False
    assert any(
        "zero position covariance" in reason
        for reason in payload["failure_reasons"]
    )
