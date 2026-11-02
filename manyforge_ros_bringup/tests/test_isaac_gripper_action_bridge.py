"""Tests for Isaac gripper bridge helper logic."""

from __future__ import annotations

import pytest

from manyforge_ros_bringup.isaac_gripper_command import (
    SettledCompletionWindow,
    SingleGoalReservation,
    coherent_joint_positions,
    joint_state_contract_ready,
    joint_positions_match,
    log_readiness_transition,
    map_gripper_positions,
    select_pneumatic_effort,
)


class _RecordingLogger:
    def __init__(self) -> None:
        self.records: list[tuple[str, str]] = []

    def info(self, message: str) -> None:
        self.records.append(("info", message))

    def warning(self, message: str) -> None:
        self.records.append(("warning", message))


def test_readiness_transition_logging_uses_stable_severity_paths() -> None:
    logger = _RecordingLogger()

    log_readiness_transition(
        logger,
        ready=False,
        previous=None,
        required_joint_names=["finger_joint"],
    )
    log_readiness_transition(
        logger,
        ready=True,
        previous=False,
        required_joint_names=["finger_joint"],
    )
    log_readiness_transition(
        logger,
        ready=False,
        previous=True,
        required_joint_names=["finger_joint"],
    )

    assert [level for level, _ in logger.records] == [
        "info",
        "info",
        "warning",
    ]
    assert [message.split(":", 1)[0] for _, message in logger.records] == [
        "Isaac gripper state contract not ready",
        "Isaac gripper state contract ready",
        "Isaac gripper state contract not ready",
    ]
    assert all("finger_joint" in message for _, message in logger.records)


def test_joint_state_normalization_rejects_ambiguous_or_invalid_samples() -> None:
    assert coherent_joint_positions(["a", "b"], [1.0, 2.0]) == {
        "a": 1.0,
        "b": 2.0,
    }
    assert coherent_joint_positions(["a", "a"], [1.0, 1.0]) is None
    assert coherent_joint_positions(["a", "b"], [1.0]) is None
    assert coherent_joint_positions(["a"], [1.0, 2.0]) is None
    assert coherent_joint_positions(["a"], [float("inf")]) is None


def test_gripper_goal_reservation_allows_only_one_active_goal() -> None:
    reservation = SingleGoalReservation()

    assert reservation.try_acquire()
    assert not reservation.try_acquire()
    reservation.release()
    assert reservation.try_acquire()


def test_select_pneumatic_effort_uses_close_effort_for_closed_target() -> None:
    assert select_pneumatic_effort(
        target_position=0.0,
        open_position=0.0081,
        closed_position=0.0,
        open_effort=5.0,
        close_effort=-5.0,
    ) == pytest.approx(-5.0)


def test_select_pneumatic_effort_uses_open_effort_for_open_target() -> None:
    assert select_pneumatic_effort(
        target_position=0.0081,
        open_position=0.0081,
        closed_position=0.0,
        open_effort=5.0,
        close_effort=-5.0,
    ) == pytest.approx(5.0)


def test_map_gripper_positions_preserves_nominal_joint_target() -> None:
    assert map_gripper_positions(
        command_position=0.0081,
        multipliers=[1.0],
        offsets=[0.0],
    ) == pytest.approx([0.0081])


def test_robotiq_completion_mapping_requires_every_mimic_joint() -> None:
    names = [
        "finger_joint",
        "left_inner_finger_joint",
        "left_inner_knuckle_joint",
        "right_inner_knuckle_joint",
        "right_outer_knuckle_joint",
        "right_inner_finger_joint",
    ]
    targets = map_gripper_positions(
        command_position=0.55,
        multipliers=[1.0, 1.0, -1.0, -1.0, -1.0, 1.0],
        offsets=[0.0] * 6,
    )
    observed = dict(zip(names, targets))

    assert joint_positions_match(observed, names, targets, tolerance=0.01)
    observed["right_outer_knuckle_joint"] = 0.0
    assert not joint_positions_match(observed, names, targets, tolerance=0.01)


