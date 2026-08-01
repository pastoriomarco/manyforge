"""Owner-neutral FoundationPose evidence and pre-effect gate contracts.

The four former live ROS cases remain exact I9/I acceptance input at
ManyForge commit 08222188293df8e12f90dc72e204c21f22f92ea1, blob
6e1af85d44d3304a3620cec30d1c83ff1f3ae9f5.  They are not runnable fallbacks
for the deleted live subscriber.
"""

from __future__ import annotations

import importlib.util
import inspect
import sys
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
QUALIFIER = REPO_ROOT / "scripts" / "ros" / "validate-foundationpose-detection3d-source.py"


def _load_qualifier() -> ModuleType:
    module_name = "manyforge_foundationpose_detection3d_qualifier_test_subject"
    spec = importlib.util.spec_from_file_location(module_name, QUALIFIER)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def _header(*, frame_id: str = "world", sec: int = 12, nanosec: int = 34):
    return SimpleNamespace(
        frame_id=frame_id,
        stamp=SimpleNamespace(sec=sec, nanosec=nanosec),
    )


def _result(*, class_id: str, score: float, variance: float):
    covariance = [0.0] * 36
    covariance[0] = variance
    covariance[7] = variance
    covariance[14] = variance
    return SimpleNamespace(
        hypothesis=SimpleNamespace(class_id=class_id, score=score),
        pose=SimpleNamespace(covariance=covariance),
    )


def test_entrypoint_is_unavailable_before_argument_parsing(capsys):
    qualifier = _load_qualifier()

    assert qualifier.main(["--not-a-real-option"]) == 78
    assert capsys.readouterr().err.strip() == qualifier.UNAVAILABLE_MESSAGE

    source = inspect.getsource(qualifier.main)
    assert "parse_args" not in source
    assert "rclpy" not in source
    assert "_qualify" not in source


def test_deployment_v0_source_config_and_cli_schema_are_absent():
    qualifier = _load_qualifier()

    assert not hasattr(qualifier, "_source_config")
    assert not hasattr(qualifier, "build_argument_parser")


def test_raw_auditor_matches_class_and_records_header_covariance():
    qualifier = _load_qualifier()
    auditor = qualifier.RawDetectionAuditor(
        detection_id="",
        class_id="mac_and_cheese",
        selection="",
    )
    message_header = _header()
    detection = SimpleNamespace(
        id="box-01",
        header=_header(frame_id="", sec=0, nanosec=0),
        results=[_result(class_id="mac_and_cheese", score=0.92, variance=0.001)],
    )

    auditor.observe(SimpleNamespace(header=message_header, detections=[detection]))

    assert auditor.evidence.seen_messages == 1
    assert auditor.evidence.matched_messages == 1
    assert auditor.evidence.last_frame_id == "world"
    assert auditor.evidence.last_detection_id == "box-01"
    assert auditor.evidence.last_class_id == "mac_and_cheese"
    assert auditor.evidence.last_score == pytest.approx(0.92)
    assert auditor.evidence.last_position_variance_m2 == pytest.approx(0.001)
    assert auditor.evidence.last_stamp_sec == 12
    assert auditor.evidence.last_stamp_nanosec == 34


def test_raw_auditor_best_score_selection_is_deterministic():
    qualifier = _load_qualifier()
    auditor = qualifier.RawDetectionAuditor(
        detection_id="",
        class_id="",
        selection="best_score",
    )
    detection = SimpleNamespace(
        id="box-01",
        header=_header(),
        results=[
            _result(class_id="low", score=0.25, variance=0.004),
            _result(class_id="high", score=0.95, variance=0.002),
        ],
    )

    auditor.observe(SimpleNamespace(header=_header(), detections=[detection]))

    assert auditor.evidence.matched_messages == 1
    assert auditor.evidence.last_selection == "best_score"
    assert auditor.evidence.last_class_id == "high"
    assert auditor.evidence.last_score == pytest.approx(0.95)
    assert auditor.evidence.last_position_variance_m2 == pytest.approx(0.002)


def test_position_covariance_rejects_short_or_nonfinite_values():
    qualifier = _load_qualifier()

    assert qualifier._position_covariance_max(None) is None
    assert qualifier._position_covariance_max([0.0] * 14) is None
    covariance = [0.0] * 36
    covariance[7] = float("nan")
    assert qualifier._position_covariance_max(covariance) is None


def test_raw_auditor_rejects_unbounded_or_malformed_message_fields():
    qualifier = _load_qualifier()
    auditor = qualifier.RawDetectionAuditor(
        detection_id="",
        class_id="mac_and_cheese",
        selection="",
    )

    with pytest.raises(ValueError, match="detections exceeds"):
        auditor.observe(
            SimpleNamespace(
                header=_header(),
                detections=(None for _ in range(qualifier.MAX_BATCH_ITEMS + 1)),
            )
        )

    oversized_result = _result(class_id="mac_and_cheese", score=0.9, variance=0.1)
    with pytest.raises(ValueError, match="detection results exceeds"):
        auditor.observe(
            SimpleNamespace(
                header=_header(),
                detections=[
                    SimpleNamespace(
                        id="box-01",
                        header=_header(),
                        results=(
                            oversized_result
                            for _ in range(qualifier.MAX_BATCH_ITEMS + 1)
                        ),
                    )
                ],
            )
        )

    with pytest.raises(ValueError, match="frame id exceeds"):
        auditor.observe(
            SimpleNamespace(
                header=_header(frame_id="x" * (qualifier.MAX_SOURCE_TEXT_CHARS + 1)),
                detections=[
                    SimpleNamespace(
                        id="box-01",
                        header=_header(frame_id="", sec=0, nanosec=0),
                        results=[oversized_result],
                    )
                ],
            )
        )


def test_raw_auditor_requires_one_bounded_association():
    qualifier = _load_qualifier()

    with pytest.raises(ValueError, match="exactly one"):
        qualifier.RawDetectionAuditor(
            detection_id="box-01",
            class_id="mac_and_cheese",
            selection="",
        )
    with pytest.raises(ValueError, match="selection must"):
        qualifier.RawDetectionAuditor(
            detection_id="",
            class_id="",
            selection="highest",
        )


@pytest.mark.parametrize(
    ("sec", "nanosec", "message"),
    [
        (True, 0, "stamp.sec must be an exact integer"),
        ("12", 0, "stamp.sec must be an exact integer"),
        (12, 0.5, "stamp.nanosec must be an exact integer"),
        (12, 1_000_000_000, "stamp.nanosec is outside"),
        (2**31, 0, "stamp.sec is outside"),
    ],
)
def test_raw_auditor_rejects_malformed_stamp_without_recording_evidence(
    sec, nanosec, message
):
    qualifier = _load_qualifier()
    auditor = qualifier.RawDetectionAuditor(
        detection_id="",
        class_id="mac_and_cheese",
        selection="",
    )
    detection = SimpleNamespace(
        id="box-01",
        header=_header(frame_id="world", sec=sec, nanosec=nanosec),
        results=[_result(class_id="mac_and_cheese", score=0.92, variance=0.001)],
    )

    with pytest.raises(ValueError, match=message):
        auditor.observe(SimpleNamespace(header=_header(), detections=[detection]))
    assert auditor.evidence.seen_messages == 0
    assert auditor.evidence.matched_messages == 0
