"""Tests for py_trees behavior node implementations."""

import threading
import time

import pytest
import py_trees

from manyforge_behavior.blackboard_namespace import (
    make_runtime_field_ref,
    write_namespace_bundle,
)
from manyforge_behavior.kernel_client import (
    ExecutionState,
    GripperCommandRequest,
    GripperCommandState,
    MockKernelClient,
    SceneEditRequest,
)
from manyforge_behavior.nodes.motion import (
    MoveManipulatorAction,
    ResetTrajectories,
)
from manyforge_behavior.nodes.simulation import (
    ResetSimulation,
    SetEntityState,
    SetSimulationState,
    StepSimulation,
)
from manyforge_behavior.nodes.control_flow import RepeatDecorator
from manyforge_behavior.nodes.scene import (
    AddCollisionObject,
    AttachObjectToLink,
    DetachObjectFromLink,
    RemoveCollisionObject,
    UpdateCollisionObjectPose,
    UpsertCollisionObject,
)
from manyforge_behavior.nodes.logic import (
    CheckKeyBoolValue,
    CheckSignalBool,
    CommandGripper,
    ConfirmGripperGrasp,
    CheckGripperState,
    SetKeyBoolValue,
    SetSignalBool,
    SetGripperState,
    WaitForKeyBool,
    WaitForSignalBool,
    WaitForSignalEdge,
    WaitForGripperState,
)
from manyforge_behavior.nodes.geometry import GetLinkPose, CheckPoseDistance
from manyforge_behavior.runtime_pause import RuntimePauseRegistry


@pytest.fixture(autouse=True)
def reset_blackboard():
    """Reset the global blackboard between tests."""
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


@pytest.fixture
def bb_with_kernel(kernel):
    """Set up blackboard with kernel."""
    client = py_trees.blackboard.Client(name="test_setup")
    client.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    client.kernel = kernel
    return client


@pytest.fixture
def runtime_pause_registry():
    return RuntimePauseRegistry()


def _tick_node(node):
    """Setup and tick a node once."""
    node.setup()
    node.initialise()
    return node.update()


def _tick_node_until_terminal(node, *, timeout_s: float = 1.0):
    """Setup and tick a node until it leaves RUNNING."""
    node.setup()
    node.initialise()
    return _poll_node_until_terminal(node, timeout_s=timeout_s)


def _poll_node_until_terminal(node, *, timeout_s: float = 1.0):
    """Poll an already-initialised node until it leaves RUNNING."""
    deadline = time.monotonic() + timeout_s
    status = node.update()
    while status == py_trees.common.Status.RUNNING and time.monotonic() < deadline:
        time.sleep(0.001)
        status = node.update()
    return status


