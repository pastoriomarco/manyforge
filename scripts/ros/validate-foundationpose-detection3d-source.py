#!/usr/bin/env python3
"""Bounded FoundationPose Detection3D evidence helpers and unavailable gate.

FoundationPose's Isaac ROS pose-estimation and tracking nodes publish
``vision_msgs/msg/Detection3DArray``.  This file retains only an owner-neutral
raw-message evidence reducer plus the live-entrypoint gate.  It does not expose
the former deployment-v0 pose-source object or its CLI schema.

The former source-config builder, argument schema, and live ROS subscriber are
intentionally absent.  Their exact implementation is preserved at
ManyForge commit 08222188293df8e12f90dc72e204c21f22f92ea1, blob
0608a1a5cb7f717548414f2aa25271769263f87f.  They are acceptance input, not a
runnable compatibility contract.
"""

from __future__ import annotations

import math
import sys
from dataclasses import dataclass
from itertools import islice
from numbers import Real
from typing import Any, Iterable, Sequence


MAX_BATCH_ITEMS = 4096
MAX_SOURCE_TEXT_CHARS = 1024
POSE_COVARIANCE_ITEMS = 36
ROS_TIME_SEC_MIN = -(2**31)
ROS_TIME_SEC_MAX = 2**31 - 1
ROS_TIME_NANOSEC_MAX = 999_999_999
UNAVAILABLE_MESSAGE = (
    "composable_cell_runtime_unavailable: FoundationPose Detection3D live "
    "qualification requires the focused ROS/Isaac adapter, not available yet"
)


@dataclass
class RawDetectionEvidence:
    seen_messages: int = 0
    matched_messages: int = 0
    last_frame_id: str = ""
    last_detection_id: str = ""
    last_class_id: str = ""
    last_selection: str = ""
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
            "last_selection": self.last_selection,
            "last_score": self.last_score,
            "last_position_variance_m2": self.last_position_variance_m2,
            "last_stamp_sec": self.last_stamp_sec,
            "last_stamp_nanosec": self.last_stamp_nanosec,
        }


class RawDetectionAuditor:
    def __init__(
        self,
        *,
        detection_id: str,
        class_id: str,
        selection: str,
    ) -> None:
        self.detection_id = _bounded_text(detection_id, label="detection_id")
        self.class_id = _bounded_text(class_id, label="class_id")
        self.selection = _bounded_text(selection, label="selection")
        if self.selection not in ("", "first", "best_score"):
            raise ValueError("selection must be 'first' or 'best_score'")
        if sum(
            bool(value)
            for value in (self.detection_id, self.class_id, self.selection)
        ) != 1:
            raise ValueError(
                "auditor requires exactly one detection_id, class_id, or selection"
            )
        self.evidence = RawDetectionEvidence()

    def observe(self, msg: Any) -> None:
        header = getattr(msg, "header", None)
        best: tuple[float, Any, Any] | None = None
        detections = _bounded_items(
            getattr(msg, "detections", ()), label="detections"
        )
        for detection in detections:
            detection_identifier = _bounded_text(
                getattr(detection, "id", "") or "", label="detection id"
            )
            if not self.selection and self.detection_id:
                if detection_identifier != self.detection_id:
                    continue
            results = _bounded_items(
                getattr(detection, "results", ()), label="detection results"
            )
            for result in results:
                hypothesis = getattr(result, "hypothesis", None)
                result_class_id = _bounded_text(
                    getattr(hypothesis, "class_id", "") or "",
                    label="result class id",
                )
                if not self.selection and self.class_id and result_class_id != self.class_id:
                    continue
                raw_score = getattr(hypothesis, "score", 0.0)
                if isinstance(raw_score, bool) or not isinstance(raw_score, Real):
                    continue
                score = float(raw_score)
                if not math.isfinite(score):
                    continue
                if self.selection == "first":
                    best = (score, detection, result)
                    break
                if best is None or score > best[0]:
                    best = (score, detection, result)
            if best is not None and self.selection == "first":
                break
        if best is None:
            self.evidence.seen_messages += 1
            return
        score, detection, result = best
        detection_header = getattr(detection, "header", None)
        selected_header = _preferred_raw_header(detection_header, header)
        frame_id = _bounded_text(
            getattr(selected_header, "frame_id", "") or "", label="frame id"
        )
        stamp = getattr(selected_header, "stamp", None)
        stamp_sec, stamp_nanosec = _validated_stamp(stamp)
        detection_identifier = _bounded_text(
            getattr(detection, "id", "") or "", label="detection id"
        )
        hypothesis = getattr(result, "hypothesis", None)
        class_identifier = _bounded_text(
            getattr(hypothesis, "class_id", "") or "", label="result class id"
        )
        position_variance_m2 = _position_covariance_max(
            getattr(getattr(result, "pose", None), "covariance", None)
        )

        self.evidence.seen_messages += 1
        self.evidence.matched_messages += 1
        self.evidence.last_frame_id = frame_id
        self.evidence.last_stamp_sec = stamp_sec
        self.evidence.last_stamp_nanosec = stamp_nanosec
        self.evidence.last_detection_id = detection_identifier
        self.evidence.last_class_id = class_identifier
        self.evidence.last_selection = self.selection
        self.evidence.last_score = score
        self.evidence.last_position_variance_m2 = position_variance_m2


