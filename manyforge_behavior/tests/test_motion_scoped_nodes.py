"""Tests for MoveUntilSignal + ProbeSearchMove BT nodes.

The tests run against MockKernelClient so they exercise the full
node lifecycle (execute → wait → outcome → tree status mapping)
without ROS. The mock's outcome factory lets each test pin the
terminal_cause it wants to verify."""

from __future__ import annotations

import math

import pytest
import py_trees

from manyforge_behavior.kernel_client import (
    MockKernelClient,
    MotionExecutionOutcome,
    MotionTripOutcome,
)
from manyforge_behavior.motion_runtime import MotionExecutionCoordinator
from manyforge_behavior.nodes.motion_scoped import (
    MoveUntilSignal,
    ProbeSearchMove,
)


@pytest.fixture(autouse=True)
def _reset_pytrees_blackboard():
    """py_trees.Blackboard is process-global; without resetting between
    tests, a value set in one test (e.g. `stop_requested=True`) leaks
    into the next and produces spurious failures."""
    py_trees.blackboard.Blackboard.clear()
    yield
    py_trees.blackboard.Blackboard.clear()


def _wire_blackboard(node, kernel):
    """Set up the blackboard the BT lifecycle expects."""
    coord = MotionExecutionCoordinator(kernel)
    node.setup()
    # py_trees blackboard requires the keys to exist before reads.
    bb = node.attach_blackboard_client()
    bb.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    bb.register_key(key="motion_coordinator", access=py_trees.common.Access.WRITE)
    bb.register_key(key="motion_error_message", access=py_trees.common.Access.WRITE)
    bb.kernel = kernel
    bb.motion_coordinator = coord


def _tick(node, max_iters: int = 8) -> py_trees.common.Status:
    """Tick the node to a terminal status (or return RUNNING after
    max_iters)."""
    for _ in range(max_iters):
        node.tick_once()
        if node.status != py_trees.common.Status.RUNNING:
            return node.status
    return node.status


def _move_until_signal(**overrides):
    kwargs = dict(
        robot_id="robot1",
        group_id="arm",
        move_id="probe_a",
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2"],
        joint_goal_positions_rad=[0.5, -0.5],
        policy_id="probe_trip",
        scope_id="probe_a_scope",
        stop_mode="spring_back_hold",
    )
    kwargs.update(overrides)
    return MoveUntilSignal(name="MoveUntilSignal", **kwargs)


def _probe_search_move(**overrides):
    kwargs = dict(
        robot_id="robot1",
        group_id="arm",
        move_id="probe_a",
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="pose_goal",
        pose_goal={"position_m": [0.4, 0.0, 0.2], "orientation": [0, 0, 0, 1]},
        policy_id="probe_trip",
        scope_id="probe_a_scope",
        stop_mode="spring_back_hold",
    )
    kwargs.update(overrides)
    return ProbeSearchMove(name="ProbeSearchMove", **kwargs)


# ----- MoveUntilSignal -----

def test_move_until_signal_signal_trip_outcome_returns_success():
    kernel = MockKernelClient()
    # MockKernelClient default for scoped moves is no_signal_trip;
    # override to signal_trip.
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause="signal_trip",
            trip=MotionTripOutcome(
                latch_id="latch-1",
                source_id="probe",
                policy_id=req.motion_interrupt_scopes[0].policy_id,
                scope_id=req.motion_interrupt_scopes[0].scope_id,
                executed_stop_mode="spring_back_hold",
                stop_command_source="native_simulated_stop",
                backend_stop_confirmed=True,
            ),
        )
    kernel.next_outcome_factory = factory  # type: ignore[assignment]

    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.SUCCESS, status
    assert kernel.last_execute_request is not None
    assert len(kernel.last_execute_request.motion_interrupt_scopes) == 1
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.policy_id == "probe_trip"
    assert scope.node_category == "generic"
    assert scope.stop_profile is not None
    assert scope.stop_profile.stop_mode == "spring_back_hold"


def test_move_until_signal_no_signal_trip_default_returns_success():
    # Default MoveUntilSignal treats no_signal_trip as ordinary
    # completion (success) — reaching the planned endpoint without a
    # trip is acceptable for generic search nodes.
    kernel = MockKernelClient()
    # MockKernelClient already produces no_signal_trip for scoped moves.
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.SUCCESS