class TestMoveManipulatorAction:
    def test_success(self, kernel, bb_with_kernel):
        bb_with_kernel.register_key(
            key="program_run_id", access=py_trees.common.Access.WRITE
        )
        bb_with_kernel.program_run_id = "run-node-test"
        node = MoveManipulatorAction(
            name="test_move",
            move_id="m1",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
        )
        # MockKernelClient instantly completes
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.execute_count == 1
        assert kernel.last_execute_request.action_span is not None
        assert (
            kernel.last_execute_request.action_span.program_run_id
            == "run-node-test"
        )
        assert "test_move" in kernel.last_execute_request.action_span.node_instance_id

    def test_success_records_one_opt_in_behavior_qualification_interval(
        self, kernel, bb_with_kernel
    ):
        boundaries = []
        kernel.record_planning_qualification_behavior_boundary = (
            lambda **value: boundaries.append(value)
        )
        node = MoveManipulatorAction(
            name="test_move_qualification",
            move_id="drop_approach",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        assert [value["phase"] for value in boundaries] == [
            "started",
            "succeeded",
        ]
        assert all(value["move_id"] == "drop_approach" for value in boundaries)
        assert (
            boundaries[0]["action_span"].node_attempt_id
            == boundaries[1]["action_span"].node_attempt_id
        )

    def test_failure(self, kernel, bb_with_kernel, runtime_pause_registry):
        # Runtime motion faults hold the owning node without terminating a
        # Parallel. Recovery is generation-bound through the shared registry;
        # there is deliberately no global blackboard retry flag.
        kernel.fail_next_execute = True
        node = MoveManipulatorAction(
            name="test_move_fail",
            move_id="m1",
            motion_type="joint_goal",
            runtime_pause_registry=runtime_pause_registry,
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].domain == "motion"
        assert faults[0].reason_code == "motion_execute_rejected"
        assert faults[0].node_name == "test_move_fail"

    def test_from_non_idle_non_terminal(self, kernel, bb_with_kernel):
        """Execute from non-terminal non-IDLE state holds RUNNING."""
        # Same soft-fail semantics: a busy kernel state at tick time is
        # surfaced as a paused-due-to-error signal rather than propagating
        # FAILURE. The cycle manager handles the recovery handshake.
        kernel._state = ExecutionState.EXECUTING
        node = MoveManipulatorAction(
            name="test_move_busy",
            move_id="m1",
            motion_type="joint_goal",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING

    def test_request_includes_tolerances_and_tcp_cap(self, kernel, bb_with_kernel):
        cap_bb = py_trees.blackboard.Client(name="tcp_cap_writer")
        cap_bb.register_key(
            key="global_max_tcp_linear_speed_mps",
            access=py_trees.common.Access.WRITE,
        )
        cap_bb.global_max_tcp_linear_speed_mps = 0.25

        vs_bb = py_trees.blackboard.Client(name="velocity_writer")
        vs_bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        vs_bb.velocity_scale = 0.8

        node = MoveManipulatorAction(
            name="test_move_params",
            move_id="m_tcp",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
            velocity_scale=0.5,
            acceleration_scale=0.6,
            max_tcp_linear_speed_mps=0.5,
            joint_position_tol_rad=0.02,
            translation_tol_m=0.003,
            rotation_tol_rad=0.04,
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.5)
        assert kernel.last_execute_request.acceleration_scale == pytest.approx(0.6)
        assert kernel.last_execute_request.max_tcp_linear_speed_mps == pytest.approx(0.25)
        assert kernel.last_execute_request.joint_position_tol_rad == pytest.approx(0.02)
        assert kernel.last_execute_request.translation_tol_m == pytest.approx(0.003)
        assert kernel.last_execute_request.rotation_tol_rad == pytest.approx(0.04)

    def test_runtime_velocity_scale_does_not_modify_dispatched_request(self, bb_with_kernel):
        kernel = MockKernelClient(execute_duration_s=0.05)
        bb_with_kernel.kernel = kernel

        vs_bb = py_trees.blackboard.Client(name="velocity_writer")
        vs_bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        vs_bb.velocity_scale = 0.8

        node = MoveManipulatorAction(
            name="test_move_async",
            move_id="m_async",
            motion_type="joint_goal",
            joint_goal_names=["j1"],
            joint_goal_positions_rad=[1.0],
            velocity_scale=0.5,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.RUNNING
        for _ in range(5):
            node.update()

        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.5)
        assert node._params["velocity_scale"] == pytest.approx(0.5)

        while node.update() == py_trees.common.Status.RUNNING:
            pass

        node.initialise()
        assert node.update() == py_trees.common.Status.RUNNING
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.velocity_scale == pytest.approx(0.5)

    def test_relative_move_request_normalizes_offset_orientation(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="test_relative_move",
            move_id="m_rel",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset": {
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
            },
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.motion_type == "relative_move"
        assert kernel.last_execute_request.relative_move is not None
        assert kernel.last_execute_request.relative_move["target_reference"] == {
            "type": "object",

            "id": "graspable",
        }
        assert "offset_reference" not in kernel.last_execute_request.relative_move
        assert "orientation_rpy_deg" not in kernel.last_execute_request.relative_move["offset"]
        assert kernel.last_execute_request.relative_move["offset"]["orientation_xyzw"] == pytest.approx(
            [0.70710678, -0.70710678, -4.32978028e-17, 4.32978028e-17],
            abs=1e-6,
        )

    def test_relative_move_applies_pose_source_before_planning(self, kernel, bb_with_kernel):
        calls = []

        def apply_pose_source(**kwargs):
            calls.append(kwargs)
            return {"success": True}

        client = py_trees.blackboard.Client(name="pose_source_setup")
        client.register_key(
            key="pose_source_apply_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.pose_source_apply_callback = apply_pose_source

        node = MoveManipulatorAction(
            name="test_relative_move_apply_source",
            move_id="m_rel_source",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset": {"position_m": [0.0, 0.0, 0.1]},
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "test_relative_move_apply_source" in (span.node_instance_id or "")
        assert calls[0] == {
            "object_ids": [],
            "reason": "motion_node:test_relative_move_apply_source",
        }

    def test_pose_goal_applies_all_pose_sources_before_planning(self, kernel, bb_with_kernel):
        calls = []

        def apply_pose_source(**kwargs):
            calls.append(kwargs)
            return {"success": True}

        client = py_trees.blackboard.Client(name="pose_source_pose_goal_setup")
        client.register_key(
            key="pose_source_apply_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.pose_source_apply_callback = apply_pose_source

        node = MoveManipulatorAction(
            name="test_pose_goal_apply_source",
            move_id="m_pose_source",
            motion_type="pose_goal",
            pose_goal={
                "position_m": [0.1, 0.2, 0.3],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "test_pose_goal_apply_source" in (span.node_instance_id or "")
        assert calls[0] == {
            "object_ids": [],
            "reason": "motion_node:test_pose_goal_apply_source",
        }

    def test_relative_move_holds_when_before_planning_pose_source_fails(
        self,
        kernel,
        bb_with_kernel,
    ):
        def apply_pose_source(**kwargs):
            return {"success": False, "error": "no fresh pose-source update for: graspable"}

        client = py_trees.blackboard.Client(name="pose_source_setup_fail")
        client.register_key(
            key="pose_source_apply_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.pose_source_apply_callback = apply_pose_source

        node = MoveManipulatorAction(
            name="test_relative_move_apply_source_fail",
            move_id="m_rel_source_fail",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset": {"position_m": [0.0, 0.0, 0.1]},
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.RUNNING
        assert node.feedback_message == "no fresh pose-source update for: graspable"

    def test_relative_move_applies_between_nodes_pose_source(self, kernel, bb_with_kernel):
        calls = []

        def apply_between_nodes(**kwargs):
            calls.append(kwargs)
            return {"success": True}

        client = py_trees.blackboard.Client(name="pose_source_between_nodes_setup")
        client.register_key(
            key="pose_source_between_nodes_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.pose_source_between_nodes_callback = apply_between_nodes

        node = MoveManipulatorAction(
            name="test_relative_move_between_nodes_source",
            move_id="m_rel_between_nodes",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset": {"position_m": [0.0, 0.0, 0.1]},
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        between_nodes = calls[0]
        span = between_nodes.pop("action_span")
        assert span.action_source == "program_node"
        assert "test_relative_move_between_nodes_source" in (
            span.node_instance_id or ""
        )
        assert between_nodes == {
            "object_ids": ["graspable"],
            "reason": "motion_node:test_relative_move_between_nodes_source",
        }

    def test_relative_move_pose_reference_normalizes_inline_pose_orientation(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="test_relative_move_pose_ref",
            move_id="m_rel_pose",
            motion_type="relative_move",
            relative_move={
                "target_reference": {
                    "type": "pose",
                    "pose": {
                        "position_m": [0.3, -0.2, 0.15],
                        "orientation_rpy_deg": [180.0, 0.0, -90.0],
                    },
                },
                "offset": {
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            },
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.relative_move is not None
        ref = kernel.last_execute_request.relative_move["target_reference"]
        assert ref["type"] == "pose"
        assert "orientation_rpy_deg" not in ref["pose"]
        assert ref["pose"]["orientation_xyzw"] == pytest.approx(
            [0.70710678, -0.70710678, -4.32978028e-17, 4.32978028e-17],
            abs=1e-6,
        )

    def test_relative_move_offset_reference_normalizes_inline_pose_orientation(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="test_relative_move_offset_ref",
            move_id="m_rel_offset_ref",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "graspable"},
                "offset_reference": {
                    "type": "pose",
                    "pose": {
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation_rpy_deg": [0.0, 0.0, 90.0],
                    },
                },
                "offset": {
                    "position_m": [0.1, 0.0, 0.0],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            },
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.relative_move is not None
        ref = kernel.last_execute_request.relative_move["offset_reference"]
        assert ref["type"] == "pose"
        assert "orientation_rpy_deg" not in ref["pose"]
        assert ref["pose"]["orientation_xyzw"] == pytest.approx(
            [0.0, 0.0, 0.70710678, 0.70710678],
            abs=1e-6,
        )

    def test_pose_goal_blackboard_reference_resolves_runtime_pose(self, kernel, bb_with_kernel):
        bb_with_kernel.register_key(
            key="perception",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            bb_with_kernel,
            "perception",
            value={
                "grasp_pose": {
                    "position_m": [0.42, -0.05, 0.31],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
            },
            status="available",
        )

        node = MoveManipulatorAction(
            name="test_pose_goal_bb_ref",
            move_id="m_pose_bb",
            motion_type="pose_goal",
            pose_goal=make_runtime_field_ref("perception", "grasp_pose"),
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.pose_goal == {
            "position_m": [0.42, -0.05, 0.31],
            "orientation_xyzw": pytest.approx(
                [0.70710678, -0.70710678, -4.32978028e-17, 4.32978028e-17],
                abs=1e-6,
            ),
        }

    def test_pose_goal_blackboard_reference_missing_key_fails_closed(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="test_pose_goal_bb_missing",
            move_id="m_pose_missing",
            motion_type="pose_goal",
            pose_goal=make_runtime_field_ref("perception", "missing_grasp_pose"),
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE
        assert kernel.execute_count == 0

    def test_relative_move_pose_reference_can_resolve_blackboard_pose(self, kernel, bb_with_kernel):
        bb_with_kernel.register_key(
            key="perception",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            bb_with_kernel,
            "perception",
            value={
                "approach_pose": {
                    "position_m": [0.3, -0.2, 0.15],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
            },
            status="available",
        )

        node = MoveManipulatorAction(
            name="test_relative_move_bb_ref",
            move_id="m_rel_bb",
            motion_type="relative_move",
            relative_move={
                "target_reference": {
                    "type": "pose",
                    "pose": make_runtime_field_ref("perception", "approach_pose"),
                },
                "offset": {
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation_rpy_deg": [0.0, 0.0, 0.0],
                },
            },
        )

        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_execute_request is not None
        ref = kernel.last_execute_request.relative_move["target_reference"]
        assert ref["type"] == "pose"
        assert ref["pose"]["orientation_xyzw"] == pytest.approx(
            [0.70710678, -0.70710678, -4.32978028e-17, 4.32978028e-17],
            abs=1e-6,
        )

    def test_repeat_resubmits_relative_move_without_manual_cache_reset(self, kernel, bb_with_kernel):
        node = MoveManipulatorAction(
            name="dynamic_pick_move",
            move_id="pick_dynamic_box",
            motion_type="relative_move",
            relative_move={
                "target_reference": {"type": "object", "id": "dynamic_box"},
                "offset": {
                    "position_m": [0.0, 0.0, 0.1],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
            },
            cache_mode="default",
        )
        repeat = RepeatDecorator("repeat_dynamic_pick", child=node, num_cycles=2)
        tree = py_trees.trees.BehaviourTree(root=repeat)

        tree.setup()
        tree.tick()
        assert repeat.status == py_trees.common.Status.RUNNING
        tree.tick()

        assert repeat.status == py_trees.common.Status.SUCCESS
        assert kernel.execute_count == 2
        assert kernel.invalidate_count == 0
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.move_id == "pick_dynamic_box"
        assert kernel.last_execute_request.motion_type == "relative_move"
        assert kernel.last_execute_request.relative_move is not None
        assert kernel.last_execute_request.relative_move["target_reference"] == {
            "type": "object",
            "id": "dynamic_box",
        }


class TestResetSimulation:
    def _install_callback(self, callback):
        client = py_trees.blackboard.Client(name="simulation_reset_writer")
        client.register_key(
            key="simulation_control_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.simulation_control_callback = callback

    def test_calls_composer_callback_with_running_authority(self):
        calls = []

        def callback(**kwargs):
            calls.append(kwargs)
            return {"success": True, "control_id": "reset1"}

        self._install_callback(callback)
        node = ResetSimulation(name="reset_sim", reason="program_reset")

        assert _tick_node_until_terminal(node) == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "reset_sim" in (span.node_instance_id or "")
        assert calls[0] == {
            "operation": "reset",
            "reason": "program_reset",
            "source": "behavior_node",
            "allow_running": True,
        }

    def test_fails_without_callback(self):
        self._install_callback(None)
        node = ResetSimulation(name="reset_sim")

        assert _tick_node(node) == py_trees.common.Status.FAILURE
        assert "callback" in node.feedback_message

    def test_runtime_rejection_pauses_by_default(self, runtime_pause_registry):
        self._install_callback(lambda **_kwargs: {"success": False, "error": "denied"})
        node = ResetSimulation(
            name="reset_sim",
            runtime_pause_registry=runtime_pause_registry,
        )

        node.setup()
        node.initialise()
        deadline = time.monotonic() + 1.0
        status = node.update()
        while not runtime_pause_registry.active_faults() and time.monotonic() < deadline:
            time.sleep(0.001)
            status = node.update()
        assert status == py_trees.common.Status.RUNNING
        assert node.feedback_message == "denied"
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].domain == "simulation"
        assert faults[0].reason_code == "simulation_control_rejected"

    def test_runtime_rejection_can_be_authored_as_condition_failure(self):
        self._install_callback(lambda **_kwargs: {"success": False, "error": "denied"})
        node = ResetSimulation(
            name="reset_sim",
            runtime_failure_policy="condition_failure",
        )

        assert _tick_node_until_terminal(node) == py_trees.common.Status.FAILURE
        assert node.feedback_message == "denied"

    def test_slow_callback_does_not_block_tick_thread(self):
        entered = threading.Event()
        release = threading.Event()

        def callback(**_kwargs):
            entered.set()
            release.wait(timeout=1.0)
            return {"success": True}

        self._install_callback(callback)
        node = ResetSimulation(name="reset_sim")
        node.setup()
        node.initialise()

        start = time.monotonic()
        status = node.update()
        elapsed = time.monotonic() - start

        assert status == py_trees.common.Status.RUNNING
        assert elapsed < 0.05
        assert entered.wait(timeout=1.0)
        release.set()
        assert _poll_node_until_terminal(node) == py_trees.common.Status.SUCCESS


class TestSimulationControlNodes:
    def _install_callback(self, callback):
        client = py_trees.blackboard.Client(name="simulation_control_writer")
        client.register_key(
            key="simulation_control_callback",
            access=py_trees.common.Access.WRITE,
        )
        client.simulation_control_callback = callback

    def test_set_simulation_state_calls_composer_callback(self):
        calls = []
        self._install_callback(lambda **kwargs: calls.append(kwargs) or {"success": True})
        node = SetSimulationState(name="pause_sim", state="paused", reason="pause")

        assert _tick_node_until_terminal(node) == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "pause_sim" in (span.node_instance_id or "")
        assert calls[0] == {
            "operation": "set_state",
            "reason": "pause",
            "source": "behavior_node",
            "allow_running": True,
            "state": "paused",
        }

    def test_step_simulation_calls_composer_callback(self):
        calls = []
        self._install_callback(lambda **kwargs: calls.append(kwargs) or {"success": True})
        node = StepSimulation(name="step_sim", steps=3)

        assert _tick_node_until_terminal(node) == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "step_sim" in (span.node_instance_id or "")
        assert calls[0] == {
            "operation": "step",
            "reason": "step_sim",
            "source": "behavior_node",
            "allow_running": True,
            "steps": 3,
        }

    def test_set_entity_state_calls_composer_callback(self):
        calls = []
        self._install_callback(lambda **kwargs: calls.append(kwargs) or {"success": True})
        pose = {
            "frame_id": "world",
            "position_m": [0.1, 0.2, 0.3],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        node = SetEntityState(name="set_entity", entity="/World/box", pose=pose)

        assert _tick_node_until_terminal(node) == py_trees.common.Status.SUCCESS
        assert len(calls) == 1
        span = calls[0].pop("action_span")
        assert span.action_source == "program_node"
        assert "set_entity" in (span.node_instance_id or "")
        assert calls[0] == {
            "operation": "set_entity_state",
            "reason": "set_entity",
            "source": "behavior_node",
            "allow_running": True,
            "entity": "/World/box",
            "pose": pose,
        }


class TestResetTrajectories:
    def test_success(self, kernel, bb_with_kernel):
        node = ResetTrajectories(name="test_reset")
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.invalidate_count == 1
        assert kernel.last_invalidate_action_span is not None
        assert kernel.last_invalidate_action_span.action_source == "program_node"
        assert "test_reset" in (
            kernel.last_invalidate_action_span.node_instance_id or ""
        )


class TestAddCollisionObject:
    def test_success(self, kernel, bb_with_kernel):
        node = AddCollisionObject(
            name="add_box",
            object_id="box1",
            shape={
                "type": "box",
                "box_dimensions_m": [1.0, 0.5, 0.1],
            },
            pose={
                "frame_id": "base_link",
                "position_m": [0.5, 0, 0.4],
                "orientation_xyzw": [0, 0, 0, 1],
            },
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].action_span is not None
        assert kernel.scene_edits[0].action_span.action_source == "program_node"
        assert "add_box" in (kernel.scene_edits[0].action_span.node_instance_id or "")

    def test_accepts_schema_backed_nested_shape(self, kernel, bb_with_kernel):
        node = AddCollisionObject(
            name="add_box",
            object_id="box1",
            shape={
                "type": "box",
                "box_dimensions_m": [1.0, 0.5, 0.1],
            },
            pose={
                "frame_id": "base_link",
                "position_m": [0.5, 0.0, 0.4],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        )
        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        request = kernel.scene_edits[-1]
        assert request.shape_type == "box"
        assert request.box_dimensions_m == [1.0, 0.5, 0.1]
        assert request.frame_id == "base_link"
        assert request.pose_position_m == [0.5, 0.0, 0.4]
        assert request.pose_orientation_xyzw == [0.0, 0.0, 0.0, 1.0]


class TestUpsertCollisionObject:
    def test_inserts_missing_object(self, kernel, bb_with_kernel):
        node = UpsertCollisionObject(
            name="upsert_box",
            object_id="box1",
            shape={
                "type": "box",
                "box_dimensions_m": [1.0, 0.5, 0.1],
            },
            pose={
                "frame_id": "base_link",
                "position_m": [0.5, 0, 0.4],
                "orientation_xyzw": [0, 0, 0, 1],
            },
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[-1].operation == "upsert"
        assert kernel.get_object_state("box1").exists is True

    def test_replaces_existing_object(self, kernel, bb_with_kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        node = UpsertCollisionObject(
            name="upsert_box",
            object_id="box1",
            shape={
                "type": "sphere",
                "sphere_radius_m": 0.05,
            },
            pose={
                "frame_id": "base_link",
                "position_m": [0.5, 0, 0.4],
                "orientation_xyzw": [0, 0, 0, 1],
            },
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        snapshot = kernel.get_scene_objects()[0]
        assert snapshot.shape is not None
        assert snapshot.shape.type == "sphere"


class TestSceneNodeBlackboardReferences:
    def test_attach_object_to_link_resolves_object_id_from_blackboard(self, kernel, bb_with_kernel):
        add_request = SceneEditRequest(
            operation="add",
            object_id="sphere_part",
            shape_type="sphere",
            sphere_radius_m=0.01,
            pose_position_m=[0.40, -0.14, 0.009],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        )
        assert kernel.scene_edit(add_request).success is True

        bb_with_kernel.register_key(
            key="planning/pick_plan",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            bb_with_kernel,
            "planning/pick_plan",
            value={"selected_object_id": "sphere_part"},
            status="planned",
        )

        registry_bb = py_trees.blackboard.Client(name="scene_registry")
        registry_bb.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        registry_bb.scene_objects = {
            "sphere_part": {
                "shape_type": "sphere",
                "sphere_radius_m": 0.01,
            }
        }

        node = AttachObjectToLink(
            name="attach_selected",
            object_id=make_runtime_field_ref(
                "planning/pick_plan",
                "selected_object_id",
            ),
            link_frame="tool0",
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert kernel.get_object_state("sphere_part").attached is True
        assert kernel.scene_edits[0].operation == "add"

    def test_failure(self, kernel, bb_with_kernel):
        kernel.fail_next_scene_edit = True
        node = AddCollisionObject(name="add_fail", object_id="fail")
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE


class TestRemoveCollisionObject:
    def test_success(self, kernel, bb_with_kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        node = RemoveCollisionObject(name="remove_box", object_id="box1")
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[-1].operation == "remove"


class TestUpdateCollisionObjectPose:
    def test_success(self, kernel, bb_with_kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        node = UpdateCollisionObjectPose(
            name="update_box",
            object_id="box1",
            pose={
                "frame_id": "base_link",
                "position_m": [1.0, 0, 0],
                "orientation_xyzw": [0, 0, 0, 1],
            },
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[-1].operation == "update_pose"

    def test_success_emits_scene_callback(self, kernel, bb_with_kernel):
        callbacks = []
        bb_with_kernel.register_key(
            key="scene_edit_callback", access=py_trees.common.Access.WRITE
        )
        bb_with_kernel.scene_edit_callback = (
            lambda operation, request: callbacks.append((operation, request))
        )
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        node = UpdateCollisionObjectPose(
            name="update_box",
            object_id="box1",
            pose={
                "frame_id": "base_link",
                "position_m": [1.0, 0, 0],
                "orientation_xyzw": [0, 0, 0, 1],
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert callbacks[-1][0] == "update_pose"
        assert callbacks[-1][1].operation == "update_pose"


class TestAttachDetachObjectNodes:
    def test_attach_delegates_to_backend_and_preserves_current_pose(self, kernel, bb_with_kernel):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {
            "box1": {
                "shape_type": "box",
                "box_dimensions_m": [0.1, 0.1, 0.1],
                "position_m": [1.5, 0.0, 0.6],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                "frame_id": "universe",
            }
        }
        bb_with_kernel.gripper_configs = {
            "primary": {
                "touch_links": ["left_inner_finger_pad", "right_inner_finger_pad"],
                "grasp_confirmation_policy": "none_open_loop",
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )

        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
        )
        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        attach_req = kernel.scene_edits[-1]
        assert attach_req.operation == "attach"
        assert attach_req.object_id == "box1"
        assert attach_req.frame_id == "tool0"
        assert attach_req.touch_links == [
            "left_inner_finger_pad",
            "right_inner_finger_pad",
        ]
        assert "none_open_loop" in node.feedback_message
        state = kernel.get_object_state("box1")
        assert state.exists is True
        assert state.attached is True
        assert state.frame_id == "tool0"
        pose = kernel.get_object_pose("box1", "tool0")
        assert pose is not None
        assert pose["position_m"] == pytest.approx([0.1, 0.0, 0.0])
        assert pose["orientation_xyzw"] == pytest.approx([0.0, 0.0, 0.0, 1.0])

    def test_attach_emits_direct_attach_callback(self, kernel, bb_with_kernel):
        callbacks = []
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(
            key="scene_edit_callback", access=py_trees.common.Access.WRITE
        )
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.scene_edit_callback = (
            lambda operation, request: callbacks.append((operation, request))
        )
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert callbacks == [("attach", kernel.scene_edits[-1])]
        assert callbacks[0][1].operation == "attach"

    def test_attach_reports_grasp_policy_with_explicit_touch_links(
        self, kernel, bb_with_kernel
    ):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.gripper_configs = {
            "primary": {
                "touch_links": ["configured_pad"],
                "grasp_confirmation_policy": "none_open_loop",
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
            touch_links=["explicit_pad"],
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        assert kernel.scene_edits[-1].touch_links == ["explicit_pad"]
        assert "none_open_loop" in node.feedback_message

    def test_attach_rejects_evidence_backed_policy_without_confirmation(
        self, kernel, bb_with_kernel, runtime_pause_registry
    ):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.gripper_configs = {
            "primary": {
                "touch_links": ["configured_pad"],
                "grasp_confirmation_policy": "operator_confirm",
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
            runtime_pause_registry=runtime_pause_registry,
        )

        assert _tick_node(node) == py_trees.common.Status.RUNNING
        assert "lacks verified grasp evidence" in node.feedback_message
        assert kernel.get_object_state("box1").attached is False
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].domain == "scene"
        assert faults[0].reason_code == "command_estimate_attach_rejected"

        reader = py_trees.blackboard.Client(name="attach_fault_reader")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "command_estimate_attach_rejected"
        assert details["surface"] == "attach_object_to_link"
        assert details["grasp_confirmation_policy"] == "operator_confirm"

    def test_attach_accepts_verified_grasp_confirmation_status(
        self, kernel, bb_with_kernel
    ):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.gripper_configs = {
            "primary": {
                "touch_links": ["configured_pad"],
                "grasp_confirmation_policy": "operator_confirm",
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        result = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.0,
                duration_s=0.0,
                actuation_kind="position_commanded",
                state_authority="real_observed",
                completion_policy="driver_result",
                grasp_confirmation_policy="operator_confirm",
                transport_kind="ros_gripper_command",
            )
        )
        assert result.accepted is True
        kernel._gripper_commands["primary"]["evidence_state"] = "confirmed"

        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        assert kernel.get_object_state("box1").attached is True

    def test_attach_rejects_lost_grasp_confirmation_status(
        self, kernel, bb_with_kernel, runtime_pause_registry
    ):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.gripper_configs = {
            "primary": {
                "touch_links": ["configured_pad"],
                "grasp_confirmation_policy": "force_or_current",
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        result = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.0,
                duration_s=0.0,
                actuation_kind="position_commanded",
                state_authority="real_observed",
                completion_policy="driver_result",
                grasp_confirmation_policy="force_or_current",
                transport_kind="ros_gripper_command",
            )
        )
        assert result.accepted is True
        kernel._gripper_commands["primary"]["evidence_state"] = (
            "pressure_or_part_lost"
        )

        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
            runtime_pause_registry=runtime_pause_registry,
        )

        assert _tick_node(node) == py_trees.common.Status.RUNNING
        assert "pressure_or_part_lost" in node.feedback_message
        assert kernel.get_object_state("box1").attached is False
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].reason_code == "grasp_confirmation_lost"

        reader = py_trees.blackboard.Client(name="attach_lost_fault_reader")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "grasp_confirmation_lost"
        assert details["evidence_state"] == "pressure_or_part_lost"

    def test_detach_delegates_to_backend_and_preserves_current_pose(
        self, kernel, bb_with_kernel
    ):
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.scene_objects = {
            "box1": {
                "shape_type": "box",
                "box_dimensions_m": [0.1, 0.1, 0.1],
            }
        }
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.1, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="tool0",
            )
        )

        node = DetachObjectFromLink(name="detach_box", object_id="box1", link_frame="tool0")
        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        detach_req = kernel.scene_edits[-1]
        assert detach_req.operation == "detach"
        assert detach_req.object_id == "box1"
        assert detach_req.frame_id == "universe"
        state = kernel.get_object_state("box1")
        assert state.exists is True
        assert state.attached is False
        assert state.frame_id == "universe"
        pose = kernel.get_object_pose("box1", "universe")
        assert pose is not None
        assert pose["position_m"] == pytest.approx([0.6, 0.0, 0.6])
        assert pose["orientation_xyzw"] == pytest.approx([0.0, 0.0, 0.0, 1.0])

    def test_detach_emits_direct_detach_callback(self, kernel, bb_with_kernel):
        callbacks = []
        bb_with_kernel.register_key(key="scene_objects", access=py_trees.common.Access.WRITE)
        bb_with_kernel.register_key(
            key="scene_edit_callback", access=py_trees.common.Access.WRITE
        )
        bb_with_kernel.scene_objects = {}
        bb_with_kernel.scene_edit_callback = (
            lambda operation, request: callbacks.append((operation, request))
        )
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.1, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="tool0",
            )
        )
        node = DetachObjectFromLink(name="detach_box", object_id="box1", link_frame="tool0")

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        assert callbacks == [("detach", kernel.scene_edits[-1])]
        assert callbacks[0][1].operation == "detach"


class TestCheckKeyBoolValue:
    def test_matching_value(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="flag", access=py_trees.common.Access.WRITE)
        writer.flag = True

        node = CheckKeyBoolValue(name="check_flag", key="flag", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_mismatched_value(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="flag", access=py_trees.common.Access.WRITE)
        writer.flag = False

        node = CheckKeyBoolValue(name="check_flag", key="flag", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE

    def test_missing_key(self, bb_with_kernel):
        node = CheckKeyBoolValue(name="check_missing", key="missing_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE


class TestSetKeyBoolValue:
    def test_sets_value(self, bb_with_kernel):
        node = SetKeyBoolValue(name="set_flag", key="my_flag", value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="my_flag", access=py_trees.common.Access.READ)
        assert reader.my_flag is True


class TestWaitForKeyBool:
    def test_returns_running_when_missing(self, bb_with_kernel):
        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING


class TestSignalNodes:
    def test_check_signal_bool(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="cell/fixture_ready", access=py_trees.common.Access.WRITE)
        writer.set("cell/fixture_ready", True)

        node = CheckSignalBool(
            name="check_fixture_ready",
            key="cell/fixture_ready",
            expected_value=True,
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

    def test_set_signal_bool(self, bb_with_kernel):
        node = SetSignalBool(
            name="set_cycle_complete",
            key="cell/cycle_complete",
            value=True,
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="cell/cycle_complete", access=py_trees.common.Access.READ)
        assert reader.get("cell/cycle_complete") is True

    def test_wait_for_signal_bool(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="cell/permit", access=py_trees.common.Access.WRITE)
        writer.set("cell/permit", True)

        node = WaitForSignalBool(
            name="wait_permit",
            key="cell/permit",
            expected_value=True,
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

    def test_wait_for_signal_edge_rising(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="cell/permit", access=py_trees.common.Access.WRITE)
        writer.set("cell/permit", False)

        node = WaitForSignalEdge(
            name="wait_edge",
            key="cell/permit",
            edge="rising",
        )
        node.setup()
        node.initialise()
        assert node.update() == py_trees.common.Status.RUNNING

        writer.set("cell/permit", True)
        assert node.update() == py_trees.common.Status.SUCCESS

    def test_wait_for_signal_edge_fails_on_non_bool(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="cell/count", access=py_trees.common.Access.WRITE)
        writer.set("cell/count", 1)

        node = WaitForSignalEdge(
            name="wait_edge",
            key="cell/count",
            edge="any",
        )
        node.setup()
        node.initialise()
        assert node.update() == py_trees.common.Status.FAILURE


class TestGripperStateNodes:
    def test_set_gripper_state_closed(self, bb_with_kernel):
        node = SetGripperState(
            name="close_gripper",
            state_key="gripper_closed",
            state="closed",
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="gripper_closed", access=py_trees.common.Access.READ)
        assert reader.get("gripper_closed") is True

    def test_check_gripper_state(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="gripper_closed", access=py_trees.common.Access.WRITE)
        writer.set("gripper_closed", False)

        node = CheckGripperState(
            name="check_open",
            state_key="gripper_closed",
            expected_state="open",
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

    def test_wait_for_gripper_state(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="gripper_closed", access=py_trees.common.Access.WRITE)
        writer.set("gripper_closed", False)

        node = WaitForGripperState(
            name="wait_open",
            state_key="gripper_closed",
            expected_state="open",
        )
        assert _tick_node(node) == py_trees.common.Status.SUCCESS

    def test_returns_success_when_matched(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="wait_key", access=py_trees.common.Access.WRITE)
        writer.wait_key = True

        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_returns_running_when_mismatched(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="wait_key", access=py_trees.common.Access.WRITE)
        writer.wait_key = False

        node = WaitForKeyBool(name="wait_flag", key="wait_key", expected_value=True)
        status = _tick_node(node)
        assert status == py_trees.common.Status.RUNNING


class TestCommandGripper:
    def test_command_gripper_moves_to_numeric_position(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
            }
        }

        node = CommandGripper(
            name="move_gripper",
            gripper_id="primary",
            position=0.42,
            max_effort=35.0,
            timeout_s=1.0,
        )
        node.setup()
        node.initialise()

        status = node.update()
        assert status == py_trees.common.Status.RUNNING

        deadline = time.monotonic() + 1.0
        while status == py_trees.common.Status.RUNNING and time.monotonic() < deadline:
            time.sleep(0.01)
            status = node.update()

        assert status == py_trees.common.Status.SUCCESS
        assert kernel.last_gripper_command_request is not None
        assert kernel.last_gripper_command_request.gripper_id == "primary"
        assert kernel.last_gripper_command_request.target_position == pytest.approx(0.42)
        assert kernel.last_gripper_command_request.max_effort == pytest.approx(35.0)
        assert kernel.last_gripper_command_request.action_span is not None
        assert kernel.last_gripper_command_request.action_span.action_source == "program_node"
        assert "move_gripper" in (
            kernel.last_gripper_command_request.action_span.node_instance_id or ""
        )

    def test_command_gripper_derives_default_timeout_from_motion_time(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
            }
        }

        node = CommandGripper(
            name="move_gripper",
            gripper_id="primary",
            position=0.7,
        )
        node.setup()
        node.initialise()

        status = node.update()
        deadline = time.monotonic() + 1.0
        while status == py_trees.common.Status.RUNNING and time.monotonic() < deadline:
            time.sleep(0.01)
            status = node.update()

        assert status == py_trees.common.Status.SUCCESS


    def test_command_gripper_failure_records_structured_fault(
        self, kernel, bb_with_kernel, runtime_pause_registry
    ):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
            }
        }
        kernel.fail_next_gripper_command = True

        node = CommandGripper(
            name="move_gripper",
            gripper_id="primary",
            position=0.7,
            timeout_s=1.0,
            runtime_pause_registry=runtime_pause_registry,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.RUNNING
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].domain == "gripper"
        assert faults[0].reason_code == "gripper_request_failed"

        reader = py_trees.blackboard.Client(name="gripper_fault_reader")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert isinstance(details, dict)
        assert details["reason_code"] == "gripper_request_failed"
        assert details["gripper_id"] == "primary"
        assert details["surface"] == "command_gripper"
        assert "injected" in details["message"]

        # A fresh command must clear the stale fault record.
        node.initialise()
        assert reader.get("gripper_fault_details") is None

    def test_command_gripper_records_invalid_semantic_transport_fault(
        self, kernel, bb_with_kernel
    ):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "plc_gripper_request",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
                "actuation_kind": "pneumatic",
                "state_authority": "commanded_estimate",
                "completion_policy": "request_ack_then_dwell",
                "grasp_confirmation_policy": "none_open_loop",
                "transport": {"kind": "plc_gripper_request"},
            }
        }

        node = CommandGripper(
            name="close_gripper",
            gripper_id="primary",
            position=0.0,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.FAILURE

        reader = py_trees.blackboard.Client(name="gripper_fault_reader_plc")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "invalid_gripper_semantics"
        assert details["actuation_kind"] == "pneumatic"
        assert details["state_authority"] == "commanded_estimate"
        assert details["transport_kind"] == "plc_gripper_request"

    def test_pneumatic_command_reports_dwell_evidence_not_grasp_proof(
        self, kernel, bb_with_kernel
    ):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.01,
                "srdf_presets": {"open": 0.0081, "closed": 0.0},
                "actuation_kind": "pneumatic",
                "state_authority": "commanded_estimate",
                "completion_policy": "dwell_elapsed",
                "grasp_confirmation_policy": "none_open_loop",
                "transport": {"kind": "native_kinematic"},
                "open_state_ref": "open",
                "closed_state_ref": "closed",
            }
        }

        node = CommandGripper(
            name="close_gripper",
            gripper_id="primary",
            position=0.0,
            timeout_s=1.0,
        )
        node.setup()
        node.initialise()

        status = node.update()
        assert status == py_trees.common.Status.RUNNING
        assert "authority=commanded_estimate" in node.feedback_message
        assert "evidence=dwell_elapsed" in node.feedback_message
        assert "evidence_state=unverified" in node.feedback_message

        deadline = time.monotonic() + 1.0
        while status == py_trees.common.Status.RUNNING and time.monotonic() < deadline:
            time.sleep(0.01)
            status = node.update()

        assert status == py_trees.common.Status.SUCCESS
        runtime_status = kernel.get_gripper_status("primary")
        assert runtime_status.commanded_state == "closed"
        assert runtime_status.commanded_position == pytest.approx(0.0)
        assert runtime_status.observed_position is None
        assert runtime_status.evidence == ["dwell_elapsed"]
        assert runtime_status.evidence_state == "unverified"

    def test_pneumatic_dwell_timeout_uses_stable_fault_token(
        self, kernel, bb_with_kernel, runtime_pause_registry
    ):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 10.0,
                "srdf_presets": {"open": 0.0081, "closed": 0.0},
                "actuation_kind": "pneumatic",
                "state_authority": "commanded_estimate",
                "completion_policy": "dwell_elapsed",
                "grasp_confirmation_policy": "none_open_loop",
                "transport": {"kind": "native_kinematic"},
                "open_state_ref": "open",
                "closed_state_ref": "closed",
            }
        }

        node = CommandGripper(
            name="close_gripper",
            gripper_id="primary",
            position=0.0,
            timeout_s=0.001,
            runtime_pause_registry=runtime_pause_registry,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.RUNNING
        time.sleep(0.01)
        assert node.update() == py_trees.common.Status.RUNNING
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].reason_code == "gripper_dwell_timeout"

        reader = py_trees.blackboard.Client(name="gripper_fault_reader_timeout")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "gripper_dwell_timeout"
        assert details["evidence"] == ["dwell_elapsed"]
        assert details["evidence_state"] == "unverified"

    def test_pneumatic_numeric_target_rejection_surfaces_semantic_fault(
        self, kernel, bb_with_kernel
    ):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.01,
                "srdf_presets": {"open": 0.0081, "closed": 0.0},
                "actuation_kind": "pneumatic",
                "state_authority": "native_backend",
                "completion_policy": "dwell_elapsed",
                "grasp_confirmation_policy": "none_open_loop",
                "transport": {"kind": "native_kinematic"},
                "open_state_ref": "open",
                "closed_state_ref": "closed",
            }
        }

        node = CommandGripper(
            name="bad_close_gripper",
            gripper_id="primary",
            position=0.004,
            timeout_s=1.0,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.FAILURE

        reader = py_trees.blackboard.Client(
            name="gripper_fault_reader_semantic_reject"
        )
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "invalid_gripper_semantics"
        assert details["actuation_kind"] == "pneumatic"
        assert details["commanded_state"] == "numeric"

    def test_command_gripper_unclassified_failure_records_nothing(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {}

        node = CommandGripper(
            name="move_gripper",
            gripper_id="missing",
            position=0.7,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.FAILURE

        reader = py_trees.blackboard.Client(name="gripper_fault_reader_2")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        assert reader.get("gripper_fault_details") is None

    def test_command_gripper_zero_timeout_disables_timeout(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
            }
        }

        node = CommandGripper(
            name="move_gripper",
            gripper_id="primary",
            position=0.7,
            duration_s=0.2,
            timeout_s=0.0,
        )
        node.setup()
        node.initialise()

        assert node.update() == py_trees.common.Status.RUNNING
        time.sleep(0.03)
        assert node.update() == py_trees.common.Status.RUNNING
        assert "Timed out" not in node.feedback_message
        assert kernel.last_gripper_command_request is not None
        assert kernel.last_gripper_command_request.duration_s == pytest.approx(0.2)

    def test_command_gripper_fails_without_position(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.02,
            }
        }

        node = CommandGripper(name="move_gripper", gripper_id="primary")
        assert _tick_node(node) == py_trees.common.Status.FAILURE
        assert "position is required" in node.feedback_message

    def test_command_gripper_fails_when_gripper_is_missing(self, kernel, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="gripper_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {}

        node = CommandGripper(
            name="move_gripper",
            gripper_id="missing",
            position=0.7,
            timeout_s=1.0,
        )
        assert _tick_node(node) == py_trees.common.Status.FAILURE

    def test_mock_kernel_rejects_overlapping_gripper_commands(self, kernel):
        first = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.7,
                duration_s=0.2,
            )
        )
        second = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.0,
                duration_s=0.2,
            )
        )

        assert first.accepted is True
        assert second.accepted is False
        assert "already executing" in (second.error or "")

    def test_mock_kernel_status_reads_are_side_effect_free(self, kernel):
        accepted = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.7,
                duration_s=0.05,
            )
        )
        assert accepted.accepted is True

        time.sleep(0.06)
        completed = kernel.get_gripper_status("primary")

        assert completed.state == GripperCommandState.COMPLETED
        assert completed.current_position == pytest.approx(0.7)
        assert completed.revision == 2
        # The observer did not commit lifecycle state or projection.
        assert kernel._gripper_commands["primary"]["state"] == (
            GripperCommandState.EXECUTING
        )
        assert kernel._gripper_positions.get("primary") is None

        repeated = kernel.get_gripper_status("primary")
        assert repeated == completed
        assert kernel._gripper_positions.get("primary") is None

    def test_mock_kernel_accepts_next_command_after_elapsed_predecessor(self, kernel):
        first = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.7,
                duration_s=0.01,
            )
        )
        assert first.accepted is True

        time.sleep(0.02)
        second = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.0,
                duration_s=0.01,
            )
        )

        assert second.accepted is True
        assert kernel.get_gripper_status("primary").target_position == pytest.approx(0.0)


class TestConfirmGripperGrasp:
    def _configure_gripper(
        self,
        kernel,
        *,
        policy: str = "discrete_signal",
    ) -> None:
        writer = py_trees.blackboard.Client(name="grasp_confirmation_setup")
        writer.register_key(key="gripper_configs", access=py_trees.common.Access.WRITE)
        writer.gripper_configs = {
            "primary": {
                "command_mode": "simulated_joint",
                "group_id": "gripper",
                "joint_name": "finger_joint",
                "motion_time_s": 0.01,
                "actuation_kind": "position_commanded",
                "state_authority": "native_backend",
                "completion_policy": "position_reached",
                "grasp_confirmation_policy": policy,
                "transport": {"kind": "native_kinematic"},
                "open_state_ref": "open",
                "closed_state_ref": "closed",
                "srdf_presets": {"open": 0.008, "closed": 0.0},
            }
        }
        result = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="gripper",
                joint_name="finger_joint",
                target_position=0.0,
                duration_s=0.0,
                actuation_kind="position_commanded",
                state_authority="native_backend",
                completion_policy="position_reached",
                grasp_confirmation_policy=policy,
                transport_kind="native_kinematic",
                open_state_ref="open",
                closed_state_ref="closed",
                srdf_presets={"open": 0.008, "closed": 0.0},
            )
        )
        assert result.accepted is True

    def test_signal_confirmation_records_verified_evidence(
        self, kernel, bb_with_kernel
    ):
        self._configure_gripper(kernel, policy="discrete_signal")
        writer = py_trees.blackboard.Client(name="signal_writer")
        writer.register_key(
            key="cell/part_present", access=py_trees.common.Access.WRITE
        )
        writer.set("cell/part_present", True)

        node = ConfirmGripperGrasp(
            name="confirm_part_present",
            gripper_id="primary",
            source_key="cell/part_present",
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        status = kernel.get_gripper_status("primary")
        assert status.evidence_state == "confirmed"
        assert "discrete_signal" in status.evidence
        assert "policy=discrete_signal" in node.feedback_message

    def test_force_current_confirmation_records_default_evidence_label(
        self,
        kernel,
        bb_with_kernel,
    ):
        self._configure_gripper(kernel, policy="force_or_current")
        writer = py_trees.blackboard.Client(name="force_current_writer")
        writer.register_key(
            key="gripper/primary/force_current_confirmed",
            access=py_trees.common.Access.WRITE,
        )
        writer.set("gripper/primary/force_current_confirmed", True)

        node = ConfirmGripperGrasp(
            name="confirm_force_current",
            gripper_id="primary",
            source_key="gripper/primary/force_current_confirmed",
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        status = kernel.get_gripper_status("primary")
        assert status.evidence_state == "confirmed"
        assert "force_or_current" in status.evidence
        assert "policy=force_or_current" in node.feedback_message

    def test_signal_confirmation_allows_evidence_backed_attach(
        self, kernel, bb_with_kernel
    ):
        self._configure_gripper(kernel, policy="discrete_signal")
        signal_writer = py_trees.blackboard.Client(name="signal_writer_attach")
        signal_writer.register_key(
            key="cell/part_present", access=py_trees.common.Access.WRITE
        )
        signal_writer.set("cell/part_present", True)
        assert _tick_node(
            ConfirmGripperGrasp(
                name="confirm_part_present",
                gripper_id="primary",
                source_key="cell/part_present",
            )
        ) == py_trees.common.Status.SUCCESS

        bb_with_kernel.register_key(
            key="scene_objects", access=py_trees.common.Access.WRITE
        )
        bb_with_kernel.scene_objects = {}
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.6, 0.0, 0.6],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )

        node = AttachObjectToLink(
            name="attach_box",
            object_id="box1",
            link_frame="tool0",
            gripper_id="primary",
        )

        assert _tick_node(node) == py_trees.common.Status.SUCCESS
        assert kernel.get_object_state("box1").attached is True

    def test_confirmation_rejects_false_source_signal(
        self, kernel, bb_with_kernel, runtime_pause_registry
    ):
        self._configure_gripper(kernel, policy="discrete_signal")
        writer = py_trees.blackboard.Client(name="signal_writer_false")
        writer.register_key(
            key="cell/part_present", access=py_trees.common.Access.WRITE
        )
        writer.set("cell/part_present", False)

        node = ConfirmGripperGrasp(
            name="confirm_part_present",
            gripper_id="primary",
            source_key="cell/part_present",
            runtime_pause_registry=runtime_pause_registry,
        )

        assert _tick_node(node) == py_trees.common.Status.RUNNING
        faults = runtime_pause_registry.active_faults()
        assert len(faults) == 1
        assert faults[0].domain == "gripper"
        assert faults[0].reason_code == "grasp_confirmation_lost"
        reader = py_trees.blackboard.Client(name="confirm_fault_reader")
        reader.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.READ
        )
        details = reader.get("gripper_fault_details")
        assert details["reason_code"] == "grasp_confirmation_lost"
        assert details["surface"] == "confirm_gripper_grasp"
        assert details["grasp_confirmation_policy"] == "discrete_signal"

    def test_confirmation_rejects_none_open_loop_policy(self, kernel, bb_with_kernel):
        self._configure_gripper(kernel, policy="none_open_loop")

        node = ConfirmGripperGrasp(
            name="confirm_open_loop",
            gripper_id="primary",
        )

        assert _tick_node(node) == py_trees.common.Status.FAILURE
        assert "cannot be converted" in node.feedback_message


class TestGetLinkPose:
    def test_success(self, kernel, bb_with_kernel):
        node = GetLinkPose(
            name="get_pose",
            target_frame="tool0",
            base_frame="base_link",
            output_key="tool_pose",
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS
        assert kernel.link_pose_calls == [("tool0", "base_link")]

        reader = py_trees.blackboard.Client(name="reader")
        reader.register_key(key="tool_pose", access=py_trees.common.Access.READ)
        assert "position_m" in reader.tool_pose


class TestCheckPoseDistance:
    def test_within_threshold(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="pose_a", access=py_trees.common.Access.WRITE)
        writer.register_key(key="pose_b", access=py_trees.common.Access.WRITE)
        writer.pose_a = {
            "position_m": [1.0, 0.0, 0.5],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        writer.pose_b = {
            "position_m": [1.001, 0.0, 0.5],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

        node = CheckPoseDistance(
            name="check_dist",
            pose_key_a="pose_a",
            pose_key_b="pose_b",
            translation_threshold_m=0.01,
            rotation_threshold_rad=0.1,
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.SUCCESS

    def test_exceeds_threshold(self, bb_with_kernel):
        writer = py_trees.blackboard.Client(name="writer")
        writer.register_key(key="pose_a", access=py_trees.common.Access.WRITE)
        writer.register_key(key="pose_b", access=py_trees.common.Access.WRITE)
        writer.pose_a = {
            "position_m": [0.0, 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
        writer.pose_b = {
            "position_m": [1.0, 1.0, 1.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }

        node = CheckPoseDistance(
            name="check_dist_fail",
            pose_key_a="pose_a",
            pose_key_b="pose_b",
            translation_threshold_m=0.01,
            rotation_threshold_rad=0.1,
        )
        status = _tick_node(node)
        assert status == py_trees.common.Status.FAILURE
