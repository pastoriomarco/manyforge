#!/usr/bin/env python3
"""Live qualification for FoundationPose-style Detection3DArray pose sources.

FoundationPose's Isaac ROS pose-estimation and tracking nodes publish
``vision_msgs/msg/Detection3DArray``. ManyForge consumes those streams through
the generic ``detection3d_array_topic`` pose-source path; this script is the
deployment/live gate that proves a concrete FoundationPose stream has the
association, confidence, covariance, frame, and timestamp properties required
before using it for execution-affecting apply policies.

The script is passive: it subscribes to ROS only and does not mutate Composer,
Isaac, or the planning scene.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from dataclasses import dataclass
from types import SimpleNamespace
from typing import Any, Sequence


DEFAULT_TOPIC = "/pose_estimation/output"


@dataclass
class RawDetectionEvidence:
    seen_messages: int = 0
    matched_messages: int = 0
    last_frame_id: str = ""
    last_detection_id: str = ""
    last_class_id: str = ""
    last_score: float | None = None
    last_position_variance_m2: float | None = None
    last_stamp_sec: int = 0
    last_stamp_nanosec: int = 0

    def to_dict(self) -> dict[str, Any]:
        return {
            "seen_messages": self.seen_messages,
            "matched_messages": self.matched_messages,
            "last_frame_id": self.last_frame_id,
            "last_detection_id": self.last_detection_id,
            "last_class_id": self.last_class_id,
            "last_score": self.last_score,
            "last_position_variance_m2": self.last_position_variance_m2,
            "last_stamp_sec": self.last_stamp_sec,
            "last_stamp_nanosec": self.last_stamp_nanosec,
        }


@dataclass
class QualificationResult:
    passed: bool
    topic: str
    object_id: str
    association: dict[str, str]
    accepted_samples: int
    last_pose_frame_id: str
    last_confidence: float | None
    last_position_variance_m2: float | None
    raw_detection: RawDetectionEvidence
    failure_reasons: list[str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "passed": self.passed,
            "topic": self.topic,
            "object_id": self.object_id,
            "association": self.association,
            "accepted_samples": self.accepted_samples,
            "last_pose_frame_id": self.last_pose_frame_id,
            "last_confidence": self.last_confidence,
            "last_position_variance_m2": self.last_position_variance_m2,
            "raw_detection": self.raw_detection.to_dict(),
            "failure_reasons": self.failure_reasons,
        }


class RawDetectionAuditor:
    def __init__(
        self,
        *,
        detection_id: str,
        class_id: str,
    ) -> None:
        self.detection_id = detection_id
        self.class_id = class_id
        self.evidence = RawDetectionEvidence()

    def observe(self, msg: Any) -> None:
        self.evidence.seen_messages += 1
        header = getattr(msg, "header", None)
        best: tuple[float, Any, Any] | None = None
        for detection in list(getattr(msg, "detections", []) or []):
            detection_identifier = str(getattr(detection, "id", "") or "")
            for result in list(getattr(detection, "results", []) or []):
                hypothesis = getattr(result, "hypothesis", None)
                result_class_id = str(getattr(hypothesis, "class_id", "") or "")
                score = float(getattr(hypothesis, "score", 0.0) or 0.0)
                if self.detection_id:
                    matched = detection_identifier == self.detection_id
                else:
                    matched = result_class_id == self.class_id
                if matched and (best is None or score > best[0]):
                    best = (score, detection, result)
        if best is None:
            return
        score, detection, result = best
        self.evidence.matched_messages += 1
        detection_header = getattr(detection, "header", None)
        selected_header = detection_header or header
        self.evidence.last_frame_id = str(getattr(selected_header, "frame_id", "") or "")
        stamp = getattr(selected_header, "stamp", None)
        self.evidence.last_stamp_sec = int(getattr(stamp, "sec", 0) or 0)
        self.evidence.last_stamp_nanosec = int(getattr(stamp, "nanosec", 0) or 0)
        self.evidence.last_detection_id = str(getattr(detection, "id", "") or "")
        hypothesis = getattr(result, "hypothesis", None)
        self.evidence.last_class_id = str(getattr(hypothesis, "class_id", "") or "")
        self.evidence.last_score = score
        pose_with_covariance = getattr(result, "pose", None)
        self.evidence.last_position_variance_m2 = _position_covariance_max(
            getattr(pose_with_covariance, "covariance", None)
        )


def _position_covariance_max(covariance: Any) -> float | None:
    try:
        values = list(covariance)
    except TypeError:
        return None
    if len(values) < 15:
        return None
    diagonal = [float(values[0]), float(values[7]), float(values[14])]
    if not all(math.isfinite(value) for value in diagonal):
        return None
    return max(diagonal)


def _source_config(args: argparse.Namespace) -> SimpleNamespace:
    metadata: dict[str, Any] = {}
    if args.detection_id:
        metadata["detection_id"] = args.detection_id
    if args.class_id:
        metadata["class_id"] = args.class_id
    metadata["max_position_variance_m2"] = args.max_position_variance_m2
    binding = SimpleNamespace(
        object_id=args.object_id,
        entity=args.detection_id or args.class_id,
        apply_policy=args.apply_policy,
        frame_id=args.expected_frame_id or args.frame_id,
        confidence_min=args.confidence_min,
        metadata=metadata,
    )
    return SimpleNamespace(
        kind="detection3d_array_topic",
        source_id=args.source_id,
        topic=args.topic,
        frame_id=args.frame_id,
        max_update_hz=args.max_update_hz,
        stale_after_s=args.stale_after_s,
        objects={args.object_id: binding},
    )


def _qualify(args: argparse.Namespace) -> QualificationResult:
    if bool(args.detection_id) == bool(args.class_id):
        raise ValueError("provide exactly one of --detection-id or --class-id")
    if args.min_accepted_samples <= 0:
        raise ValueError("--min-accepted-samples must be > 0")
    if args.confidence_min <= 0.0 or not math.isfinite(args.confidence_min):
        raise ValueError("--confidence-min must be positive and finite")
    if (
        args.max_position_variance_m2 <= 0.0
        or not math.isfinite(args.max_position_variance_m2)
    ):
        raise ValueError("--max-position-variance-m2 must be positive and finite")

    import rclpy
    from rclpy.executors import SingleThreadedExecutor
    from rclpy.qos import QoSProfile
    from vision_msgs.msg import Detection3DArray

    from manyforge_ros.pose_source_sidecar import Detection3DArrayTopicPoseSource

    rclpy.init(args=None)
    node = rclpy.create_node("manyforge_foundationpose_detection3d_qualifier")
    executor = SingleThreadedExecutor()
    executor.add_node(node)
    auditor = RawDetectionAuditor(
        detection_id=args.detection_id,
        class_id=args.class_id,
    )
    raw_subscription = node.create_subscription(
        Detection3DArray,
        args.topic,
        auditor.observe,
        QoSProfile(depth=1),
    )
    source = Detection3DArrayTopicPoseSource.from_deployment_config(
        node=node,
        source_config=_source_config(args),
    )

    accepted_samples = 0
    accepted_update_times: set[float] = set()
    last_pose_frame_id = ""
    last_confidence: float | None = None
    last_position_variance_m2: float | None = None
    failure_reasons: list[str] = []
    deadline_s = time.monotonic() + args.timeout_s

    try:
        while time.monotonic() < deadline_s and accepted_samples < args.min_accepted_samples:
            executor.spin_once(timeout_sec=min(args.poll_s, 0.2))
            snapshot = source.snapshot(monotonic_s=time.monotonic())
            if not snapshot.objects:
                continue
            state = snapshot.objects[0]
            if state.update is None:
                continue
            if state.update.monotonic_s in accepted_update_times:
                continue
            accepted_update_times.add(state.update.monotonic_s)
            accepted_samples += 1
            last_pose_frame_id = state.update.pose.frame_id
            metadata = dict(state.update.metadata or {})
            confidence = metadata.get("confidence")
            variance = metadata.get("maxPositionVarianceM2")
            last_confidence = float(confidence) if confidence is not None else None
            last_position_variance_m2 = float(variance) if variance is not None else None

        raw = auditor.evidence
        if raw.seen_messages <= 0:
            failure_reasons.append(f"no Detection3DArray messages received on {args.topic}")
        if raw.matched_messages <= 0:
            failure_reasons.append("no detection matched the configured association")
        if accepted_samples < args.min_accepted_samples:
            failure_reasons.append(
                f"accepted {accepted_samples} samples; need {args.min_accepted_samples}"
            )
        if args.expected_frame_id and last_pose_frame_id != args.expected_frame_id:
            failure_reasons.append(
                f"pose frame '{last_pose_frame_id}' != expected '{args.expected_frame_id}'"
            )
        elif not last_pose_frame_id:
            failure_reasons.append("accepted pose did not carry a frame id")
        if (
            last_confidence is None
            or not math.isfinite(last_confidence)
            or last_confidence < args.confidence_min
        ):
            failure_reasons.append("accepted pose confidence did not satisfy threshold")
        if (
            last_position_variance_m2 is None
            or not math.isfinite(last_position_variance_m2)
            or last_position_variance_m2 > args.max_position_variance_m2
        ):
            failure_reasons.append(
                "accepted pose covariance did not satisfy max-position-variance threshold"
            )
        if (
            not args.allow_zero_covariance
            and raw.last_position_variance_m2 is not None
            and raw.last_position_variance_m2 <= 0.0
        ):
            failure_reasons.append(
                "matched detection reported zero position covariance; pass "
                "--allow-zero-covariance only after confirming the producer's covariance contract"
            )
        if args.require_nonzero_stamp and (
            raw.last_stamp_sec == 0 and raw.last_stamp_nanosec == 0
        ):
            failure_reasons.append("matched detection did not carry a non-zero stamp")
        passed = not failure_reasons
        return QualificationResult(
            passed=passed,
            topic=args.topic,
            object_id=args.object_id,
            association={
                "detection_id": args.detection_id,
                "class_id": args.class_id,
            },
            accepted_samples=accepted_samples,
            last_pose_frame_id=last_pose_frame_id,
            last_confidence=last_confidence,
            last_position_variance_m2=last_position_variance_m2,
            raw_detection=raw,
            failure_reasons=failure_reasons,
        )
    finally:
        source.close()
        node.destroy_subscription(raw_subscription)
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topic", default=DEFAULT_TOPIC)
    parser.add_argument("--source-id", default="foundationpose_detections")
    parser.add_argument("--object-id", required=True)
    association = parser.add_mutually_exclusive_group(required=True)
    association.add_argument("--detection-id", default="")
    association.add_argument("--class-id", default="")
    parser.add_argument("--frame-id", default="")
    parser.add_argument("--expected-frame-id", default="")
    parser.add_argument("--apply-policy", default="observe_only")
    parser.add_argument("--confidence-min", type=float, required=True)
    parser.add_argument("--max-position-variance-m2", type=float, required=True)
    parser.add_argument("--max-update-hz", type=float, default=30.0)
    parser.add_argument("--stale-after-s", type=float, default=1.0)
    parser.add_argument("--timeout-s", type=float, default=15.0)
    parser.add_argument("--poll-s", type=float, default=0.05)
    parser.add_argument("--min-accepted-samples", type=int, default=3)
    parser.add_argument(
        "--allow-zero-covariance",
        action="store_true",
        help=(
            "Allow zero position covariance. Use only after confirming the "
            "producer intentionally publishes zero covariance."
        ),
    )
    parser.add_argument(
        "--require-nonzero-stamp",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    args = parser.parse_args(argv)

    try:
        result = _qualify(args)
    except Exception as exc:
        print(
            json.dumps(
                {
                    "passed": False,
                    "failure_reasons": [str(exc)],
                },
                indent=2,
                sort_keys=True,
            )
        )
        return 1
    print(json.dumps(result.to_dict(), indent=2, sort_keys=True))
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