def test_move_until_signal_no_signal_trip_with_strict_flag_returns_failure():
    kernel = MockKernelClient()
    node = _move_until_signal(not_found_is_failure=True)
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_move_until_signal_failed_outcome_returns_failure_with_message():
    kernel = MockKernelClient()
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause="failed",
            error_code="motion_interrupt_backend_stop_failed",
            error_message="controller refused decelerate_on_cancel",
            error_reason="hard_deceleration_not_canceled",
        )
    kernel.next_outcome_factory = factory  # type: ignore[assignment]
    # Mock kernel completes execute synchronously; we need the
    # motion_coordinator.update path to read FAILED state. The
    # MockKernelClient transitions to COMPLETED on synchronous
    # execute; for FAILED we'd need a different mock. Instead test
    # that the node maps outcome.terminal_cause=failed to FAILURE
    # even on the COMPLETED kernel path.
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_move_until_signal_interrupted_fault_returns_failure():
    kernel = MockKernelClient()
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause="interrupted_fault",
        )
    kernel.next_outcome_factory = factory  # type: ignore[assignment]
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_move_until_signal_writes_outcome_to_blackboard():
    kernel = MockKernelClient()
    node = _move_until_signal(scope_outcome_key="my_probe_outcome")
    _wire_blackboard(node, kernel)
    bb = node.attach_blackboard_client()
    bb.register_key(
        key="my_probe_outcome", access=py_trees.common.Access.READ
    )
    _tick(node)
    outcome = bb.get("my_probe_outcome")
    assert isinstance(outcome, MotionExecutionOutcome)
    assert outcome.terminal_cause == "no_signal_trip"