def test_gripper_state_contract_requires_complete_finite_topology() -> None:
    required = ["finger_joint", "right_inner_finger_joint"]

    assert joint_state_contract_ready(
        {"finger_joint": 0.1, "right_inner_finger_joint": 0.1}, required
    )
    assert not joint_state_contract_ready({"finger_joint": 0.1}, required)
    assert not joint_state_contract_ready(
        {"finger_joint": 0.1, "right_inner_finger_joint": float("nan")}, required
    )


def test_completion_window_requires_distinct_samples_and_elapsed_settle_time() -> None:
    window = SettledCompletionWindow(settle_time_s=0.1, min_samples=3)

    assert not window.observe(now=1.0, generation=10, matches=True)
    assert not window.observe(now=1.04, generation=10, matches=True)
    assert not window.observe(now=1.05, generation=11, matches=True)
    assert window.observe(now=1.11, generation=12, matches=True)


def test_completion_window_resets_after_any_linkage_mismatch() -> None:
    window = SettledCompletionWindow(settle_time_s=0.05, min_samples=2)

    assert not window.observe(now=1.0, generation=1, matches=True)
    assert not window.observe(now=1.1, generation=2, matches=False)
    assert not window.observe(now=1.2, generation=3, matches=True)
    assert window.observe(now=1.26, generation=4, matches=True)


def test_completion_window_ignores_precommand_and_out_of_order_samples() -> None:
    window = SettledCompletionWindow(
        settle_time_s=0.0,
        min_samples=2,
        last_generation=10,
    )

    assert not window.observe(now=1.0, generation=10, matches=True)
    assert not window.observe(now=1.1, generation=9, matches=True)
    assert not window.observe(now=1.2, generation=11, matches=True)
    assert window.observe(now=1.3, generation=12, matches=True)


def test_completion_mapping_rejects_non_finite_evidence() -> None:
    assert not joint_positions_match(
        {"finger_joint": float("nan")},
        ["finger_joint"],
        [0.5],
        tolerance=0.04,
    )

    with pytest.raises(ValueError, match="command_position must be finite"):
        map_gripper_positions(float("nan"), [1.0], [0.0])


def test_gripper_hold_and_readiness_no_longer_depend_on_arm_gate() -> None:
    from types import SimpleNamespace
    import threading
    from manyforge_ros_bringup.isaac_gripper_action_bridge import IsaacGripperActionBridge

    writes: list[list[float]] = []
    ready_updates: list[bool] = []
    bridge = SimpleNamespace(
        _lock=threading.Lock(), _active_target_positions=[0.25],
        _publish_command=writes.append,
        _publish_readiness_if_due=lambda: ready_updates.append(True),
    )
    IsaacGripperActionBridge._reconcile(bridge)
    assert writes == [[0.25]]
    assert ready_updates == [True]
    bridge._active_target_positions = None
    IsaacGripperActionBridge._reconcile(bridge)
    assert writes == [[0.25]]
    assert ready_updates == [True, True]


def test_cancel_holds_current_fingers_before_accepting() -> None:
    """N10b: like the stock controller, the accepted cancel already holds."""
    from types import SimpleNamespace
    import threading
    from rclpy.action import CancelResponse
    from manyforge_ros_bringup.isaac_gripper_action_bridge import IsaacGripperActionBridge

    writes: list[list[float]] = []
    bridge = SimpleNamespace(
        _lock=threading.Lock(), _goal_cancelled=False,
        _active_target_positions=[0.0],
        _latest_positions={"finger_joint": 0.31, "left_inner_finger_joint": -0.31},
        _commanded_joint="finger_joint",
        _command_multipliers=[1.0], _command_offsets=[0.0],
        _publish_command=writes.append,
        _publish_readiness_if_due=lambda: None,
    )
    assert IsaacGripperActionBridge._cancel_callback(bridge, object()) == CancelResponse.ACCEPT
    assert writes == [[0.31]]
    assert bridge._goal_cancelled is True
    assert bridge._active_target_positions is None
    # The periodic reconcile no longer republishes the old target.
    IsaacGripperActionBridge._reconcile(bridge)
    assert writes == [[0.31]]
