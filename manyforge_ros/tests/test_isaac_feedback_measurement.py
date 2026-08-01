from __future__ import annotations

import importlib.util
import inspect
import sys
from pathlib import Path
from types import ModuleType, SimpleNamespace

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
MEASURER = REPO_ROOT / "scripts" / "ros" / "measure-isaac-controller-feedback.py"


def _load_measurer() -> ModuleType:
    module_name = "manyforge_isaac_feedback_measurement_test_subject"
    spec = importlib.util.spec_from_file_location(module_name, MEASURER)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def test_entrypoint_is_unavailable_before_argument_parsing(capsys):
    measurer = _load_measurer()

    assert measurer.main(["--not-a-real-option"]) == 78
    assert capsys.readouterr().err.strip() == measurer.UNAVAILABLE_MESSAGE

    source = inspect.getsource(measurer.main)
    assert "parse_args" not in source
    assert "rclpy" not in source
    assert "FeedbackProbe" not in source


def test_topic_stats_summarizes_cadence():
    measurer = _load_measurer()
    stats = measurer.TopicStats()

    stats.observe(10.0)
    stats.observe(10.5)
    stats.observe(11.0)

    assert stats.summary() == {
        "count": 3,
        "hz": pytest.approx(2.0),
        "duration_s": pytest.approx(1.0),
        "interval_min_s": pytest.approx(0.5),
        "interval_mean_s": pytest.approx(0.5),
        "interval_max_s": pytest.approx(0.5),
    }


@pytest.mark.parametrize("clock", (True, float("nan"), float("inf"), -1.0))
def test_topic_stats_rejects_nonfinite_nonreal_or_negative_clocks(clock):
    measurer = _load_measurer()
    stats = measurer.TopicStats()

    with pytest.raises((TypeError, ValueError)):
        stats.observe(clock)


def test_topic_stats_is_online_bounded_and_rejects_clock_regression():
    measurer = _load_measurer()
    stats = measurer.TopicStats()
    assert not hasattr(stats, "intervals_s")

    stats.observe(1.0)
    with pytest.raises(ValueError, match="monotonic"):
        stats.observe(0.9)
    assert stats.count == 1

    stats.count = measurer.MAX_TOPIC_SAMPLES
    with pytest.raises(ValueError, match="sample bound"):
        stats.observe(1.1)


def test_feedback_accumulator_reduces_messages_without_ros_ownership():
    measurer = _load_measurer()
    args = measurer.build_argument_parser().parse_args(
        ["--required-joints", "joint_a", "joint_b"]
    )
    times = iter([1.0, 1.1, 1.2, 1.3, 2.0])
    accumulator = measurer.FeedbackEvidenceAccumulator(args, now=times.__next__)
    accumulator.observe_controller_state(
        SimpleNamespace(
            joint_names=["joint_a", "joint_b"],
            feedback=SimpleNamespace(positions=[0.2, 0.4]),
            actual=None,
            error=SimpleNamespace(positions=[-0.01, 0.03]),
        )
    )
    accumulator.observe_joint_state(
        SimpleNamespace(name=["joint_a", "joint_b"], position=[0.21, 0.39])
    )
    accumulator.observe_isaac_joint_state(
        SimpleNamespace(name=["joint_a"], position=[0.22])
    )

    result = accumulator.result()

    assert result["duration_s"] == pytest.approx(1.0)
    assert result["controller_tracking"] == {
        "max_abs_feedback_error_rad": pytest.approx(0.03),
        "max_abs_feedback_error_joint": "joint_b",
    }
    assert result["latest_positions"]["controller_state"] == {
        "joint_a": pytest.approx(0.2),
        "joint_b": pytest.approx(0.4),
    }
    assert result["missing_required_samples"] == {
        "controller_state": 0,
        "joint_states": 0,
        "isaac_joint_states": 1,
    }


def test_positions_by_name_rejects_mismatched_duplicate_or_invalid_vectors():
    measurer = _load_measurer()

    with pytest.raises(ValueError, match="equal length"):
        measurer._positions_by_name(["joint_a", "joint_b"], [0.5])
    with pytest.raises(ValueError, match="duplicate"):
        measurer._positions_by_name(["joint_a", "joint_a"], [0.5, 0.6])
    with pytest.raises(TypeError, match="real number, not bool"):
        measurer._positions_by_name(["joint_a"], [True])
    with pytest.raises(ValueError, match="finite"):
        measurer._positions_by_name(["joint_a"], [float("inf")])
    with pytest.raises(ValueError, match="item bound"):
        measurer._positions_by_name(
            [f"joint_{index}" for index in range(measurer.MAX_JOINTS + 1)],
            [0.0] * (measurer.MAX_JOINTS + 1),
        )


def test_feedback_accumulator_rejects_duplicate_identity_and_clock_regression():
    measurer = _load_measurer()
    parser = measurer.build_argument_parser()
    duplicate_joints = parser.parse_args(
        ["--required-joints", "joint_a", "joint_a"]
    )
    with pytest.raises(ValueError, match="duplicate"):
        measurer.FeedbackEvidenceAccumulator(duplicate_joints, now=lambda: 1.0)

    duplicate_topics = parser.parse_args(
        [
            "--controller-state-topic",
            "/same",
            "--joint-states-topic",
            "/same",
            "--required-joints",
            "joint_a",
        ]
    )
    with pytest.raises(ValueError, match="distinct"):
        measurer.FeedbackEvidenceAccumulator(duplicate_topics, now=lambda: 1.0)

    args = parser.parse_args(["--required-joints", "joint_a"])
    times = iter([1.0, 0.9])
    accumulator = measurer.FeedbackEvidenceAccumulator(args, now=times.__next__)
    with pytest.raises(ValueError, match="regressed"):
        accumulator.observe_joint_state(
            SimpleNamespace(name=["joint_a"], position=[0.0])
        )
    assert accumulator.joint_state_stats.count == 0