def test_move_until_signal_passes_stop_profile_fields():
    kernel = MockKernelClient()
    node = _move_until_signal(
        stop_mode="hard_deceleration",
        stop_max_time_s=0.5,
        stop_deceleration_time_s=0.2,
        allowed_fallback_modes=["spring_back_hold"],
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.stop_mode == "hard_deceleration"
    assert scope.stop_profile.max_stop_time_s == 0.5
    assert scope.stop_profile.deceleration_time_s == 0.2
    assert scope.stop_profile.allowed_fallback_modes == ["spring_back_hold"]
    assert scope.stop_profile.allow_natural_completion_on_short_path is False


def test_move_until_signal_soft_on_path_defaults_to_short_path_natural_completion():
    kernel = MockKernelClient()
    node = _move_until_signal(stop_mode="soft_on_path")
    _wire_blackboard(node, kernel)
    _tick(node)
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.allow_natural_completion_on_short_path is True


def test_move_until_signal_can_disable_short_path_natural_completion():
    kernel = MockKernelClient()
    node = _move_until_signal(
        stop_mode="soft_on_path",
        allow_natural_completion_on_short_path=False,
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.allow_natural_completion_on_short_path is False


def test_move_until_signal_passes_soft_on_path_joint_deceleration_limits():
    kernel = MockKernelClient()
    node = _move_until_signal(
        stop_mode="soft_on_path",
        stop_deceleration_time_s=0.35,
        per_joint_max_deceleration_rad_s2={"j1": 2.0, "j2": 1.5},
        allowed_fallback_modes=["spring_back_hold"],
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.stop_mode == "soft_on_path"
    assert scope.stop_profile.deceleration_time_s == 0.35
    assert scope.stop_profile.per_joint_max_deceleration_rad_s2 == {
        "j1": 2.0,
        "j2": 1.5,
    }


def test_move_until_signal_rejects_stop_deceleration_time_over_cap():
    kernel = MockKernelClient()
    node = _move_until_signal(stop_deceleration_time_s=2.001)
    _wire_blackboard(node, kernel)

    with pytest.raises(ValueError, match="deceleration_time_s must be <= 2.0"):
        _tick(node)


def test_move_until_signal_accepts_stop_deceleration_time_at_cap():
    kernel = MockKernelClient()
    node = _move_until_signal(stop_deceleration_time_s=2.0)
    _wire_blackboard(node, kernel)

    _tick(node)

    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.deceleration_time_s == 2.0


def test_move_until_signal_rejects_non_finite_per_joint_deceleration_limit():
    kernel = MockKernelClient()
    node = _move_until_signal(
        stop_mode="soft_on_path",
        stop_deceleration_time_s=0.35,
        per_joint_max_deceleration_rad_s2={"j1": math.nan},
    )
    _wire_blackboard(node, kernel)

    with pytest.raises(ValueError, match="limits must be finite"):
        _tick(node)


def test_move_until_signal_rejects_unknown_allowed_fallback_mode():
    kernel = MockKernelClient()
    node = _move_until_signal(allowed_fallback_modes=["not_a_stop_mode"])
    _wire_blackboard(node, kernel)

    with pytest.raises(ValueError, match="allowed_fallback_modes"):
        _tick(node)


# ----- ProbeSearchMove -----

def test_probe_search_move_signal_trip_returns_success():
    kernel = MockKernelClient()
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause="signal_trip",
            trip=MotionTripOutcome(
                latch_id="latch-1",
                executed_stop_mode="spring_back_hold",
                stop_command_source="native_simulated_stop",
                backend_stop_confirmed=True,
            ),
        )
    kernel.next_outcome_factory = factory  # type: ignore[assignment]
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.SUCCESS
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.node_category == "precision_contact"
    assert scope.activation_mode == "require_inactive_at_arm"


def test_probe_search_move_no_signal_trip_returns_failure_by_default():
    # Precision-contact node: reaching endpoint without a contact
    # is a failure (the probe MUST have touched).
    kernel = MockKernelClient()
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_probe_search_move_rejects_allowed_fallback_modes_at_construction():
    # The conjunctive-fallback rule (spec/210 §3.11) says precision-
    # contact node category defaults to empty fallback. We refuse the
    # construction entirely rather than silently dropping the field
    # at arm time.
    with pytest.raises(ValueError, match="ProbeSearchMove disallows"):
        _probe_search_move(allowed_fallback_modes=["hard_deceleration"])


def test_probe_search_move_disables_and_rejects_short_path_natural_completion():
    kernel = MockKernelClient()
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    _tick(node)
    scope = kernel.last_execute_request.motion_interrupt_scopes[0]
    assert scope.stop_profile.allow_natural_completion_on_short_path is False

    with pytest.raises(ValueError, match="disallows natural completion"):
        _probe_search_move(allow_natural_completion_on_short_path=True)


def test_move_until_signal_no_outcome_surface_fails_closed():
    # Reviewer Finding 2: a kernel client that doesn't implement
    # get_execution_outcome (returns None) MUST NOT be treated as
    # SUCCESS for a scoped move. Especially load-bearing for
    # ProbeSearchMove where "no proof of contact" would silently
    # certify a stop that never happened.
    class NoOutcomeKernel(MockKernelClient):
        def get_execution_outcome(self, execution_id):
            return None
    kernel = NoOutcomeKernel()
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_probe_search_move_no_outcome_surface_fails_closed():
    class NoOutcomeKernel(MockKernelClient):
        def get_execution_outcome(self, execution_id):
            return None
    kernel = NoOutcomeKernel()
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_move_until_signal_outcome_wrong_execution_id_fails_closed():
    # Defense in depth: even if get_execution_outcome returns SOMETHING,
    # if the execution_id doesn't match what the node owns we cannot
    # certify the move's terminal cause. Fail closed.
    class WrongIdKernel(MockKernelClient):
        def get_execution_outcome(self, execution_id):
            return MotionExecutionOutcome(
                execution_id=execution_id + 99,  # belongs to a different move
                move_id="some_other_move",
                terminal_cause="signal_trip",
            )
    kernel = WrongIdKernel()
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE


def test_move_until_signal_terminate_cancels_inflight_motion():
    # Reviewer Finding 3: terminate() must stop an in-flight scoped
    # motion when the BT halts the node. Otherwise a Reset / selector
    # preempt / tree rebuild orphans a scoped search/contact motion
    # on the controller with no BT owner — the worst kind of safety
    # hazard for precision contact.
    kernel = MockKernelClient(execute_duration_s=1.0)  # long-running
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    # First tick starts execution.
    node.tick_once()
    assert node.status == py_trees.common.Status.RUNNING
    assert kernel.stop_count == 0
    # Halt the node — py_trees calls terminate(INVALID).
    node.stop(py_trees.common.Status.INVALID)
    assert kernel.stop_count == 1, "terminate must cancel the owned execution"


def test_probe_search_move_terminate_cancels_inflight_motion():
    kernel = MockKernelClient(execute_duration_s=1.0)
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    node.tick_once()
    assert node.status == py_trees.common.Status.RUNNING
    assert kernel.stop_count == 0
    node.stop(py_trees.common.Status.INVALID)
    assert kernel.stop_count == 1


def test_move_until_signal_terminate_no_op_when_kernel_id_mismatches():
    # If the kernel has moved on to a different execution_id by the
    # time we terminate, we must NOT cancel — that would stop someone
    # else's motion.
    kernel = MockKernelClient(execute_duration_s=1.0)
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    node.tick_once()
    assert node.status == py_trees.common.Status.RUNNING
    # Force the kernel to advance its execution_id past ours.
    kernel._execution_id = 999  # type: ignore[attr-defined]
    node.stop(py_trees.common.Status.INVALID)
    assert kernel.stop_count == 0


# ----- Motion-field parity -----


def _wire_with_optional_keys(node, kernel, extra_keys=()):
    """Wire blackboard like _wire_blackboard, plus optional motion keys
    (stop_requested, global_max_tcp_linear_speed_mps, pose-source
    callbacks) so the parity tests can poke them. Returns the test's
    blackboard client (the one with WRITE access to the extra keys),
    so subsequent test code shares the same client handle."""
    _wire_blackboard(node, kernel)
    bb = node.attach_blackboard_client()
    for key in extra_keys:
        bb.register_key(key=key, access=py_trees.common.Access.WRITE)
    return bb


def test_move_until_signal_propagates_full_motion_field_set():
    # Without parity these silently fall back to ExecuteRequest defaults
    # and authored programs cannot control them per-move.
    kernel = MockKernelClient()
    node = _move_until_signal(
        velocity_scale=0.5,
        acceleration_scale=0.4,
        max_tcp_linear_speed_mps=0.25,
        joint_position_tol_rad=0.02,
        translation_tol_m=0.001,
        rotation_tol_rad=0.01,
        planning_timeout_s=3.0,
        execution_timeout_s=12.0,
        cache_mode="disable",
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    req = kernel.last_execute_request
    assert req is not None
    assert req.velocity_scale == 0.5
    assert req.acceleration_scale == 0.4
    assert req.max_tcp_linear_speed_mps == 0.25
    assert req.joint_position_tol_rad == 0.02
    assert req.translation_tol_m == 0.001
    assert req.rotation_tol_rad == 0.01
    assert req.planning_timeout_s == 3.0
    assert req.execution_timeout_s == 12.0
    assert req.cache_mode == "disable"


def test_probe_search_move_propagates_full_motion_field_set():
    kernel = MockKernelClient()
    node = _probe_search_move(
        velocity_scale=0.3,
        acceleration_scale=0.2,
        max_tcp_linear_speed_mps=0.05,
        joint_position_tol_rad=0.005,
        translation_tol_m=0.0001,
        rotation_tol_rad=0.002,
        planning_timeout_s=4.0,
        execution_timeout_s=8.0,
        cache_mode="disable",
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    req = kernel.last_execute_request
    assert req is not None
    assert req.velocity_scale == 0.3
    assert req.acceleration_scale == 0.2
    assert req.max_tcp_linear_speed_mps == 0.05
    assert req.joint_position_tol_rad == 0.005
    assert req.translation_tol_m == 0.0001
    assert req.rotation_tol_rad == 0.002
    assert req.planning_timeout_s == 4.0
    assert req.execution_timeout_s == 8.0
    assert req.cache_mode == "disable"


def test_global_tcp_cap_clamps_per_move_speed():
    # Global cap of 0.10 m/s must clamp the authored 0.50 m/s.
    kernel = MockKernelClient()
    node = _move_until_signal(max_tcp_linear_speed_mps=0.50)
    bb = _wire_with_optional_keys(
        node, kernel, extra_keys=("global_max_tcp_linear_speed_mps",)
    )
    bb.set("global_max_tcp_linear_speed_mps", 0.10)
    _tick(node)
    assert kernel.last_execute_request.max_tcp_linear_speed_mps == 0.10


def test_global_tcp_cap_fills_in_when_per_move_absent():
    kernel = MockKernelClient()
    node = _move_until_signal()  # no per-move cap
    bb = _wire_with_optional_keys(
        node, kernel, extra_keys=("global_max_tcp_linear_speed_mps",)
    )
    bb.set("global_max_tcp_linear_speed_mps", 0.15)
    _tick(node)
    assert kernel.last_execute_request.max_tcp_linear_speed_mps == 0.15


def test_stop_requested_blocks_new_motion_dispatch():
    # Operator-driven stop must prevent a scoped node from dispatching
    # a new motion. Without this gate, Reset/Stop wouldn't stop a queued
    # scoped trip from firing on the next tick — orphaning safety.
    kernel = MockKernelClient()
    node = _move_until_signal()
    bb = _wire_with_optional_keys(node, kernel, extra_keys=("stop_requested",))
    bb.set("stop_requested", True)
    node.tick_once()
    assert node.status == py_trees.common.Status.RUNNING
    assert kernel.last_execute_request is None, (
        "stop_requested must prevent the scoped execute from dispatching"
    )


def test_probe_search_move_stop_requested_blocks_new_motion_dispatch():
    kernel = MockKernelClient()
    node = _probe_search_move()
    bb = _wire_with_optional_keys(node, kernel, extra_keys=("stop_requested",))
    bb.set("stop_requested", True)
    node.tick_once()
    assert node.status == py_trees.common.Status.RUNNING
    assert kernel.last_execute_request is None


def test_pose_source_apply_callback_invoked_for_object_relative_move():
    # If the relative move references a scene object, the pose-source
    # apply callback must run before the kernel sees the request.
    kernel = MockKernelClient()
    apply_calls: list[dict] = []

    def apply_callback(*, object_ids, reason, action_span):
        apply_calls.append(
            {
                "object_ids": list(object_ids),
                "reason": reason,
                "action_span": action_span,
            }
        )
        return {"success": True}

    node = _move_until_signal(
        motion_type="relative_move",
        joint_goal_names=[],
        joint_goal_positions_rad=[],
        relative_move={
            "target_reference": {"type": "object", "id": "part_a"},
            "offset": {"position_m": [0.01, 0, 0]},
        },
    )
    bb = _wire_with_optional_keys(
        node,
        kernel,
        extra_keys=(
            "pose_source_apply_callback",
            "pose_source_between_nodes_callback",
        ),
    )
    bb.set("pose_source_apply_callback", apply_callback)
    _tick(node)
    assert apply_calls, "pose-source apply callback must be invoked"
    assert apply_calls[0]["object_ids"] == []
    assert apply_calls[0]["action_span"].action_source == "program_node"
    assert "MoveUntilSignal" in (
        apply_calls[0]["action_span"].node_instance_id or ""
    )


def test_pose_source_apply_callback_failure_surfaces_as_failure():
    # When the pose-source apply callback fails, the node must FAIL
    # (with motion_error_message set) rather than dispatching with a
    # stale scene pose.
    kernel = MockKernelClient()

    def failing_callback(*, object_ids, reason, action_span):
        _ = (object_ids, reason, action_span)
        return {"success": False, "error": "pose source disconnected"}

    node = _move_until_signal(
        motion_type="relative_move",
        joint_goal_names=[],
        joint_goal_positions_rad=[],
        relative_move={
            "target_reference": {"type": "object", "id": "part_a"},
            "offset": {"position_m": [0.01, 0, 0]},
        },
    )
    bb = _wire_with_optional_keys(
        node,
        kernel,
        extra_keys=(
            "pose_source_apply_callback",
            "pose_source_between_nodes_callback",
            "motion_error_message",
        ),
    )
    bb.set("pose_source_apply_callback", failing_callback)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    assert kernel.last_execute_request is None
    assert "pose source disconnected" in str(bb.get("motion_error_message"))


def test_pose_goal_rpy_normalized_to_xyzw_quaternion():
    # When pose_goal arrives with orientation_rpy_deg, the node must
    # normalize it to orientation_xyzw before submitting to the kernel.
    kernel = MockKernelClient()
    node = _probe_search_move(
        pose_goal={
            "position_m": [0.4, 0.0, 0.2],
            "orientation_rpy_deg": [0.0, 0.0, 90.0],
        },
    )
    _wire_blackboard(node, kernel)
    _tick(node)
    sent_pose = kernel.last_execute_request.pose_goal
    assert "orientation_xyzw" in sent_pose
    assert "orientation_rpy_deg" not in sent_pose


# ----- Failure observability -----
#
# Every FAILURE terminal cause must set motion_error_message so the
# cycle manager / operator surface gets a concrete reason. Without
# this, the operator sees a FAILURE with no explanation — opaque is
# the worst kind of fault for industrial contact/search.


def _outcome_factory(cause: str):
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause=cause,
        )
    return factory


def _read_motion_error(node):
    bb = node.attach_blackboard_client()
    bb.register_key(
        key="motion_error_message", access=py_trees.common.Access.READ
    )
    return bb.get("motion_error_message")


def test_probe_search_move_no_signal_trip_failure_writes_message():
    # ProbeSearchMove defaults not_found_is_failure=True; reaching the
    # endpoint without contact must surface "probe did not contact".
    kernel = MockKernelClient()  # default outcome is no_signal_trip
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    msg = _read_motion_error(node)
    assert msg is not None
    assert "probe did not contact" in msg


def test_move_until_signal_strict_no_signal_trip_writes_message():
    kernel = MockKernelClient()
    node = _move_until_signal(not_found_is_failure=True)
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    msg = _read_motion_error(node)
    assert msg is not None
    assert "probe did not contact" in msg


def test_scoped_node_completed_normally_writes_message():
    kernel = MockKernelClient()
    kernel.next_outcome_factory = _outcome_factory("completed_normally")
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    msg = _read_motion_error(node)
    assert msg is not None
    assert "completed_normally" in msg


def test_scoped_node_stopped_writes_message():
    kernel = MockKernelClient()
    kernel.next_outcome_factory = _outcome_factory("stopped")
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    msg = _read_motion_error(node)
    assert msg is not None
    assert "operator stop" in msg


def test_scoped_node_interrupted_fault_writes_message():
    kernel = MockKernelClient()
    kernel.next_outcome_factory = _outcome_factory("interrupted_fault")
    node = _move_until_signal()
    _wire_blackboard(node, kernel)
    status = _tick(node)
    assert status == py_trees.common.Status.FAILURE
    msg = _read_motion_error(node)
    assert msg is not None
    assert "fault session" in msg


def test_probe_search_move_writes_outcome_with_trip_details():
    kernel = MockKernelClient()
    def factory(eid, req):
        return MotionExecutionOutcome(
            execution_id=eid,
            move_id=req.move_id,
            terminal_cause="signal_trip",
            trip=MotionTripOutcome(
                latch_id="latch-1",
                executed_stop_mode="spring_back_hold",
                stop_command_source="fjt_preempt_hold",
                backend_stop_confirmed=True,
                stop_distance_rad=0.01,
            ),
        )
    kernel.next_outcome_factory = factory  # type: ignore[assignment]
    node = _probe_search_move()
    _wire_blackboard(node, kernel)
    bb = node.attach_blackboard_client()
    bb.register_key(
        key="motion_scope_outcome", access=py_trees.common.Access.READ
    )
    _tick(node)
    outcome = bb.get("motion_scope_outcome")
    assert isinstance(outcome, MotionExecutionOutcome)
    assert outcome.terminal_cause == "signal_trip"
    assert outcome.trip is not None
    assert outcome.trip.executed_stop_mode == "spring_back_hold"
    assert outcome.trip.stop_distance_rad == 0.01


def test_scoped_nodes_attach_program_run_id_from_blackboard():
    """Scoped motion nodes must carry the same spec/500 §2.2 correlation id
    as plain moves — a probe move's kernel events are part of the run's
    story too."""
    kernel = MockKernelClient()
    node = _move_until_signal()
    _wire_blackboard(node, kernel)

    bb = py_trees.blackboard.Client(name="test_program_run_id_writer")
    bb.register_key(key="program_run_id", access=py_trees.common.Access.WRITE)
    bb.program_run_id = "run-test-cafe0123"

    _tick(node)
    assert kernel.last_execute_request is not None
    assert kernel.last_execute_request.program_run_id == "run-test-cafe0123"
    assert kernel.last_execute_request.action_span is not None
    assert (
        kernel.last_execute_request.action_span.program_run_id
        == "run-test-cafe0123"
    )
    assert (
        kernel.last_execute_request.action_span.action_source
        == "program_node"
    )


def test_scoped_nodes_execute_without_program_run_id():
    """Standalone trees (no cycle manager) must run identically without it."""
    kernel = MockKernelClient()
    node = _probe_search_move()
    _wire_blackboard(node, kernel)

    _tick(node)
    assert kernel.last_execute_request is not None
    assert kernel.last_execute_request.program_run_id is None
