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


def _publisher_code(
    *,
    topic: str,
    covariance: float,
    detection_id: str = "box-01",
    class_id: str = "mac_and_cheese",
    score: float = 0.92,
    frame_id: str = "world",
) -> str:
    return textwrap.dedent(
        f"""
        import signal
        import time

        import rclpy
        from vision_msgs.msg import Detection3DArray, Detection3D, ObjectHypothesisWithPose

        rclpy.init(args=None)
        node = rclpy.create_node("foundationpose_qualifier_test_publisher")
        publisher = node.create_publisher(Detection3DArray, {topic!r}, 1)
        stop_requested = False

        def request_stop(_signum, _frame):
            global stop_requested
            stop_requested = True

        signal.signal(signal.SIGTERM, request_stop)
        deadline = time.time() + 6.0
        while not stop_requested and time.time() < deadline:
            msg = Detection3DArray()
            msg.header.frame_id = {frame_id!r}
            msg.header.stamp = node.get_clock().now().to_msg()
            detection = Detection3D()
            detection.header = msg.header
            detection.id = {detection_id!r}
            result = ObjectHypothesisWithPose()
            result.hypothesis.class_id = {class_id!r}
            result.hypothesis.score = {score!r}
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


def _run_qualifier(
    *,
    covariance: float,
    min_samples: int,
    association_args: list[str] | None = None,
    confidence_min: float = 0.8,
    extra_args: list[str] | None = None,
    publisher_kwargs: dict[str, object] | None = None,
) -> subprocess.CompletedProcess[str]:
    topic = f"/foundationpose_qualifier_test_{os.getpid()}_{time.monotonic_ns()}"
    publisher_kwargs = dict(publisher_kwargs or {})
    publisher = subprocess.Popen(
        [
            sys.executable,
            "-c",
            _publisher_code(
                topic=topic,
                covariance=covariance,
                **publisher_kwargs,
            ),
        ],
        cwd=REPO_ROOT,
        text=True,
    )
    try:
        association = association_args or ["--class-id", "mac_and_cheese"]
        result = subprocess.run(
            [
                sys.executable,
                str(QUALIFIER),
                "--topic",
                topic,
                "--object-id",
                "isaac_cheese_box",
                *association,
                "--confidence-min",
                str(confidence_min),
                "--max-position-variance-m2",
                "0.01",
                "--expected-frame-id",
                "world",
                "--min-accepted-samples",
                str(min_samples),
                "--timeout-s",
                "5",
                *(extra_args or []),
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


def test_foundationpose_detection3d_qualifier_selection_accepts_bootstrap_stream():
    result = _run_qualifier(
        covariance=0.0,
        min_samples=1,
        association_args=["--selection", "first"],
        confidence_min=0.0,
        extra_args=["--allow-zero-confidence", "--allow-zero-covariance"],
        publisher_kwargs={
            "detection_id": "",
            "class_id": "",
            "score": 0.0,
        },
    )

    assert result.returncode == 0, result.stderr + result.stdout
    payload = json.loads(result.stdout)
    assert payload["passed"] is True
    assert payload["association"]["selection"] == "first"
    assert payload["last_confidence"] == pytest.approx(0.0)
    assert payload["last_position_variance_m2"] == pytest.approx(0.0)
    assert payload["raw_detection"]["last_selection"] == "first"


def test_foundationpose_detection3d_qualifier_rejects_zero_confidence_by_default():
    result = _run_qualifier(
        covariance=0.001,
        min_samples=1,
        association_args=["--selection", "first"],
        confidence_min=0.0,
        extra_args=["--allow-zero-covariance"],
        publisher_kwargs={
            "detection_id": "",
            "class_id": "",
            "score": 0.0,
        },
    )

    assert result.returncode != 0
    payload = json.loads(result.stdout)
    assert payload["passed"] is False
    assert any(
        "zero" in reason and "confidence" in reason
        for reason in payload["failure_reasons"]
    )
