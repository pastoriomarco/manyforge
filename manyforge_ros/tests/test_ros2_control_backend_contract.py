from __future__ import annotations

import importlib
import threading
import time
import uuid
from pathlib import Path

import pytest


JOINTS = [
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
]
START_POSITIONS = [0.0, -1.0, 1.2, -1.5, -1.57, 0.0]
GOAL_POSITIONS = [0.01, -1.0, 1.2, -1.5, -1.57, 0.0]
URDF_PATH = Path("/usr/local/share/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf")
SRDF_PATH = Path("/usr/local/share/roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf")
UR10E_ROBOTIQ_URDF_PATH = Path(
    "/workspace/manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.urdf"
)
UR10E_ROBOTIQ_SRDF_PATH = Path(
    "/workspace/manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.srdf"
)


def _native_module():
    if not URDF_PATH.exists() or not SRDF_PATH.exists():
        pytest.skip("RoboPlan example UR5 model is not installed")
    try:
        module = importlib.import_module("manyforge_roboplan_py")
    except ImportError:
        pytest.skip("manyforge_roboplan_py is not built in this test environment")
    if not hasattr(module, "create_ros2_control_kernel_from_paths"):
        pytest.skip("manyforge_roboplan_py was built without ros2_control support")
    return module


def _create_bundle(
    *,
    action_name: str,
    joint_state_topic: str,
    controller_state_topic: str,
    wait_s: float,
    config_overrides: dict | None = None,
    model_name: str = "ur5_test",
    urdf_path: Path = URDF_PATH,
    srdf_path: Path = SRDF_PATH,
    package_paths: list[str] | None = None,
):
    module = _native_module()
    config = {
        "action_name": action_name,
        "joint_state_topic": joint_state_topic,
        "controller_state_topic": controller_state_topic,
        "required_joints": JOINTS,
        "wait_for_state_timeout_s": wait_s,
        "wait_for_action_server_timeout_s": 1.0,
        "joint_state_max_age_ms": 250,
        "start_tolerance_rad": 0.05,
        "final_tolerance_rad": 0.05,
        "require_fresh_state_before_planning": True,
        "execution_state_source_mode": "observed_test",
    }
    if config_overrides:
        config.update(config_overrides)
    package_paths = package_paths or ["/usr/local/share"]
    return module.create_ros2_control_kernel_from_paths(
        model_name,
        "arm",
        str(urdf_path),
        str(srdf_path),
        package_paths,
        config,
        None,
    )


def _execute_joint_goal(bundle, move_id: str):
    return bundle.execute(
        {
            "robot_id": "ur5",
            "group_id": "arm",
            "move_id": move_id,
            "base_frame": "world",
            "tcp_frame": "tool0",
            "motion_type": "joint_goal",
            "joint_goal_names": JOINTS,
            "joint_goal_positions_rad": GOAL_POSITIONS,
            "planning_timeout_s": 1.0,
            "execution_timeout_s": 2.0,
            "joint_position_tol_rad": 0.05,
        }
    )


def test_ros2_control_backend_requires_fresh_state_before_planning():
    suffix = uuid.uuid4().hex
    bundle = _create_bundle(
        action_name=f"/manyforge_contract_{suffix}/follow_joint_trajectory",
        joint_state_topic=f"/manyforge_contract_{suffix}/joint_states",
        controller_state_topic=f"/manyforge_contract_{suffix}/controller_state",
        wait_s=0.05,
    )

    result = _execute_joint_goal(bundle, "no_fresh_observed_state")

    assert result["ok"] is False
    assert "No fresh complete observed robot state before planning" in result["error"]
    bundle.reset()


def test_ros2_control_backend_rejects_sim_tolerance_override_outside_commanded_sim():
    suffix = uuid.uuid4().hex

    with pytest.raises(ValueError, match="disable_controller_path_tolerances"):
        _create_bundle(
            action_name=f"/manyforge_contract_tolerance_guard_{suffix}/follow_joint_trajectory",
            joint_state_topic=f"/manyforge_contract_tolerance_guard_{suffix}/joint_states",
            controller_state_topic=f"/manyforge_contract_tolerance_guard_{suffix}/controller_state",
            wait_s=0.05,
            config_overrides={
                "execution_state_source_mode": "observed_robot",
                "disable_controller_path_tolerances": True,
            },
        )