def _position_covariance_max(covariance: Any) -> float | None:
    try:
        values = tuple(islice(iter(covariance), POSE_COVARIANCE_ITEMS + 1))
    except TypeError:
        return None
    if len(values) != POSE_COVARIANCE_ITEMS:
        return None
    try:
        raw_diagonal = (values[0], values[7], values[14])
        if any(
            isinstance(value, bool) or not isinstance(value, Real)
            for value in raw_diagonal
        ):
            return None
        diagonal = tuple(float(value) for value in raw_diagonal)
    except (TypeError, ValueError):
        return None
    if not all(math.isfinite(value) and value >= 0.0 for value in diagonal):
        return None
    return max(diagonal)


def _bounded_items(value: Iterable[Any] | None, *, label: str) -> tuple[Any, ...]:
    if value is None:
        return ()
    try:
        items = tuple(islice(iter(value), MAX_BATCH_ITEMS + 1))
    except TypeError as exc:
        raise ValueError(f"{label} must be iterable") from exc
    if len(items) > MAX_BATCH_ITEMS:
        raise ValueError(f"{label} exceeds the {MAX_BATCH_ITEMS}-item bound")
    return items


def _bounded_text(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > MAX_SOURCE_TEXT_CHARS:
        raise ValueError(
            f"{label} exceeds the {MAX_SOURCE_TEXT_CHARS}-character bound"
        )
    return value


def _preferred_raw_header(primary: Any, fallback: Any) -> Any:
    stamp = getattr(primary, "stamp", None)
    stamp_sec, stamp_nanosec = _validated_stamp(stamp)
    frame_id = _bounded_text(
        getattr(primary, "frame_id", "") or "", label="frame id"
    )
    if frame_id or stamp_sec != 0 or stamp_nanosec != 0:
        return primary
    return fallback


def _validated_stamp(stamp: Any) -> tuple[int, int]:
    if stamp is None:
        return (0, 0)
    sec = getattr(stamp, "sec", 0)
    nanosec = getattr(stamp, "nanosec", 0)
    if type(sec) is not int:
        raise ValueError("stamp.sec must be an exact integer")
    if type(nanosec) is not int:
        raise ValueError("stamp.nanosec must be an exact integer")
    if not ROS_TIME_SEC_MIN <= sec <= ROS_TIME_SEC_MAX:
        raise ValueError("stamp.sec is outside the signed ROS Time range")
    if not 0 <= nanosec <= ROS_TIME_NANOSEC_MAX:
        raise ValueError("stamp.nanosec is outside the ROS Time range")
    return (sec, nanosec)


def main(argv: Sequence[str] | None = None) -> int:
    del argv
    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