def test_ros2_control_commanded_sim_uses_backend_initial_state_without_observed_feed():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")
    pytest.importorskip("std_msgs")

    from rclpy.executors import MultiThreadedExecutor
    from std_msgs.msg import Bool

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    suffix = uuid.uuid4().hex
    action_name = f"/manyforge_contract_commanded_sim_{suffix}/follow_joint_trajectory"
    joint_state_topic = f"/manyforge_contract_commanded_sim_{suffix}/joint_states"
    controller_state_topic = (
        f"/manyforge_contract_commanded_sim_{suffix}/controller_state"
    )
    command_gate_topic = f"/manyforge_contract_commanded_sim_{suffix}/command_enable"
    initial_positions = [0.0, -1.57, 1.57, -1.57, -1.57, 0.0]

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_commanded_sim_{suffix}")
    fake_server = None
    bundle = None
    gate_messages: list[bool] = []

    try:
        fake_server = FakeFollowJointTrajectoryServer(node, action_name=action_name)
        node.create_subscription(
            Bool,
            command_gate_topic,
            lambda msg: gate_messages.append(bool(msg.data)),
            10,
        )
        executor.add_node(node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        spin_thread.start()
        time.sleep(0.2)

        bundle = _create_bundle(
            action_name=action_name,
            joint_state_topic=joint_state_topic,
            controller_state_topic=controller_state_topic,
            wait_s=0.05,
            config_overrides={
                "execution_state_source_mode": "commanded_sim",
                "require_fresh_state_before_planning": False,
                "initial_joint_positions_rad": initial_positions,
                "command_gate_topic": command_gate_topic,
                "sim_reset_motion_duration_s": 0.01,
                "wait_for_action_server_timeout_s": 3.0,
                "disable_controller_path_tolerances": True,
            },
        )

        reset = bundle.reset()
        assert reset["accepted"] is True, reset

        deadline = time.time() + 1.0
        while time.time() < deadline and True not in gate_messages:
            time.sleep(0.01)
        assert True in gate_messages

        assert fake_server.goals
        reset_goal = fake_server.goals[0]
        assert reset_goal.joint_names == JOINTS
        assert reset_goal.point_count == 1
        assert reset_goal.points_positions == [initial_positions]
        assert reset_goal.path_tolerance_positions == [-1.0] * len(JOINTS)
        assert reset_goal.goal_tolerance_positions == [-1.0] * len(JOINTS)

        snapshot = bundle.get_execution_snapshot()
        assert snapshot["state_name"] == "Idle"
        assert snapshot["joint_positions_rad"] == initial_positions

        # No joint-state/controller-state publisher is running in this test.
        # commanded_sim must still plan and execute from the backend-authored
        # state instead of adopting or waiting for observed simulator pose.
        result = _execute_joint_goal(bundle, "commanded_sim_backend_truth")
        assert result["ok"] is True
        deadline = time.time() + 2.0
        while time.time() < deadline and len(fake_server.goals) < 2:
            time.sleep(0.01)
        assert len(fake_server.goals) >= 2
        exec_goal = fake_server.goals[1]
        assert exec_goal.path_tolerance_positions == [-1.0] * len(JOINTS)
        assert exec_goal.goal_tolerance_positions == [-1.0] * len(JOINTS)
    finally:
        if bundle is not None:
            try:
                bundle.reset()
            except RuntimeError:
                pass
        if fake_server is not None:
            fake_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_streaming_end_waits_for_terminal_soft_stop_result():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")

    from rclpy.executors import MultiThreadedExecutor

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    suffix = uuid.uuid4().hex
    action_name = (
        f"/manyforge_contract_streaming_terminal_{suffix}/follow_joint_trajectory"
    )
    joint_state_topic = f"/manyforge_contract_streaming_terminal_{suffix}/joint_states"
    controller_state_topic = (
        f"/manyforge_contract_streaming_terminal_{suffix}/controller_state"
    )

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=3)
    node = rclpy.create_node(f"manyforge_contract_streaming_terminal_{suffix}")
    fake_server = None
    bundle = None

    try:
        fake_server = FakeFollowJointTrajectoryServer(
            node,
            action_name=action_name,
            hold_after_first_feedback_s=0.25,
        )
        executor.add_node(node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        spin_thread.start()
        time.sleep(0.2)

        bundle = _create_bundle(
            action_name=action_name,
            joint_state_topic=joint_state_topic,
            controller_state_topic=controller_state_topic,
            wait_s=0.05,
            config_overrides={
                "execution_state_source_mode": "commanded_sim",
                "require_fresh_state_before_planning": False,
                "initial_joint_positions_rad": START_POSITIONS,
                "wait_for_action_server_timeout_s": 3.0,
                "stop_completion_timeout_s": 1.0,
                "disable_controller_path_tolerances": True,
            },
        )
        reset = bundle.reset()
        assert reset["accepted"] is True, reset
        initial_goal_count = len(fake_server.goals)

        begin = bundle.begin_streaming_session(
            {
                "group_id": "arm",
                "joint_names": JOINTS,
                "input_mode": "joint",
                "target_time_from_start_min_s": 0.03,
                "soft_stop_deceleration_time_s": 0.05,
            }
        )
        assert begin["accepted"] is True, begin

        started = time.monotonic()
        ended = bundle.end_streaming_session(begin["session_id"], "spring_back")
        elapsed_s = time.monotonic() - started

        assert ended["accepted"] is True, ended
        assert elapsed_s >= 0.20
        assert len(fake_server.goals) >= initial_goal_count + 1
    finally:
        if bundle is not None:
            try:
                bundle.reset()
            except RuntimeError:
                pass
        if fake_server is not None:
            fake_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_backend_preserves_fjt_abort_diagnostics():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")
    pytest.importorskip("trajectory_msgs")

    from control_msgs.msg import JointTrajectoryControllerState
    from rclpy.executors import MultiThreadedExecutor
    from sensor_msgs.msg import JointState
    from trajectory_msgs.msg import JointTrajectoryPoint

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    suffix = uuid.uuid4().hex
    action_name = f"/manyforge_contract_abort_{suffix}/follow_joint_trajectory"
    joint_state_topic = f"/manyforge_contract_abort_{suffix}/joint_states"
    controller_state_topic = f"/manyforge_contract_abort_{suffix}/controller_state"

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_abort_{suffix}")
    stop_event = threading.Event()
    fake_server = None
    bundle = None

    try:
        fake_server = FakeFollowJointTrajectoryServer(
            node,
            action_name=action_name,
            abort_after_feedback=True,
            feedback_error_positions=[0.0, 0.0, 0.0, 0.0, 0.0, -0.203],
            abort_error_string="Aborted due to path tolerance violation",
        )
        joint_pub = node.create_publisher(JointState, joint_state_topic, 10)
        state_pub = node.create_publisher(
            JointTrajectoryControllerState,
            controller_state_topic,
            10,
        )

        def publish_state_loop() -> None:
            while not stop_event.is_set():
                joint_state = JointState()
                joint_state.name = list(JOINTS)
                joint_state.position = list(START_POSITIONS)
                joint_pub.publish(joint_state)

                controller_state = JointTrajectoryControllerState()
                controller_state.joint_names = list(JOINTS)
                controller_state.feedback = JointTrajectoryPoint()
                controller_state.feedback.positions = list(START_POSITIONS)
                controller_state.error = JointTrajectoryPoint()
                controller_state.error.positions = [0.0] * len(JOINTS)
                state_pub.publish(controller_state)
                time.sleep(0.01)

        executor.add_node(node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        publish_thread = threading.Thread(target=publish_state_loop, daemon=True)
        spin_thread.start()
        publish_thread.start()

        bundle = _create_bundle(
            action_name=action_name,
            joint_state_topic=joint_state_topic,
            controller_state_topic=controller_state_topic,
            wait_s=5.0,
        )
        time.sleep(1.5)

        result = _execute_joint_goal(bundle, "fake_abort_diagnostic")
        assert result["ok"] is True

        deadline = time.monotonic() + 5.0
        snapshot = {}
        while time.monotonic() < deadline:
            snapshot = bundle.get_execution_snapshot()
            if snapshot.get("error"):
                break
            time.sleep(0.05)

        error = snapshot.get("error")
        assert error is not None
        assert "Aborted due to path tolerance violation" in error
        assert "peak controller feedback max position error: wrist_3_joint=-0.2030 rad" in error
        assert "elapsed=" in error
    finally:
        stop_event.set()
        if bundle is not None:
            try:
                bundle.reset()
            except RuntimeError:
                pass
        if fake_server is not None:
            fake_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_backend_preserves_monitor_failure_after_late_result():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")
    pytest.importorskip("trajectory_msgs")

    from control_msgs.msg import JointTrajectoryControllerState
    from rclpy.executors import MultiThreadedExecutor
    from sensor_msgs.msg import JointState
    from trajectory_msgs.msg import JointTrajectoryPoint

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    suffix = uuid.uuid4().hex
    action_name = f"/manyforge_contract_late_result_{suffix}/follow_joint_trajectory"
    joint_state_topic = f"/manyforge_contract_late_result_{suffix}/joint_states"
    controller_state_topic = f"/manyforge_contract_late_result_{suffix}/controller_state"

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_late_result_{suffix}")
    stop_event = threading.Event()
    fake_server = None
    bundle = None

    try:
        fake_server = FakeFollowJointTrajectoryServer(
            node,
            action_name=action_name,
            actual_position_offsets=[0.0, 0.0, 0.0, 0.0, 0.0, 0.2],
            hold_after_first_feedback_s=0.35,
        )
        joint_pub = node.create_publisher(JointState, joint_state_topic, 10)
        state_pub = node.create_publisher(
            JointTrajectoryControllerState,
            controller_state_topic,
            10,
        )

        def publish_state_loop() -> None:
            while not stop_event.is_set():
                joint_state = JointState()
                joint_state.name = list(JOINTS)
                joint_state.position = list(START_POSITIONS)
                joint_pub.publish(joint_state)

                controller_state = JointTrajectoryControllerState()
                controller_state.joint_names = list(JOINTS)
                controller_state.feedback = JointTrajectoryPoint()
                controller_state.feedback.positions = list(START_POSITIONS)
                controller_state.error = JointTrajectoryPoint()
                controller_state.error.positions = [0.0] * len(JOINTS)
                state_pub.publish(controller_state)
                time.sleep(0.01)

        executor.add_node(node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        publish_thread = threading.Thread(target=publish_state_loop, daemon=True)
        spin_thread.start()
        publish_thread.start()

        bundle = _create_bundle(
            action_name=action_name,
            joint_state_topic=joint_state_topic,
            controller_state_topic=controller_state_topic,
            wait_s=5.0,
            config_overrides={
                "tracking_error_tolerance_rad": 0.01,
                "tracking_error_timeout_s": 0.05,
            },
        )
        time.sleep(1.5)

        result = _execute_joint_goal(bundle, "monitor_failure_late_result")
        assert result["ok"] is True

        deadline = time.monotonic() + 5.0
        snapshot = {}
        while time.monotonic() < deadline:
            snapshot = bundle.get_execution_snapshot()
            error = snapshot.get("error")
            if error and "Controller tracking error exceeded tolerance" in error:
                break
            time.sleep(0.05)

        assert "Controller tracking error exceeded tolerance" in (snapshot.get("error") or "")

        # The fake server may report success/cancel after the monitor has already
        # failed the execution. That late action result must not overwrite the
        # terminal failure snapshot.
        time.sleep(0.6)
        final_snapshot = bundle.get_execution_snapshot()
        assert final_snapshot.get("state_name") == "Failed"
        assert "Controller tracking error exceeded tolerance" in (
            final_snapshot.get("error") or ""
        )
    finally:
        stop_event.set()
        if bundle is not None:
            try:
                bundle.reset()
            except RuntimeError:
                pass
        if fake_server is not None:
            fake_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_backend_resyncs_after_reset_timeout_before_next_run():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")
    pytest.importorskip("trajectory_msgs")

    from control_msgs.msg import JointTrajectoryControllerState
    from rclpy.executors import MultiThreadedExecutor
    from sensor_msgs.msg import JointState
    from trajectory_msgs.msg import JointTrajectoryPoint

    from manyforge_ros.testing.fake_follow_joint_trajectory import (
        FakeFollowJointTrajectoryServer,
    )

    suffix = uuid.uuid4().hex
    action_name = f"/manyforge_contract_resync_{suffix}/follow_joint_trajectory"
    joint_state_topic = f"/manyforge_contract_resync_{suffix}/joint_states"
    controller_state_topic = f"/manyforge_contract_resync_{suffix}/controller_state"

    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_resync_{suffix}")
    stop_event = threading.Event()
    fake_server = None
    bundle = None

    try:
        fake_server = FakeFollowJointTrajectoryServer(
            node,
            action_name=action_name,
            hold_after_first_feedback_s=1.0,
        )
        joint_pub = node.create_publisher(JointState, joint_state_topic, 10)
        state_pub = node.create_publisher(
            JointTrajectoryControllerState,
            controller_state_topic,
            10,
        )

        def publish_state_loop() -> None:
            while not stop_event.is_set():
                joint_state = JointState()
                joint_state.name = list(JOINTS)
                joint_state.position = list(START_POSITIONS)
                joint_pub.publish(joint_state)

                controller_state = JointTrajectoryControllerState()
                controller_state.joint_names = list(JOINTS)
                controller_state.feedback = JointTrajectoryPoint()
                controller_state.feedback.positions = list(START_POSITIONS)
                controller_state.error = JointTrajectoryPoint()
                controller_state.error.positions = [0.0] * len(JOINTS)
                state_pub.publish(controller_state)
                time.sleep(0.01)

        executor.add_node(node)
        spin_thread = threading.Thread(target=executor.spin, daemon=True)
        publish_thread = threading.Thread(target=publish_state_loop, daemon=True)
        spin_thread.start()
        publish_thread.start()

        bundle = _create_bundle(
            action_name=action_name,
            joint_state_topic=joint_state_topic,
            controller_state_topic=controller_state_topic,
            wait_s=5.0,
            config_overrides={
                "monitor_period_s": 0.5,
                "stop_completion_timeout_s": 0.01,
            },
        )
        time.sleep(1.5)

        first = _execute_joint_goal(bundle, "reset_timeout_first")
        assert first["ok"] is True
        time.sleep(0.1)

        reset_result = bundle.reset()
        assert reset_result["accepted"] is False
        snapshot = bundle.get_execution_snapshot()
        assert snapshot.get("state_name") == "InterventionRequired"
        assert (snapshot.get("error_details") or {}).get("resync_required") == "true"

        # Once the old monitor has naturally exited, a fresh run from
        # intervention_required must resync observed state and proceed instead
        # of requiring a service restart.
        time.sleep(0.7)
        second = _execute_joint_goal(bundle, "reset_timeout_second")
        assert second["ok"] is True
    finally:
        stop_event.set()
        if bundle is not None:
            try:
                bundle.reset()
            except RuntimeError:
                pass
        if fake_server is not None:
            fake_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_backend_reset_clears_inflight_gripper_command():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")

    from control_msgs.action import GripperCommand
    from rclpy.action import ActionServer
    from rclpy.executors import MultiThreadedExecutor

    suffix = uuid.uuid4().hex
    gripper_action = f"/manyforge_contract_gripper_reset_{suffix}/gripper_cmd"
    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_gripper_reset_{suffix}")
    stop_event = threading.Event()

    def execute_gripper_goal(goal_handle):
        result = GripperCommand.Result()
        while not stop_event.is_set():
            if goal_handle.is_cancel_requested:
                goal_handle.canceled()
                result.position = 0.0
                result.effort = 0.0
                result.stalled = False
                result.reached_goal = False
                return result
            feedback = GripperCommand.Feedback()
            feedback.position = 0.0
            feedback.effort = 0.0
            feedback.stalled = False
            feedback.reached_goal = False
            goal_handle.publish_feedback(feedback)
            time.sleep(0.05)
        goal_handle.abort()
        result.position = 0.0
        result.effort = 0.0
        result.stalled = True
        result.reached_goal = False
        return result

    action_server = ActionServer(
        node,
        GripperCommand,
        gripper_action,
        execute_callback=execute_gripper_goal,
    )
    executor.add_node(node)
    spin_thread = threading.Thread(target=executor.spin, daemon=True)
    spin_thread.start()
    bundle = _create_bundle(
        action_name=f"/manyforge_contract_gripper_reset_{suffix}/follow_joint_trajectory",
        joint_state_topic=f"/manyforge_contract_gripper_reset_{suffix}/joint_states",
        controller_state_topic=f"/manyforge_contract_gripper_reset_{suffix}/controller_state",
        wait_s=0.05,
    )
    try:
        if not hasattr(bundle, "configure_ros_gripper_action"):
            pytest.skip("manyforge_roboplan_py was built without ROS gripper support")

        bundle.configure_ros_gripper_action(
            "primary",
            {
                "action_name": gripper_action,
                "wait_for_action_server_timeout_s": 2.0,
                "result_timeout_s": 10.0,
            },
        )
        time.sleep(1.0)

        first = bundle.command_ros_gripper("primary", 0.0, 10.0, 5.0)
        assert first["accepted"] is True, first
        assert bundle.get_ros_gripper_status("primary")["state"] == "executing"

        bundle.reset()

        second = bundle.command_ros_gripper("primary", 0.0, 10.0, 5.0)
        assert second["accepted"] is True
    finally:
        stop_event.set()
        try:
            bundle.reset()
        except RuntimeError:
            pass
        action_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_gripper_updates_roboplan_scene_group_state():
    rclpy = pytest.importorskip("rclpy")
    pytest.importorskip("control_msgs")

    from control_msgs.action import GripperCommand
    from rclpy.action import ActionServer
    from rclpy.executors import MultiThreadedExecutor

    suffix = uuid.uuid4().hex
    gripper_action = f"/manyforge_contract_gripper_scene_{suffix}/gripper_cmd"
    rclpy.init(args=None)
    executor = MultiThreadedExecutor(num_threads=2)
    node = rclpy.create_node(f"manyforge_contract_gripper_scene_{suffix}")
    stop_event = threading.Event()

    def execute_gripper_goal(goal_handle):
        target = float(goal_handle.request.command.position)
        result = GripperCommand.Result()
        start = time.monotonic()
        while not stop_event.is_set():
            progress = min(max((time.monotonic() - start) / 0.2, 0.0), 1.0)
            feedback = GripperCommand.Feedback()
            feedback.position = target * progress
            feedback.effort = 0.0
            feedback.stalled = False
            feedback.reached_goal = progress >= 1.0
            goal_handle.publish_feedback(feedback)
            if progress >= 1.0:
                goal_handle.succeed()
                result.position = target
                result.effort = 0.0
                result.stalled = False
                result.reached_goal = True
                return result
            time.sleep(0.02)
        goal_handle.abort()
        result.position = 0.0
        result.effort = 0.0
        result.stalled = True
        result.reached_goal = False
        return result

    action_server = ActionServer(
        node,
        GripperCommand,
        gripper_action,
        execute_callback=execute_gripper_goal,
    )
    executor.add_node(node)
    spin_thread = threading.Thread(target=executor.spin, daemon=True)
    spin_thread.start()
    bundle = _create_bundle(
        action_name=f"/manyforge_contract_gripper_scene_{suffix}/follow_joint_trajectory",
        joint_state_topic=f"/manyforge_contract_gripper_scene_{suffix}/joint_states",
        controller_state_topic=f"/manyforge_contract_gripper_scene_{suffix}/controller_state",
        wait_s=0.05,
        model_name="ur10e_robotiq_contract",
        urdf_path=UR10E_ROBOTIQ_URDF_PATH,
        srdf_path=UR10E_ROBOTIQ_SRDF_PATH,
        package_paths=["/workspace"],
        config_overrides={
            "execution_state_source_mode": "commanded_sim",
            "require_fresh_state_before_planning": False,
            "initial_joint_positions_rad": START_POSITIONS,
            "disable_controller_path_tolerances": True,
        },
    )
    try:
        if not hasattr(bundle, "configure_ros_gripper_action"):
            pytest.skip("manyforge_roboplan_py was built without ROS gripper support")

        assert bundle.get_group_joint_positions("gripper") == pytest.approx([0.0])
        bundle.configure_ros_gripper_action(
            "primary",
            {
                "action_name": gripper_action,
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "wait_for_action_server_timeout_s": 2.0,
                "result_timeout_s": 2.0,
            },
        )
        time.sleep(0.2)

        result = bundle.command_ros_gripper("primary", 0.7, 0.2, 5.0)
        assert result["accepted"] is True, result

        deadline = time.monotonic() + 1.5
        observed_motion = False
        final_position = 0.0
        while time.monotonic() < deadline:
            bundle.update_ros_grippers()
            final_position = bundle.get_group_joint_positions("gripper")[0]
            if 0.05 < final_position < 0.69:
                observed_motion = True
            if abs(final_position - 0.7) <= 2e-2:
                break
            time.sleep(0.02)

        assert observed_motion
        assert final_position == pytest.approx(0.7, abs=2e-2)
    finally:
        stop_event.set()
        try:
            bundle.reset()
        except RuntimeError:
            pass
        action_server.destroy()
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


def test_ros2_control_backend_documents_lock_order_contract():
    repo_root = Path(__file__).resolve().parents[2]
    header = (
        repo_root
        / "manyforge_roboplan"
        / "include"
        / "manyforge_roboplan"
        / "ros2_control_execution_backend.hpp"
    ).read_text()

    assert "Lock-order contract:" in header
    assert "Do not hold state_mutex_ while taking execution_mutex_" in header
    assert "best-effort scene updates" in header
