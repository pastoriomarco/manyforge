"""Tests for the CycleManager."""

from pathlib import Path
import threading
import time
import tempfile
import os
import pytest
import py_trees
import yaml

import manyforge_behavior.cycle_manager as cycle_manager_module
from manyforge_behavior.capabilities import V0_0_REQUIRED_KEYS
from manyforge_behavior.kernel_client import (
    CommandResult,
    ExecutionState,
    ExecutionStatus,
    MockKernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.signals import SignalUpdate
from manyforge_behavior.cycle_manager import (
    ROS2_CONTROL_BACKEND_FAULT_CLASSES,
    CycleManager,
    CycleState,
    InterventionSession,
    RecoveryTrigger,
    _scene_resume_fingerprint_from_objects,
    canonical_recovery_trigger_values,
)

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)


def _write_yaml_file(data: dict) -> str:
    handle = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.safe_dump(data, handle, sort_keys=False)
    handle.close()
    return handle.name


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


class _ContainmentRejectingKernel(MockKernelClient):
    """Mock the RT-8P0 authority retaining an interrupted retry."""

    def __init__(self, *args, retryable: bool = True, **kwargs):
        super().__init__(*args, **kwargs)
        self._interrupted_retryable = retryable

    def acknowledge_interrupted_for_retry(
        self,
        principal_id: str,
        reason: str,
        *,
        expected_epoch: int | None = None,
    ) -> CommandResult:
        del principal_id, reason, expected_epoch
        self.acknowledge_interrupted_for_retry_count += 1
        return CommandResult(
            accepted=False,
            error="runtime-collision stop is not yet certified",
            reason_code="runtime_collision_stop_not_yet_certified",
            details={
                "retryable": "true" if self._interrupted_retryable else "false"
            },
        )


class TestCycleManagerInitialState:
    """Test initial state of CycleManager."""
    
    def test_initial_state_idle(self, kernel):
        """CycleManager starts in IDLE state."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.state == CycleState.IDLE
    
    def test_velocity_scale_default_1_0(self, kernel):
        """Velocity scale defaults to 1.0."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.velocity_scale == 1.0


class TestCycleManagerRun:
    """Test run() method."""
    
    def test_run_from_idle_starts_ticking(self, kernel):
        """run() from IDLE builds tree and starts tick thread."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.run()
        
        assert manager.state == CycleState.RUNNING
        # Give tick thread time to run
        time.sleep(0.2)
        # Tree should complete quickly, so state may change
        # But we know it started running

    def test_run_from_idle_after_reset_reuses_prepared_tree_and_scene(self, kernel):
        """Idle start after Reset uses the prepared baseline without rebuilding scene/tree again."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.reset()
        initial_tree = manager._tree
        initial_scene_edit_count = len(slow_kernel.scene_edits)

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert manager._tree is not None
        assert manager._tree is initial_tree
        assert len(slow_kernel.scene_edits) == initial_scene_edit_count

    def test_reset_clears_motion_soft_fail_blackboard(self, kernel):
        """Reset starts a fresh cycle, not a resume of a previous motion soft-fail."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        bb = py_trees.blackboard.Client(name="test_motion_soft_fail_reset")
        bb.register_key(key="motion_paused_due_to_error", access=py_trees.common.Access.WRITE)
        bb.register_key(key="motion_error_message", access=py_trees.common.Access.WRITE)
        bb.register_key(key="motion_paused_node_name", access=py_trees.common.Access.WRITE)
        bb.register_key(key="resume_after_error_requested", access=py_trees.common.Access.WRITE)
        bb.set("motion_paused_due_to_error", True)
        bb.set("motion_error_message", "stale collision")
        bb.set("motion_paused_node_name", "stale_motion")
        bb.set("resume_after_error_requested", True)

        manager.reset()

        assert bb.get("motion_paused_due_to_error") is False
        assert bb.get("motion_error_message") is None
        assert bb.get("motion_paused_node_name") is None
        assert bb.get("resume_after_error_requested") is False
    
    def test_run_from_running_is_noop(self, kernel):
        """run() from RUNNING state is a no-op."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        # Call run again — should be no-op
        manager.run()
        assert manager.state == CycleState.RUNNING
    
    def test_run_from_stopped_resumes(self, kernel):
        """run() from STOPPED resumes execution."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            # Run then stop
            manager.run()
            time.sleep(0.2)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            # Run again — should resume
            manager.run()
            assert manager.state == CycleState.RUNNING
        finally:
            manager.reset()

    def test_run_from_stopped_rejects_if_live_scene_changed(self, kernel):
        """A resumable STOPPED cycle owns the live scene state.

        If an external route detaches/moves/removes scene objects while stopped,
        the memory behavior tree may otherwise resume past an already-successful
        attach node with stale assumptions. Run must fail closed instead.
        """

        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            time.sleep(0.2)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            result = slow_kernel.scene_edit(
                SceneEditRequest(
                    operation="update_pose",
                    object_id="table",
                    pose_position_m=[0.3, 0.1, 0.2],
                    pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                    frame_id="base_link",
                )
            )
            assert result.success is True

            with pytest.raises(RuntimeError, match="live scene changed"):
                manager.run()
            assert manager.state == CycleState.STOPPED
            assert manager.error_code == "resumable_scene_changed"
        finally:
            manager.reset()

    def test_run_from_stopped_rejects_if_attached_object_detached(self, kernel):
        """Resume must not trust a memory tree after held attachment is lost."""

        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            attach = slow_kernel.scene_edit(
                SceneEditRequest(
                    operation="attach",
                    object_id="table",
                    frame_id="tool0",
                )
            )
            assert attach.success is True
            time.sleep(0.2)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            detach = slow_kernel.scene_edit(
                SceneEditRequest(operation="detach", object_id="table")
            )
            assert detach.success is True

            with pytest.raises(RuntimeError, match="live scene changed"):
                manager.run()
            assert manager.state == CycleState.STOPPED
            assert manager.error_code == "resumable_scene_changed"
        finally:
            manager.reset()

    def test_attached_scene_fingerprint_uses_attachment_frame_not_world_pose(self):
        """Real feedback/FK drift must not look like an attachment mutation."""

        shape = SceneShape(type="box", box_dimensions_m=[0.1, 0.2, 0.3])
        first = SceneObjectSnapshot(
            object_id="part",
            shape=shape,
            pose_in_frame={
                "position_m": [0.01, 0.0, 0.02],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            pose_in_universe={
                "position_m": [1.0, 2.0, 3.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            attached=True,
            frame_id="tool0",
        )
        second = SceneObjectSnapshot(
            object_id="part",
            shape=shape,
            pose_in_frame={
                "position_m": [0.01, 0.0, 0.02],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            pose_in_universe={
                "position_m": [1.1, 2.1, 3.1],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            attached=True,
            frame_id="tool0",
        )

        assert _scene_resume_fingerprint_from_objects([first]) == (
            _scene_resume_fingerprint_from_objects([second])
        )

    def test_run_from_held_motion_soft_fail_clears_interrupted_kernel_retry(
        self, kernel
    ):
        """Run from held motion stop is an explicit retry after a motion soft-fail.

        Runtime collision faults leave the kernel in INTERRUPTED with an
        active latch. Removing the obstacle fixes the scene but does not clear
        that execution-local latch. The cycle keeps ticking with motion held;
        the operator pressing Run means "retry this held motion". CycleManager
        must acknowledge the interrupted kernel execution boundary first so the
        motion node can submit a fresh execute request.
        This must not call kernel.reset(), because ROS/Isaac reset can command
        the robot back to its initial pose. If the obstacle is still present,
        the fresh execute/monitor path will latch a new collision.
        """
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        bb = py_trees.blackboard.Client(name="test_motion_retry_after_collision")
        bb.register_key(key="motion_paused_due_to_error", access=py_trees.common.Access.READ)
        bb.register_key(key="motion_error_message", access=py_trees.common.Access.READ)
        bb.register_key(key="motion_paused_node_name", access=py_trees.common.Access.READ)
        bb.register_key(key="resume_after_error_requested", access=py_trees.common.Access.READ)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1

            # Simulate a runtime collision fault while the authored motion node
            # still owns the in-flight execution. This preserves the node's
            # internal soft-fail state, matching the live path that Run must
            # resume. Fabricating only blackboard keys after reset would build a
            # fresh node that has no _soft_fail_error to consume.
            initial_reset_count = slow_kernel.reset_count
            slow_kernel._state = ExecutionState.INTERRUPTED

            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"
            assert manager._tick_pause_event.is_set()
            assert bb.get("motion_paused_due_to_error") is False
            assert bb.get("motion_error_message") is None
            previous_execute_count = slow_kernel.execute_count

            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == previous_execute_count and time.time() < deadline:
                time.sleep(0.01)

            assert slow_kernel.reset_count == initial_reset_count
            assert slow_kernel.acknowledge_interrupted_for_retry_count == 1
            assert slow_kernel.execute_count > previous_execute_count
            assert bb.get("motion_paused_due_to_error") is False
            assert bb.get("motion_error_message") is None
            assert bb.get("motion_paused_node_name") is None
            assert bb.get("resume_after_error_requested") is False
            assert manager.state == CycleState.RUNNING
        finally:
            manager.reset()

    def test_run_keeps_held_motion_recovery_when_kernel_retains_retry_authority(
        self,
    ):
        """A settling preemption stays held without becoming a cycle error."""

        retry_kernel = _ContainmentRejectingKernel(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        retry_kernel._state = ExecutionState.INTERRUPTED
        with manager._lock:
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "runtime collision"
            manager._error_code = "runtime_collision"

        manager.run()

        assert retry_kernel.acknowledge_interrupted_for_retry_count == 1
        assert retry_kernel.get_status().state == ExecutionState.INTERRUPTED
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "held"
        assert manager.error == "runtime collision"
        assert manager.error_code == "runtime_collision"

    def test_run_keeps_stopped_recovery_when_kernel_retains_retry_authority(
        self,
    ):
        """STOPPED retry remains pending until the kernel changes its offer."""

        retry_kernel = _ContainmentRejectingKernel(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        retry_kernel._state = ExecutionState.INTERRUPTED
        bb = py_trees.blackboard.Client(name="test_stopped_retry_containment")
        bb.register_key(
            key="motion_paused_due_to_error",
            access=py_trees.common.Access.WRITE,
        )
        bb.register_key(
            key="motion_error_message",
            access=py_trees.common.Access.WRITE,
        )
        bb.register_key(
            key="stop_requested",
            access=py_trees.common.Access.WRITE,
        )
        bb.register_key(
            key="motion_stop_active",
            access=py_trees.common.Access.WRITE,
        )
        bb.set("motion_paused_due_to_error", True)
        bb.set("motion_error_message", "runtime collision")
        bb.set("stop_requested", True)
        bb.set("motion_stop_active", True)
        pending_recovery = object()
        with manager._lock:
            manager._state = CycleState.STOPPED
            manager._motion_stop_state = "held"
            manager._error = "runtime collision"
            manager._error_code = "runtime_collision"
            manager._pending_delegated_recovery = pending_recovery

        manager.run()

        assert retry_kernel.acknowledge_interrupted_for_retry_count == 1
        assert retry_kernel.get_status().state == ExecutionState.INTERRUPTED
        assert manager.state == CycleState.STOPPED
        assert manager.motion_stop_state == "held"
        assert manager.error == "runtime collision"
        assert manager.error_code == "runtime_collision"
        assert manager._pending_delegated_recovery is pending_recovery
        assert bb.get("motion_paused_due_to_error") is True
        assert bb.get("motion_error_message") == "runtime collision"
        assert bb.get("stop_requested") is True
        assert bb.get("motion_stop_active") is True

    def test_run_surfaces_non_retryable_containment_rejection(self):
        """A definitive failed preemption is not mislabeled as settling."""

        retry_kernel = _ContainmentRejectingKernel(retryable=False)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        retry_kernel._state = ExecutionState.INTERRUPTED
        with manager._lock:
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"

        with pytest.raises(
            RuntimeError,
            match="kernel.acknowledge_interrupted_for_retry rejected",
        ):
            manager.run()

        assert retry_kernel.acknowledge_interrupted_for_retry_count == 1
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "held"

    def test_run_promotes_pending_motion_soft_fail_before_noop(self, kernel):
        """Run must not 200/no-op while a retryable motion fault is pending.

        The motion node writes its soft-fail signal on the blackboard, then the
        cycle-manager tick loop promotes that signal into `motion_stop_state =
        held`. An operator can press Run in that small window. Treating RUNNING
        + idle motion-stop as a no-op leaves the collision banner latched even
        though the operator explicitly requested retry.
        """
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        bb = py_trees.blackboard.Client(name="test_pending_motion_soft_fail_run")
        bb.register_key(
            key="motion_paused_due_to_error",
            access=py_trees.common.Access.WRITE,
        )
        bb.register_key(key="motion_error_message", access=py_trees.common.Access.WRITE)
        bb.register_key(
            key="motion_paused_node_name",
            access=py_trees.common.Access.WRITE,
        )
        bb.register_key(
            key="resume_after_error_requested",
            access=py_trees.common.Access.WRITE,
        )
        bb.set("motion_paused_due_to_error", True)
        bb.set("motion_error_message", "Motion execution ended in interrupted")
        bb.set("motion_paused_node_name", "move_to_pick_approach")
        bb.set("resume_after_error_requested", False)
        slow_kernel._state = ExecutionState.INTERRUPTED

        with manager._lock:
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "idle"

        manager.run()

        assert slow_kernel.acknowledge_interrupted_for_retry_count == 1
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "idle"
        assert manager.error is None
        assert manager.error_code is None
        assert bb.get("motion_paused_due_to_error") is False
        assert bb.get("motion_error_message") is None
        assert bb.get("motion_paused_node_name") is None
        assert bb.get("resume_after_error_requested") is True

    def test_node_failure_feedback_is_exposed_as_cycle_error(self, kernel, tmp_path):
        """Clean node failures should surface node feedback in manager.error."""
        failing_program = tmp_path / "missing_bb.program.yaml"
        failing_program.write_text(
            """api_version: manyforge.program.v0
program_profile: v0.0
name: missing_bb_failure
description: Motion node fails on missing blackboard key.
catalog_version: "0.2.0"

robot:
  robot_id: ur5
  group_id: arm
  base_frame: base_link
  tcp_frame: tool0

scene: []

parameters: []

blackboard:
  namespaces:
    - id: missing_pose
      contract: query_model_output_v1
      prefix: missing
      fields:
        - name: pose
          type: pose

tree:
  id: sequence
  name: main_sequence
  children:
    - id: move_manipulator_action
      name: move_missing_pose_goal
      params:
        move_id: missing_pose
        motion_type: pose_goal
        pose_goal:
          field_ref:
            namespace: missing_pose
            field: pose
        planning_timeout_s: 0.5
        execution_timeout_s: 0.5
""",
            encoding="utf-8",
        )
        runner = OrchestratorRunner(CATALOG_PATH, str(failing_program), kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        deadline = time.time() + 1.0
        while manager.state == CycleState.RUNNING and time.time() < deadline:
            time.sleep(0.01)

        assert manager.state == CycleState.IDLE
        assert manager.error is not None
        assert "move_missing_pose_goal" in manager.error
        assert "Missing blackboard key 'missing/value/pose'" in manager.error


class TestCycleManagerStop:
    """Test stop() method."""
    
    def test_stop_from_running(self, kernel):
        """stop() from RUNNING sets state to STOPPED."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)
        
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

    def test_stop_exposes_stopping_state_and_run_is_noop_until_settled(self):
        """The HMI must see the controlled-stop window as STOPPING so Run can
        stay disabled and Stop can render "Stopping" until the backend returns."""

        class BlockingStopKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=1.0)
                self.stop_entered = threading.Event()
                self.release_stop = threading.Event()

            def stop(self):  # type: ignore[override]
                self.stop_entered.set()
                assert self.release_stop.wait(timeout=2.0)
                return super().stop()

        slow_kernel = BlockingStopKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)

        stop_thread = threading.Thread(target=manager.full_stop, daemon=True)
        stop_thread.start()
        assert slow_kernel.stop_entered.wait(timeout=2.0)
        assert manager.state == CycleState.STOPPING

        manager.run()
        assert manager.state == CycleState.STOPPING

        slow_kernel.release_stop.set()
        stop_thread.join(timeout=2.0)
        assert not stop_thread.is_alive()
        assert manager.state == CycleState.STOPPED

    def test_stop_remains_stopping_until_async_kernel_stop_terminal(self):
        """The production kernel accepts stop asynchronously. Cycle STOPPING
        must remain visible after kernel.stop() returns until the kernel itself
        reaches a terminal state."""

        class AsyncStoppingKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=1.0)
                self.stop_called = threading.Event()
                self.release_terminal = threading.Event()
                self._reported_state = ExecutionState.EXECUTING

            def stop(self):  # type: ignore[override]
                self.stop_called.set()
                self._reported_state = ExecutionState.STOPPING
                return CommandResult(accepted=True)

            def get_status(self):  # type: ignore[override]
                if self.release_terminal.is_set():
                    self._reported_state = ExecutionState.STOPPED
                return ExecutionStatus(
                    state=self._reported_state,
                    execution_id=self._execution_id,
                    progress=0.0,
                    active_move_id=self._active_move_id,
                )

        slow_kernel = AsyncStoppingKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)

        manager.full_stop()
        assert slow_kernel.stop_called.wait(timeout=2.0)
        assert manager.state == CycleState.STOPPING

        manager.run()
        assert manager.state == CycleState.STOPPING

        slow_kernel.release_terminal.set()
        deadline = time.time() + 2.0
        while manager.state == CycleState.STOPPING and time.time() < deadline:
            time.sleep(0.01)
        assert manager.state == CycleState.STOPPED
    
    def test_tick_loop_sleeps_via_interruptible_stop_event(self, kernel):
        """Audit 6D (finding 2.9): the tick loop's inter-tick sleep must be
        `_tick_stop_event.wait(timeout=...)` so a stop request wakes it
        immediately; reverting to a bare time.sleep() fails this guard."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        wait_timeouts = []
        original_wait = manager._tick_stop_event.wait

        def recording_wait(timeout=None):
            if timeout is not None:
                wait_timeouts.append(timeout)
            return original_wait(timeout=timeout)

        manager._tick_stop_event.wait = recording_wait
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        assert wait_timeouts, (
            "tick loop must sleep via _tick_stop_event.wait(timeout=...), "
            "not time.sleep()"
        )

    def test_paused_tick_loop_uses_bounded_event_wait(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._tick_pause_event.clear()
        manager._tick_stop_event.clear()
        wait_timeouts = []

        def recording_wait(timeout=None):
            wait_timeouts.append(timeout)
            manager._tick_stop_event.set()
            return False

        manager._tick_pause_event.wait = recording_wait
        manager._tick_loop()

        assert wait_timeouts == [cycle_manager_module._PAUSED_TICK_WAIT_S]

    def test_stop_from_stopped_is_noop(self, kernel):
        """stop() from STOPPED is a no-op."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        
        # Stop again — should be no-op
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
    
    def test_stop_from_idle_is_noop(self, kernel):
        """stop() from IDLE is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.full_stop()
        assert manager.state == CycleState.IDLE
    
    def test_stop_routes_through_controlled_stop_not_pause(self, kernel):
        """Operator stop() routes the in-flight motion through the kernel's
        controlled stop (kernel.stop()), not a resumable pause. The controlled
        stop is the industrial default operator-stop path."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)
        manager.full_stop()

        # Controlled stop, not pause: kernel.stop() invoked, kernel.pause() not.
        assert slow_kernel.stop_count == 1
        assert slow_kernel.pause_count == 0


class TestCycleManagerMotionStop:
    """Test the cooperative operator Motion Stop path."""

    @staticmethod
    def _parallel_motion_timer_program(
        timer_duration_s: float = 0.2,
        *,
        success_policy: str | None = None,
    ) -> str:
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.1" if success_policy is not None else "v0.0",
            "name": "motion_stop_parallel_test",
            "description": "Motion stop cooperative ticking test.",
            "catalog_version": "0.2.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": {
                "id": "parallel",
                "name": "root_parallel",
                "children": [
                    {
                        "id": "move_manipulator_action",
                        "name": "held_motion",
                        "params": {
                            "move_id": "held_motion",
                            "motion_type": "joint_goal",
                            "joint_goal_names": ["j1", "j2", "j3", "j4", "j5", "j6"],
                            "joint_goal_positions_rad": [0.1, -0.8, 0.7, -1.0, -0.9, 0.2],
                            "planning_timeout_s": 2.0,
                            "execution_timeout_s": 10.0,
                        },
                    },
                    {
                        "id": "timer",
                        "name": "background_wait",
                        "operator_stop_policy": "continue",
                        "params": {"duration_s": timer_duration_s},
                    },
                ],
            },
        }
        if success_policy is not None:
            data["tree"]["params"] = {"success_policy": success_policy}
        return _write_yaml_file(data)

    def test_motion_stop_keeps_cycle_running_and_holds_motion(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1

            manager.stop()

            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"
            assert manager._tick_pause_event.is_set()
            assert slow_kernel.stop_count == 1
        finally:
            manager.full_stop()

    def test_motion_stop_resume_redispatches_held_motion(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            manager.stop()
            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            previous_execute_count = slow_kernel.execute_count

            manager.run()

            deadline = time.time() + 1.0
            while slow_kernel.execute_count == previous_execute_count and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert slow_kernel.execute_count > previous_execute_count
        finally:
            manager.full_stop()

    def test_motion_stop_resume_acknowledges_natural_completion(self):
        class NaturalCompletionStopKernel(MockKernelClient):
            def __init__(self) -> None:
                super().__init__(execute_duration_s=1.0)

            def stop(self):  # type: ignore[override]
                self.stop_count += 1
                if self._state != ExecutionState.EXECUTING:
                    return CommandResult(
                        accepted=False, error="Cannot stop from current state"
                    )
                self._emit_kernel_event("stop_requested")
                self._emit_kernel_event(
                    "state_transition", from_state="executing", to_state="stopping"
                )
                self._state = ExecutionState.COMPLETED
                self._execute_start = None
                self._metrics["executions_completed"] += 1
                self._emit_kernel_event("execute_completed")
                self._emit_kernel_event(
                    "state_transition", from_state="stopping", to_state="completed"
                )
                return CommandResult(accepted=True)

        kernel = NaturalCompletionStopKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert kernel.execute_count == 1

            manager.stop()
            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"
            assert kernel.get_status().state == ExecutionState.COMPLETED
            previous_execute_count = kernel.execute_count

            manager.run()

            deadline = time.time() + 1.0
            while kernel.execute_count == previous_execute_count and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert kernel.execute_count > previous_execute_count
            assert manager.error is None
        finally:
            manager.full_stop()

    def test_parallel_continue_branch_ticks_while_motion_is_held(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.2)
        runner = OrchestratorRunner(CATALOG_PATH, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            manager.stop()
            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)

            deadline = time.time() + 1.0
            while (
                manager.get_node_statuses().get("background_wait") != "SUCCESS"
                and time.time() < deadline
            ):
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"
            assert manager.get_node_statuses().get("background_wait") == "SUCCESS"
        finally:
            manager.full_stop()
            os.unlink(program_path)

    def test_motion_soft_fail_keeps_continue_branch_ticking(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.3)
        runner = OrchestratorRunner(CATALOG_PATH, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1
            assert manager.get_node_statuses().get("background_wait") != "SUCCESS"

            slow_kernel._state = ExecutionState.INTERRUPTED

            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"
            assert manager._tick_pause_event.is_set()

            deadline = time.time() + 1.0
            while (
                manager.get_node_statuses().get("background_wait") != "SUCCESS"
                and time.time() < deadline
            ):
                time.sleep(0.01)
            assert manager.get_node_statuses().get("background_wait") == "SUCCESS"
        finally:
            manager.full_stop()
            os.unlink(program_path)

    def test_motion_soft_fail_terminal_tree_clears_motion_hold(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(
            timer_duration_s=0.25,
            success_policy="success_on_one",
        )
        runner = OrchestratorRunner(CATALOG_PATH, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        bb = py_trees.blackboard.Client(name="test_terminal_motion_hold_clear")
        bb.register_key(key="motion_stop_active", access=py_trees.common.Access.READ)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)

            slow_kernel._state = ExecutionState.INTERRUPTED

            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"

            deadline = time.time() + 1.0
            while manager.state != CycleState.IDLE and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.IDLE
            assert manager.motion_stop_state == "idle"
            assert bb.get("motion_stop_active") is False
        finally:
            manager.full_stop()
            os.unlink(program_path)

    def test_motion_soft_fail_intervention_required_fails_closed(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.3)
        runner = OrchestratorRunner(CATALOG_PATH, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1

            slow_kernel._state = ExecutionState.INTERVENTION_REQUIRED

            deadline = time.time() + 1.0
            while manager.state != CycleState.STOPPED and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.STOPPED
            assert manager.motion_stop_state == "idle"
            assert not manager._tick_pause_event.is_set()
            assert manager.error is not None
        finally:
            manager.reset()
            os.unlink(program_path)

    def test_held_motion_soft_fail_later_intervention_required_fails_closed(self):
        """A retryable held motion stop must not stay resumable after escalation.

        The live runtime-collision path can first publish an interrupted soft
        failure, then later escalate to INTERVENTION_REQUIRED if controlled stop
        confirmation fails. The cycle must switch from held motion resume to the
        explicit acknowledge/reset recovery boundary.
        """
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.3)
        runner = OrchestratorRunner(CATALOG_PATH, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            deadline = time.time() + 1.0
            while slow_kernel.execute_count == 0 and time.time() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1

            slow_kernel._state = ExecutionState.INTERRUPTED
            deadline = time.time() + 1.0
            while manager.motion_stop_state != "held" and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "held"

            slow_kernel._state = ExecutionState.INTERVENTION_REQUIRED
            slow_kernel._emit_kernel_event(  # noqa: SLF001 - inject native transition.
                "state_transition",
                from_state="interrupted",
                to_state="intervention_required",
            )

            deadline = time.time() + 1.0
            while manager.state != CycleState.STOPPED and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.STOPPED
            assert manager.motion_stop_state == "idle"
            assert not manager._tick_pause_event.is_set()
            assert manager.error_code == "motion_soft_fail_requires_intervention"
            assert manager.error_details is not None
            assert (
                manager.error_details["motion_soft_stop_reason"]
                == "motion_soft_fail_requires_intervention"
            )
            assert manager.error_details["kernel_state"] == "intervention_required"
        finally:
            manager.reset()
            os.unlink(program_path)


class TestCycleManagerReset:
    """Test reset() method."""
    
    def test_reset_from_running(self, kernel):
        """reset() from RUNNING stops then resets."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        manager.reset()
        assert manager.state == CycleState.IDLE
    
    def test_reset_from_stopped(self, kernel):
        """reset() from STOPPED resets to IDLE."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        
        manager.reset()
        assert manager.state == CycleState.IDLE
        assert manager.error is None
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()
    
    def test_reset_from_idle_is_noop(self, kernel):
        """reset() from IDLE prepares a fresh software/runtime cycle state."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        assert manager.state == CycleState.IDLE
        initial_reset_count = kernel.reset_count

        manager.reset()
        assert manager.state == CycleState.IDLE
        assert manager._tree is not None
        assert kernel.get_object_state("table").exists is True
        # Kernel reset is not used as a synthetic state-assignment path when already idle.
        assert kernel.reset_count == initial_reset_count

    def test_reset_records_phase_events(self, kernel):
        """Reset emits durable phase spans so a slow reset is diagnosable."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        events: list[dict] = []
        manager.set_on_reset_phase_event(events.append)

        manager.reset(request_id="req-reset-1")

        by_phase_status = {
            (event["phase"], event["status"]): event for event in events
        }
        assert ("kernel_status", "started") in by_phase_status
        assert ("kernel_status", "completed") in by_phase_status
        assert ("scene_clear", "completed") in by_phase_status
        assert ("tree_rebuild", "completed") in by_phase_status
        assert ("scene_apply", "completed") in by_phase_status
        assert (
            by_phase_status[("scene_apply", "completed")]["request_id"]
            == "req-reset-1"
        )
        assert "duration_ms" in by_phase_status[("scene_apply", "completed")]

    def test_slow_kernel_reset_fails_closed_after_phase_deadline(self, monkeypatch):
        """A reset phase that exceeds the bounded budget must not report IDLE."""

        class SlowResetKernel(MockKernelClient):
            def reset(self, *, expected_epoch=None):  # type: ignore[override]
                time.sleep(0.02)
                return super().reset(expected_epoch=expected_epoch)

        monkeypatch.setattr(cycle_manager_module, "_RESET_PHASE_DEADLINE_S", 0.001)
        slow_kernel = SlowResetKernel()
        slow_kernel._state = ExecutionState.STOPPED  # noqa: SLF001
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        events: list[dict] = []
        manager.set_on_reset_phase_event(events.append)

        with pytest.raises(RuntimeError, match="kernel_reset"):
            manager.reset(request_id="req-timeout")

        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "reset_kernel_reset_timeout"
        failed = [
            event
            for event in events
            if event["phase"] == "kernel_reset" and event["status"] == "failed"
        ]
        assert failed
        assert failed[-1]["request_id"] == "req-timeout"

    def test_reset_refusal_is_not_reported_as_idle(self):
        """A refused kernel reset must stay visible; CycleManager must not
        force a fresh IDLE cycle over an uncleared kernel fault."""

        class RefusingResetKernel(MockKernelClient):
            def reset(self, *, expected_epoch=None):  # type: ignore[override]
                self.reset_count += 1
                if stale := self._stale_control_command(expected_epoch):
                    return stale
                return CommandResult(
                    accepted=False,
                    error="reset refused by test",
                    reason_code="test_reset_refused",
                )

        refused_kernel = RefusingResetKernel()
        refused_kernel._state = ExecutionState.INTERVENTION_REQUIRED
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, refused_kernel)
        manager = CycleManager(runner, refused_kernel)

        manager.reset()

        assert manager.state == CycleState.STOPPED
        assert manager.error == "reset refused by test"
        assert manager.error_code == "test_reset_refused"
        assert refused_kernel.reset_count == 1

    def test_reset_removes_runtime_added_scene_objects(self, kernel):
        """Reset must clear scene objects that were added at runtime so the
        planning scene matches the operator's authored intent. Without this,
        a wall/obstacle spawned during a cycle (BT node, REST edit, etc.)
        persists in the backend after reset and silently shapes the next
        plan."""
        from manyforge_behavior.kernel_client import SceneEditRequest

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        # Materialise the authored scene first.
        manager.reset()
        assert kernel.get_object_state("table").exists is True

        # Simulate something adding an object at runtime (this is what the
        # AddCollisionObject BT node, an assistant runtime override, or a
        # scene-edit REST call would do).
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="runtime_wall",
                shape_type="box",
                box_dimensions_m=[0.02, 0.4, 0.8],
                pose_position_m=[-0.3, 0.4, 0.4],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        assert kernel.get_object_state("runtime_wall").exists is True

        manager.reset()
        # Authored object survives, runtime-added object is gone.
        assert kernel.get_object_state("table").exists is True
        assert kernel.get_object_state("runtime_wall").exists is False

    def test_reset_detaches_and_removes_runtime_attached_objects(self, kernel):
        """Reset means 'robot is back to known authored state, not holding
        anything from the previous cycle'. A runtime-added object that was
        attached during the cycle (e.g., a part the BT picked up) must be
        both detached and removed — it is not in the authored scene, so
        apply_scene() will not re-add it."""
        from manyforge_behavior.kernel_client import SceneEditRequest

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.reset()

        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="runtime_part",
                shape_type="box",
                box_dimensions_m=[0.05, 0.05, 0.05],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        kernel.scene_edit(
            SceneEditRequest(
                operation="attach",
                object_id="runtime_part",
                frame_id="tool0",
            )
        )
        assert kernel.get_object_state("runtime_part").attached is True

        manager.reset()
        # Runtime-added attached object: detached and removed entirely.
        assert kernel.get_object_state("runtime_part").exists is False

    def test_reset_detaches_authored_attached_objects_and_re_adds_them(self, kernel):
        """Authored objects that became attached during the prior cycle (the
        normal pick-and-place case: `attach_cheese_box` succeeded, then the
        cycle was stopped and reset) must be detached and re-added at the
        authored pose. Without the detach, apply_scene() would call
        scene_edit(add, cheese_box) on a still-existing object and fail with
        'Object already exists', breaking reset after a pick. The pick flow
        is exactly the scenario the user is testing with Isaac live."""
        from manyforge_behavior.kernel_client import SceneEditRequest

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.reset()
        # Authored objects are present after the initial scene apply.
        assert kernel.get_object_state("table").exists is True

        # Simulate a successful pick: an authored object becomes attached.
        kernel.scene_edit(
            SceneEditRequest(
                operation="attach",
                object_id="table",
                frame_id="tool0",
            )
        )
        assert kernel.get_object_state("table").attached is True

        # Reset must succeed (the bug: apply_scene would re-add `table` and
        # the kernel would reject "Object already exists").
        manager.reset()

        # Authored object survives reset but is no longer attached.
        state = kernel.get_object_state("table")
        assert state.exists is True
        assert state.attached is False

    def test_event_harvest_prefers_collision_pair_over_latch(self, kernel):
        """Issue #1/#3 rich-error rendering: when a runtime fault fires,
        the kernel publishes a `collision_detected` event (carrying the
        colliding pair, classification, waypoint, snapshot revision)
        FIRST, then `interrupt_latched` (policy + latch identity). A
        naive newest-first walk would surface the latch event and discard
        the colliding-pair diagnostic, leaving the viewer with only
        'Motion interrupted by runtime_collision_fault' when the real
        operator-actionable info is 'wall <-> wrist_1_link at waypoint 0'.

        This test pins the priority: collision_detected wins regardless
        of buffer order, and the harvest emits the `runtime_collision`
        code (not `validation_collision` — pre-execute validation
        rejection is a distinct operator event)."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        # Mirror the live event sequence: collision_detected at lower
        # event_index, interrupt_latched after it. The kernel's runtime
        # collision monitor emits in this order.
        collision_details = {
            "collision_result_first_failure_pair":
                "object/wall/shape/0 <-> robot/robot0/link/wrist_1_link/collision/0",
            "collision_result_first_failure_classification": "blocking_collision",
            "collision_result_waypoint_index": "0",
            "reason_code": "runtime_collision_monitor",
            "surface": "observed_runtime_monitor",
        }
        latch_details = {
            "policy_id": "manyforge.kernel.runtime_collision_fault.preempt_trajectory",
            "preemption_action": "preempt_trajectory",
            "latch_id": "interrupt-1",
            "source_id": "manyforge.kernel.runtime_collision_fault",
        }
        with manager._lock:
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "collision_detected",
                "execution_id": 1,
                "timestamp_ns": 1_000,
                "event_index": manager._kernel_event_index,
                "details": collision_details,
            })
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "interrupt_latched",
                "execution_id": 1,
                "timestamp_ns": 2_000,
                "event_index": manager._kernel_event_index,
                "details": latch_details,
            })

        code, details = manager._capture_kernel_error_diagnostics()
        assert code == "runtime_collision", (
            "Runtime collision monitor must surface as 'runtime_collision' "
            "(distinct from pre-execute 'validation_collision'); got: " + str(code)
        )
        assert details is not None
        assert details["collision_result_first_failure_pair"] == (
            "object/wall/shape/0 <-> robot/robot0/link/wrist_1_link/collision/0"
        ), "Colliding pair must be preserved when both events are buffered"
        assert details["collision_result_waypoint_index"] == "0"
        assert details["collision_result_first_failure_classification"] == (
            "blocking_collision"
        )

    def test_event_harvest_falls_back_to_latch_when_no_collision(self, kernel):
        """Drift-fault / operator-stop / other non-collision fault paths
        publish `interrupt_latched` without a preceding `collision_detected`.
        In that case the harvest must surface the latch details rather than
        returning (None, None)."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        with manager._lock:
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "interrupt_latched",
                "execution_id": 1,
                "timestamp_ns": 1_000,
                "event_index": manager._kernel_event_index,
                "details": {
                    "policy_id": "manyforge.kernel.drift_fault.preempt_trajectory",
                    "preemption_action": "preempt_trajectory",
                    "latch_id": "interrupt-7",
                },
            })

        code, details = manager._capture_kernel_error_diagnostics()
        assert code == "motion_interrupt_latched"
        assert details is not None
        assert details["policy_id"] == (
            "manyforge.kernel.drift_fault.preempt_trajectory"
        )

    def test_snapshot_error_promotes_ros2_backend_reason_code(self, kernel):
        """ROS2-control backend faults keep the top-level ErrorDTO code
        compatible with the core enum and carry the exact operator-facing
        cause in error_details.reason_code. The cycle manager should surface
        that exact token to the viewer instead of a generic execution_aborted
        banner."""

        class TrackingFaultKernel(MockKernelClient):
            def get_execution_snapshot(self):
                snapshot = super().get_execution_snapshot()
                snapshot.error = (
                    "Controller tracking error exceeded tolerance: 0.125 rad"
                )
                snapshot.error_code = "execution_aborted"
                snapshot.error_details = {
                    "backend": "ros2_control",
                    "reason_code": "tracking_error_exceeded",
                    "joint_name": "shoulder_pan_joint",
                    "peak_tracking_error_rad": "0.125",
                    "tracking_error_tolerance_rad": "0.05",
                }
                return snapshot

        fault_kernel = TrackingFaultKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, fault_kernel)
        manager = CycleManager(runner, fault_kernel)

        code, details = manager._capture_kernel_error_diagnostics()

        assert code == "tracking_error_exceeded"
        assert details is not None
        assert details["joint_name"] == "shoulder_pan_joint"
        assert details["peak_tracking_error_rad"] == "0.125"

    def test_reset_fails_loudly_when_scene_remove_fails(self, kernel):
        """Industrial reset semantics: if a runtime object cannot be cleared,
        reset must surface that failure rather than silently producing a
        half-clean scene. Otherwise a leftover wall (or whatever) silently
        shapes the next plan."""
        from manyforge_behavior.kernel_client import SceneEditRequest

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.reset()
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="runtime_obstacle",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        # Inject a failure on the next scene_edit call.
        kernel.fail_next_scene_edit = True

        import pytest

        with pytest.raises(RuntimeError, match="reset failed to (remove|detach)"):
            manager.reset()


class TestCycleManagerSignalRegistry:
    class _CountingProvider:
        adapter_id = "plc"

        def __init__(self) -> None:
            self.poll_count = 0

        def poll_signal_updates(self, now_ns: int):
            self.poll_count += 1
            return [
                SignalUpdate(
                    adapter="plc",
                    signal="cell.ready",
                    value=True,
                    observed_at_ns=100,
                    mirror_key="cell/ready",
                )
            ]

    def test_external_signal_mirrors_to_blackboard_and_clears_when_stale(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.publish_signal(
            adapter="plc",
            signal="cell.ready",
            value=True,
            stale_after_ms=1,
            mirror_key="cell/ready",
        )

        assert py_trees.blackboard.Blackboard.get("cell/ready") is True

        time.sleep(0.01)
        manager._refresh_signal_registry()

        with pytest.raises(KeyError):
            py_trees.blackboard.Blackboard.get("cell/ready")
        snapshot = manager.get_signal_snapshot()
        assert snapshot["signals"]["plc:cell.ready"]["freshness"] == "stale"

    def test_refresh_signal_registry_skips_sync_until_provider_or_signal_changes(
        self,
        kernel,
    ):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._signal_provider_poll_ticks = 3
        provider = self._CountingProvider()
        manager.register_signal_provider(provider)

        sync_calls = []
        original_sync = manager._sync_signal_blackboard

        def recording_sync(now_ns: int) -> None:
            sync_calls.append(now_ns)
            original_sync(now_ns)

        manager._sync_signal_blackboard = recording_sync

        manager._refresh_signal_registry()
        assert provider.poll_count == 1
        assert len(sync_calls) == 1
        assert py_trees.blackboard.Blackboard.get("cell/ready") is True

        manager._refresh_signal_registry()
        manager._refresh_signal_registry()
        assert provider.poll_count == 1
        assert len(sync_calls) == 1

        manager._refresh_signal_registry()
        assert provider.poll_count == 2
        assert len(sync_calls) == 1

    def test_force_current_evidence_publishes_bounded_boolean_signal(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        result = manager.publish_force_current_evidence(
            gripper_id="primary",
            value=0.42,
            min_value=0.3,
            units="A",
            source="motor_current",
            stale_after_ms=1000,
        )

        assert result["accepted"] is True
        assert result["confirmed"] is True
        assert result["mirror_key"] == "gripper/primary/force_current_confirmed"
        assert (
            py_trees.blackboard.Blackboard.get(
                "gripper/primary/force_current_confirmed"
            )
            is True
        )
        snapshot = manager.get_signal_snapshot()
        signal = snapshot["signals"]["gripper_evidence:gripper.force_current.primary"]
        assert signal["value"] is True
        assert signal["metadata"]["evidence_kind"] == "force_or_current"
        assert signal["metadata"]["value"] == 0.42
        assert signal["metadata"]["min_value"] == 0.3
        assert signal["metadata"]["units"] == "A"

        below = manager.publish_force_current_evidence(
            gripper_id="primary",
            value=0.12,
            min_value=0.3,
            stale_after_ms=1000,
        )

        assert below["confirmed"] is False
        assert (
            py_trees.blackboard.Blackboard.get(
                "gripper/primary/force_current_confirmed"
            )
            is False
        )

    def test_force_current_evidence_clears_when_stale(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.publish_force_current_evidence(
            gripper_id="primary",
            value=12.0,
            min_value=10.0,
            mirror_key="gripper/primary/grasp_force_ok",
            stale_after_ms=1,
        )
        assert py_trees.blackboard.Blackboard.get(
            "gripper/primary/grasp_force_ok"
        ) is True

        time.sleep(0.01)
        manager._refresh_signal_registry()

        with pytest.raises(KeyError):
            py_trees.blackboard.Blackboard.get(
                "gripper/primary/grasp_force_ok"
            )

    def test_force_current_evidence_rejects_unbounded_or_invalid_samples(
        self,
        kernel,
    ):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        with pytest.raises(ValueError, match="requires min_value or max_value"):
            manager.publish_force_current_evidence(
                gripper_id="primary",
                value=0.42,
            )

        with pytest.raises(ValueError, match="must be finite"):
            manager.publish_force_current_evidence(
                gripper_id="primary",
                value=float("nan"),
                min_value=0.1,
            )

        with pytest.raises(ValueError, match="min_value must be <= max_value"):
            manager.publish_force_current_evidence(
                gripper_id="primary",
                value=0.42,
                min_value=1.0,
                max_value=0.5,
            )



class TestCycleManagerVelocityScale:
    """Test velocity scale functionality."""
    
    def test_velocity_scale_clamped(self, kernel):
        """velocity_scale is clamped to [0.0, 1.0]."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        # Test clamping below 0
        manager.set_velocity_scale(-0.5)
        assert manager.velocity_scale == 0.0
        
        # Test clamping above 1
        manager.set_velocity_scale(2.0)
        assert manager.velocity_scale == 1.0
        
        # Test valid value
        manager.set_velocity_scale(0.5)
        assert manager.velocity_scale == 0.5

        # Test NaN rejected
        with pytest.raises(ValueError):
            manager.set_velocity_scale(float("nan"))
        assert manager.velocity_scale == 0.5  # unchanged

        # Test infinity rejected
        with pytest.raises(ValueError):
            manager.set_velocity_scale(float("inf"))
    
    def test_velocity_scale_forwarded_to_runtime_kernel(self, kernel):
        """velocity_scale is written to blackboard and forwarded as live control."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.set_velocity_scale(0.75, principal_id="operator_alice")
        
        # Check blackboard has the value
        bb = py_trees.blackboard.Client(name="test_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.READ)
        assert bb.velocity_scale == 0.75
        assert kernel.runtime_velocity_scale == pytest.approx(0.75)
        assert kernel.runtime_velocity_scale_updates[-1] == pytest.approx(0.75)
        assert kernel.runtime_velocity_scale_principal_ids[-1] == "operator_alice"
    
    def test_stop_flag_blocks_new_motion(self, kernel):
        """stop_requested flag is written to blackboard."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.1)
        
        # Check stop flag is False when running
        bb = py_trees.blackboard.Client(name="test_stop")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.READ)
        assert bb.stop_requested == False
        
        manager.full_stop()
        
        # Check stop flag is True when stopped
        assert bb.stop_requested == True


class TestCycleManagerProgramCompletion:
    """Test program completion through CycleManager."""
    
    def test_program_completes_through_cycle_manager(self, kernel):
        """A simple program completes when run through CycleManager."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        
        # Wait for program to complete
        time.sleep(1.0)
        
        # Program should have completed (tree ticked to SUCCESS)
        # The tick thread exits when tree completes
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()
        assert manager.state == CycleState.IDLE
        
        # Kernel should have been called
        assert kernel.execute_count >= 1

    def test_run_from_idle_after_completion_is_noop_until_reset(self, kernel):
        """A terminal idle tree requires Reset before another run can start."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(1.0)
        first_tree = manager._tree
        assert first_tree is not None
        assert manager.state == CycleState.IDLE
        execute_count_after_completion = kernel.execute_count

        manager.run()
        time.sleep(0.1)

        assert manager.state == CycleState.IDLE
        assert manager._tree is first_tree
        assert kernel.execute_count == execute_count_after_completion


# =========================================================================
# PRD-4 Qualification: Error propagation (Item 7)
# =========================================================================

class TestErrorPropagation:
    """Verify that tick-loop exceptions surface via manager.error and state."""

    def test_tick_exception_sets_error_and_parks_stopped(self, kernel):
        """A tick() exception must populate manager.error and park on the
        STOPPED + error surface (spec/410's uniform failure contract), never
        a bare IDLE that hides the fault."""
        slow_kernel = MockKernelClient(execute_duration_s=2.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)

        def exploding_tick():
            raise RuntimeError("sensor fault injected")

        manager._tree.tick = exploding_tick

        # Wait for the tick loop to encounter the exception
        deadline = time.time() + 2.0
        while manager.state != CycleState.STOPPED and time.time() < deadline:
            time.sleep(0.01)

        assert manager.state == CycleState.STOPPED
        assert manager.error is not None
        assert "sensor fault injected" in manager.error
        assert manager.error_code == "tick_loop_exception"

    def test_error_cleared_on_next_run(self, kernel):
        """run() resuming a stopped cycle clears a stale error field."""
        slow_kernel = MockKernelClient(execute_duration_s=2.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)
        manager.full_stop()
        deadline = time.time() + 2.0
        while manager.state != CycleState.STOPPED and time.time() < deadline:
            time.sleep(0.01)
        assert manager.state == CycleState.STOPPED

        with manager._lock:
            manager._error = "stale error from a previous incident"

        # A new run() must clear the error
        manager.run()
        assert manager.error is None


# =========================================================================
# PRD-4 Qualification: Zombie thread detection (Item 8)
# =========================================================================

class TestZombieThread:
    """Verify zombie tick-thread guards work."""

    def test_run_refused_while_zombie_thread_alive(self, kernel):
        """run() raises RuntimeError if the previous tick thread is still alive."""
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)

        # Replace tick with one that blocks for 5s, ignoring stop signals.
        # The tick loop calls tick() *before* checking the stop event on the
        # next iteration, so if tick() itself blocks, the thread is stuck.
        block_event = threading.Event()
        entered_tick = threading.Event()

        def blocking_tick():
            entered_tick.set()
            block_event.wait(timeout=5.0)

        manager._tree.tick = blocking_tick

        # Ensure the thread is inside the blocking tick before we reset
        entered_tick.wait(timeout=2.0)

        # Reset — this should detect the zombie because join(1.0) will timeout
        manager.reset()

        # The zombie guard should have set an error
        assert manager.error is not None
        assert "terminate" in manager.error

        # Attempting to run again should raise
        with pytest.raises(RuntimeError, match="still alive"):
            manager.run()

        # Clean up — unblock the thread so it can exit
        block_event.set()
        time.sleep(0.2)

    def test_reset_detects_zombie_and_sets_error(self, kernel):
        """reset() sets error when tick thread does not exit within timeout."""
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)

        # Make tick block so thread cannot exit within join timeout
        block_event = threading.Event()
        entered_tick = threading.Event()

        def blocking_tick():
            entered_tick.set()
            block_event.wait(timeout=5.0)

        manager._tree.tick = blocking_tick
        entered_tick.wait(timeout=2.0)

        manager.reset()

        assert manager.state == CycleState.IDLE
        assert manager.error is not None
        assert "terminate" in manager.error

        # Clean up
        block_event.set()
        time.sleep(0.2)


# =========================================================================
# PRD-4 Qualification: Concurrent rapid cycles (Item 4)
# =========================================================================

class TestConcurrentRapidCycles:
    """Verify stability under rapid run/stop/reset sequences."""

    def test_rapid_run_stop_reset_cycles(self, kernel):
        """20 rapid run/stop/reset cycles must not crash or deadlock."""
        slow_kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        for i in range(20):
            manager.run()
            assert manager.state == CycleState.RUNNING
            time.sleep(0.02)  # Let a few ticks execute

            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            manager.reset()
            assert manager.state == CycleState.IDLE
            assert manager.error is None

    def test_rapid_run_reset_without_stop(self, kernel):
        """Rapid run/reset cycles (skipping explicit stop) must be stable."""
        slow_kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        for i in range(15):
            manager.run()
            assert manager.state == CycleState.RUNNING
            time.sleep(0.02)

            manager.reset()
            assert manager.state == CycleState.IDLE

    def test_concurrent_status_reads_during_cycles(self, kernel):
        """get_node_statuses() called from another thread during rapid cycles must not crash."""
        slow_kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        errors = []
        stop_reader = threading.Event()

        def status_reader():
            while not stop_reader.is_set():
                try:
                    statuses = manager.get_node_statuses()
                    # Must always be a dict (possibly empty)
                    if not isinstance(statuses, dict):
                        errors.append(f"Expected dict, got {type(statuses)}")
                except Exception as e:
                    errors.append(str(e))
                time.sleep(0.005)

        reader_thread = threading.Thread(target=status_reader, daemon=True)
        reader_thread.start()

        try:
            for _ in range(10):
                manager.run()
                time.sleep(0.03)
                manager.full_stop()
                manager.reset()
        finally:
            stop_reader.set()
            reader_thread.join(timeout=2.0)

        assert errors == [], f"Status reader errors: {errors}"


# =========================================================================
# PRD-4 Qualification: Observer lag independence (Item 5)
# =========================================================================

class TestObserverLagIndependence:
    """Verify that slow observers do not affect tick-loop timing."""

    def test_slow_status_reader_does_not_block_ticks(self, kernel):
        """A slow get_node_statuses() consumer must not delay tick execution."""
        slow_kernel = MockKernelClient(execute_duration_s=2.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)

        # Simulate a slow observer: hold the lock while reading
        tick_counts_before = slow_kernel.execute_count

        stop_reader = threading.Event()

        def slow_reader():
            while not stop_reader.is_set():
                statuses = manager.get_node_statuses()
                # Simulate slow processing (200ms per read)
                time.sleep(0.2)

        reader_thread = threading.Thread(target=slow_reader, daemon=True)
        reader_thread.start()

        # Let system run for 0.5s with the slow reader
        time.sleep(0.5)

        stop_reader.set()
        reader_thread.join(timeout=2.0)

        # The tick loop should still be running at ~100Hz.
        # In 0.5s, we expect ~50 ticks. The tree drives kernel execute calls.
        # Key check: the manager is still running and hasn't deadlocked.
        assert manager.state == CycleState.RUNNING

        manager.full_stop()
        manager.reset()


class TestIntervention:
    """Tests for direct-control intervention mode."""

    def _make_manager(self, kernel, execute_duration_s=0.0):
        if execute_duration_s > 0:
            kernel = MockKernelClient(execute_duration_s=execute_duration_s)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        # Load program so _build_node works
        runner.load()
        return manager

    def _simple_tree_dict(self):
        """A trivial timer node that completes quickly."""
        return {
            "id": "timer",
            "name": "test_timer",
            "params": {"duration_s": 0.05},
        }

    def _idle_manual_metadata(self):
        return {
            "session_kind": "manual_intervention",
            "authority_source": "human",
            "principal_id": "human:test-operator",
            "approval_mode": "human_approved",
        }

    def _stopped_manual_metadata(self, *, authority_source="llm"):
        return {
            "session_kind": "manual_intervention",
            "authority_source": authority_source,
            "principal_id": "agent:test-llm" if authority_source != "human" else "human:test-operator",
        }

    def test_execute_from_idle(self, kernel):
        """Intervention starts from IDLE and transitions to DIRECT_CONTROL."""
        manager = self._make_manager(kernel)
        session = manager.execute_intervention(
            self._simple_tree_dict(),
            **self._idle_manual_metadata(),
        )

        assert isinstance(session, InterventionSession)
        assert session.status == "running"
        assert session.session_kind == "manual_intervention"
        assert session.authority_source == "human"
        assert manager.state == CycleState.DIRECT_CONTROL

        # Wait for completion
        time.sleep(0.3)
        assert manager.state == CycleState.IDLE
        assert session.status == "completed"
        assert session.finished_at_ns is not None

    def test_execute_from_idle_requires_human_approval(self, kernel):
        """IDLE direct control requires explicit human-approved manual metadata."""
        manager = self._make_manager(kernel)

        with pytest.raises(RuntimeError, match="human-approved manual intervention"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="llm",
                principal_id="agent:test-llm",
                grant_type="policy_granted",
            )

    def test_execute_requires_principal_id(self, kernel):
        """principal_id is mandatory for all interventions."""
        manager = self._make_manager(kernel)

        with pytest.raises(RuntimeError, match="principal_id is required"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="human",
                approval_mode="human_approved",
                # principal_id deliberately omitted
            )

    def test_execute_rejects_empty_principal_id(self, kernel):
        """Whitespace-only principal_id is rejected."""
        manager = self._make_manager(kernel)

        with pytest.raises(RuntimeError, match="principal_id is required"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="human",
                principal_id="   ",
                approval_mode="human_approved",
            )

    def test_grant_type_stored_on_session(self, kernel):
        """principal_id and grant_type are stored on the InterventionSession."""
        manager = self._make_manager(kernel)
        session = manager.execute_intervention(
            self._simple_tree_dict(),
            **self._idle_manual_metadata(),
        )
        assert session.principal_id == "human:test-operator"
        assert session.grant_type == "human_approved"

    def test_intervention_session_events_are_bounded(self, kernel):
        session = InterventionSession(
            session_id="sess-bounded",
            parent_run_id=None,
            entry_state="idle",
            lease_timeout_ms=10_000,
            lease_expires_at_ns=time.monotonic_ns() + 10_000_000_000,
            resume_target="stay_idle",
        )

        for index in range(cycle_manager_module._INTERVENTION_SESSION_EVENT_LIMIT + 17):
            session.record_event("session_progress", value=index)

        assert len(session.events) == cycle_manager_module._INTERVENTION_SESSION_EVENT_LIMIT
        assert session.events[0]["event_index"] == 18
        assert session.events[-1]["event_index"] == (
            cycle_manager_module._INTERVENTION_SESSION_EVENT_LIMIT + 17
        )

    def test_paused_intervention_tick_loop_uses_bounded_event_wait(
        self,
        kernel,
        monkeypatch,
    ):
        manager = self._make_manager(kernel)
        session = InterventionSession(
            session_id="sess-paused",
            parent_run_id=None,
            entry_state="idle",
            lease_timeout_ms=10_000,
            lease_expires_at_ns=time.monotonic_ns() + 10_000_000_000,
            resume_target="stay_idle",
        )
        manager._tick_pause_event.clear()
        manager._tick_stop_event.clear()
        wait_timeouts = []

        def recording_wait(timeout=None):
            wait_timeouts.append(timeout)
            manager._tick_stop_event.set()
            return False

        monkeypatch.setattr(manager._tick_pause_event, "wait", recording_wait)
        monkeypatch.setattr(manager, "_finish_intervention", lambda *args, **kwargs: None)
        monkeypatch.setattr(
            manager,
            "_restore_after_intervention",
            lambda *args, **kwargs: None,
        )

        manager._intervention_tick_loop(session)

        assert wait_timeouts == [cycle_manager_module._PAUSED_TICK_WAIT_S]

    def test_grant_type_human_approved_requires_human_authority(self, kernel):
        """grant_type=human_approved is invalid with non-human authority_source."""
        manager = self._make_manager(kernel)
        manager._state = CycleState.STOPPED

        with pytest.raises(RuntimeError, match="grant_type=human_approved requires authority_source=human"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="llm",
                principal_id="agent:test-llm",
                grant_type="human_approved",
            )

    def test_grant_type_policy_granted_rejects_human_authority(self, kernel):
        """grant_type=policy_granted is invalid with human authority_source."""
        manager = self._make_manager(kernel)

        with pytest.raises(RuntimeError, match="grant_type=policy_granted is not valid for authority_source=human"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="human",
                principal_id="human:test-operator",
                grant_type="policy_granted",
            )

    def test_grant_type_deployment_granted_rejected(self, kernel):
        """deployment_granted is reserved and not in ACTIVE set."""
        manager = self._make_manager(kernel)

        with pytest.raises(RuntimeError, match="grant_type must be one of"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="human",
                principal_id="human:test-operator",
                grant_type="deployment_granted",
            )

    def test_approval_mode_must_match_grant_type(self, kernel):
        """approval_mode that contradicts grant_type is rejected."""
        manager = self._make_manager(kernel)
        manager._state = CycleState.STOPPED

        with pytest.raises(RuntimeError, match="contradicts grant_type"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                session_kind="manual_intervention",
                authority_source="llm",
                principal_id="agent:test-llm",
                grant_type="policy_granted",
                approval_mode="not_required",
            )

    def test_execute_from_stopped(self, kernel):
        """Intervention can start from STOPPED state."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        # The tick thread from the main cycle may still be alive (paused).
        # Reset cleans it up properly.
        manager.reset()
        # Now go to stopped by running and stopping again, or just use IDLE.
        # Actually, execute_intervention accepts IDLE too. Test IDLE path
        # for "from stopped" by manually putting state to STOPPED after reset.
        manager._state = CycleState.STOPPED

        session = manager.execute_intervention(
            self._simple_tree_dict(),
            **self._stopped_manual_metadata(),
        )
        assert manager.state == CycleState.DIRECT_CONTROL
        time.sleep(0.3)
        assert session.status in ("completed", "failed")
        assert manager.state == CycleState.STOPPED

    def test_completed_intervention_rebaselines_resumable_stopped_scene(self, kernel):
        """A successful direct-control recovery may intentionally change the scene.

        The STOPPED resume guard must keep blocking unapproved scene edits, but
        once an explicit intervention completes and returns to STOPPED, the
        post-intervention scene is the new operator-approved resume baseline.
        """

        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(slow_kernel)

        try:
            manager.run()
            time.sleep(0.05)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            session = manager.execute_intervention(
                {
                    "id": "timer",
                    "name": "scene_recovery_timer",
                    "params": {"duration_s": 0.15},
                },
                resume_target="stop",
                **self._stopped_manual_metadata(authority_source="human"),
            )
            assert manager.state == CycleState.DIRECT_CONTROL

            result = slow_kernel.scene_edit(
                SceneEditRequest(
                    operation="update_pose",
                    object_id="table",
                    pose_position_m=[0.35, 0.1, 0.2],
                    pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                    frame_id="base_link",
                )
            )
            assert result.success is True

            for _ in range(50):
                if session.status != "running" and manager.state == CycleState.STOPPED:
                    break
                time.sleep(0.02)

            assert session.status == "completed"
            assert manager.state == CycleState.STOPPED
            assert any(
                event["type"] == "resume_scene_baselined"
                for event in session.events
            )

            manager.run()
            assert manager.state == CycleState.RUNNING
        finally:
            manager.reset()

    def test_execute_from_running_raises(self, kernel):
        """Intervention raises RuntimeError when cycle is RUNNING."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        assert manager.state == CycleState.RUNNING

        with pytest.raises(RuntimeError, match="Cannot start intervention"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                **self._idle_manual_metadata(),
            )

        manager.full_stop()
        manager.reset()

    def test_execute_from_stopped_rejects_paused_kernel(self, kernel):
        """Wrapper-only intervention does not hijack a paused kernel motion.

        Operator stop() now preempts via the controlled stop (kernel STOPPED,
        no held motion), so it no longer produces a paused kernel. The guard
        still protects any STOPPED recovery state where the backend is
        genuinely PAUSED; recreate that condition directly and assert the
        guard still rejects the wrapper-only intervention."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        manager.full_stop()
        # Recreate a STOPPED recovery state with a paused backend. _make_manager
        # builds its own kernel, so override the manager's actual kernel.
        manager._kernel._state = ExecutionState.PAUSED

        with pytest.raises(RuntimeError, match="paused motion"):
            manager.execute_intervention(
                self._simple_tree_dict(),
                **self._stopped_manual_metadata(),
            )

    def test_abort_running_intervention(self, kernel):
        """abort_intervention() stops a running intervention."""
        manager = self._make_manager(kernel)

        # Use a long-running timer that won't complete before abort
        tree_dict = {
            "id": "timer",
            "name": "long_timer",
            "params": {"duration_s": 10.0},
        }

        session = manager.execute_intervention(
            tree_dict,
            **self._idle_manual_metadata(),
        )
        time.sleep(0.1)
        assert manager.state == CycleState.DIRECT_CONTROL

        manager.abort_intervention()
        assert manager.state == CycleState.IDLE
        assert session.status == "aborted"

    def test_abort_restores_stopped_entry_state(self, kernel):
        """Aborting an intervention started from STOPPED returns to STOPPED."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        manager.full_stop()
        manager.reset()
        manager._state = CycleState.STOPPED

        session = manager.execute_intervention(
            {
                "id": "timer",
                "name": "long_timer",
                "params": {"duration_s": 10.0},
            },
            **self._stopped_manual_metadata(),
        )
        time.sleep(0.1)
        manager.abort_intervention()

        assert session.status == "aborted"
        assert manager.state == CycleState.STOPPED

    def test_lease_expiry_from_idle_forces_stopped(self, kernel):
        """Lease expiry transitions to STOPPED even when entry state was IDLE."""
        manager = self._make_manager(kernel)
        session = manager.execute_intervention(
            {
                "id": "timer",
                "name": "long_timer",
                "params": {"duration_s": 10.0},
            },
            lease_timeout_ms=50,
            **self._idle_manual_metadata(),
        )

        time.sleep(0.2)

        assert session.status == "expired"
        assert session.actual_outcome == "stopped"
        assert manager.state == CycleState.STOPPED

    def test_abort_noop_when_not_direct_control(self, kernel):
        """abort_intervention() is a no-op when not in DIRECT_CONTROL."""
        manager = self._make_manager(kernel)
        assert manager.state == CycleState.IDLE
        manager.abort_intervention()  # should not raise
        assert manager.state == CycleState.IDLE

    def test_intervention_property_none_initially(self, kernel):
        """intervention property is None before any intervention runs."""
        manager = self._make_manager(kernel)
        assert manager.intervention is None

    def test_failed_tree_build_raises(self, kernel):
        """Invalid tree dict raises RuntimeError and sets session to failed."""
        manager = self._make_manager(kernel)
        bad_tree = {"id": "nonexistent_node_type_xyz", "name": "bad"}

        with pytest.raises(RuntimeError, match="Failed to build intervention tree"):
            manager.execute_intervention(
                bad_tree,
                **self._idle_manual_metadata(),
            )

        session = manager.intervention
        assert session is not None
        assert session.status == "failed"
        assert manager.state == CycleState.IDLE

    def test_execute_intervention_loads_runner_when_needed(self, kernel):
        """Intervention can bootstrap the runner if it was not preloaded."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        session = manager.execute_intervention(
            self._simple_tree_dict(),
            **self._idle_manual_metadata(),
        )

        assert session.status == "running"
        time.sleep(0.2)
        assert manager.intervention is not None

    def test_intervention_rewrite_uses_catalog_blackboard_metadata(self, kernel):
        """Reader/writer scoping follows catalog metadata rather than hardcoded node IDs."""
        manager = self._make_manager(kernel)
        catalog = manager._runner.catalog
        assert catalog is not None

        tree_dict = {
            "id": "sequence",
            "name": "scope_test",
            "children": [
                {
                    "id": "set_key_bool_value",
                    "name": "write_flag",
                    "params": {"key": {"key_ref": "shared.flag"}, "value": True},
                },
                {
                    "id": "check_key_bool_value",
                    "name": "read_flag",
                    "params": {"key": {"key_ref": "shared.flag"}, "expected_value": True},
                },
            ],
        }

        reader_entry = catalog.get("check_key_bool_value")
        original_reader_params = list(reader_entry.bb_reader_params)
        reader_entry.bb_reader_params = []
        try:
            rewritten = manager._rewrite_intervention_tree(tree_dict, "sess123")
        finally:
            reader_entry.bb_reader_params = original_reader_params

        writer_params = rewritten["children"][0]["params"]
        reader_params = rewritten["children"][1]["params"]
        assert writer_params["key"] == "_intervention/sess123/shared/flag"
        assert reader_params["key"] == {"key_ref": "shared.flag"}


class TestKernelEventBridge:
    """Tests for native kernel event bridging through CycleManager."""

    def test_mock_kernel_emits_events_on_execute(self):
        """MockKernelClient emits kernel events on execute()."""
        kernel = MockKernelClient()
        events = kernel.drain_kernel_events()
        assert events == []

        from manyforge_behavior.kernel_client import ExecuteRequest
        req = ExecuteRequest(
            robot_id="r", group_id="g", move_id="m1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal",
        )
        kernel.execute(req)
        events = kernel.drain_kernel_events()
        assert len(events) > 0
        types = [e["type"] for e in events]
        assert "plan_started" in types
        assert "state_transition" in types

    def test_mock_kernel_emits_events_on_stop(self):
        """MockKernelClient emits stop events."""
        kernel = MockKernelClient(execute_duration_s=5.0)
        from manyforge_behavior.kernel_client import ExecuteRequest
        req = ExecuteRequest(
            robot_id="r", group_id="g", move_id="m1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal",
        )
        kernel.execute(req)
        kernel.drain_kernel_events()  # clear

        kernel.stop()
        events = kernel.drain_kernel_events()
        types = [e["type"] for e in events]
        assert "stop_requested" in types
        assert "state_transition" in types

    def test_drain_clears_buffer(self):
        """drain_kernel_events() returns events and clears the buffer."""
        kernel = MockKernelClient()
        from manyforge_behavior.kernel_client import ExecuteRequest
        req = ExecuteRequest(
            robot_id="r", group_id="g", move_id="m1",
            base_frame="base_link", tcp_frame="tool0",
            motion_type="joint_goal",
        )
        kernel.execute(req)
        first = kernel.drain_kernel_events()
        assert len(first) > 0
        second = kernel.drain_kernel_events()
        assert len(second) == 0

    def test_cycle_manager_collects_kernel_events(self, kernel):
        """CycleManager collects kernel events during execution."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        # No events initially
        events = manager.get_kernel_events_since(0)
        assert events == []

        # Run a program cycle
        manager.run()
        time.sleep(0.3)  # let tree tick and complete

        events = manager.get_kernel_events_since(0)
        assert len(events) > 0
        # All events should have event_index
        for event in events:
            assert "event_index" in event
            assert event["event_index"] > 0

        # Polling with last index returns no new events
        last_index = events[-1]["event_index"]
        new_events = manager.get_kernel_events_since(last_index)
        assert len(new_events) == 0

        manager.reset()

    def test_get_kernel_events_since_matches_full_scan_after_ring_wrap(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        with manager._lock:
            for index in range(1, 5001):
                manager._kernel_events.append(
                    {"event_index": index, "type": "fixture", "payload": index}
                )
            buffered = list(manager._kernel_events)

        for event_index in (
            0,
            buffered[0]["event_index"] - 1,
            buffered[0]["event_index"],
            buffered[len(buffered) // 2]["event_index"],
            buffered[-1]["event_index"] - 1,
            buffered[-1]["event_index"],
        ):
            expected = [
                dict(event)
                for event in buffered
                if event["event_index"] > event_index
            ]
            assert manager.get_kernel_events_since(event_index) == expected

    def test_cycle_manager_drains_direct_kernel_events_on_demand(self, kernel):
        """Direct kernel calls can be surfaced without waiting for a tree tick."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        from manyforge_behavior.kernel_client import ExecuteRequest
        req = ExecuteRequest(
            robot_id="r",
            group_id="g",
            move_id="direct",
            base_frame="base_link",
            tcp_frame="tool0",
            motion_type="joint_goal",
        )
        kernel.execute(req)

        assert manager.get_kernel_events_since(0) == []
        drained = manager.drain_kernel_events_now()
        assert len(drained) > 0
        events = manager.get_kernel_events_since(0)
        assert len(events) == len(drained)
        assert all("event_index" in event for event in events)
        assert manager.drain_kernel_events_now() == []


# --- Recovery trigger taxonomy ---


class TestRecoveryTriggerTaxonomy:
    """spec/460 §6.2.2 canonical taxonomy; legacy aliases for migration."""

    CANONICAL = {
        "planning_failed",
        "validation_failed",
        "execute_failed",
        "execute_timeout",
        "scene_mismatch",
        "precondition_failed",
        "postcondition_failed",
    }

    def test_all_canonical_values_are_loadable(self):
        for value in self.CANONICAL:
            assert RecoveryTrigger(value).value == value

    def test_canonical_set_is_subset_of_enum(self):
        present = {member.value for member in RecoveryTrigger}
        assert self.CANONICAL.issubset(present)

    def test_live_authoring_allowlist_excludes_legacy_aliases(self):
        assert canonical_recovery_trigger_values() == self.CANONICAL
        assert "timeout" not in canonical_recovery_trigger_values()
        assert "node_failure" not in canonical_recovery_trigger_values()

    def test_legacy_aliases_are_removed(self):
        # Persisted traces carry trigger strings outward only and are never
        # parsed back into the enum, so the legacy aliases are gone.
        with pytest.raises(ValueError):
            RecoveryTrigger("timeout")
        with pytest.raises(ValueError):
            RecoveryTrigger("node_failure")
        assert {member.value for member in RecoveryTrigger} == self.CANONICAL

    def test_unknown_string_is_rejected(self):
        with pytest.raises(ValueError):
            RecoveryTrigger("made_up_reason")


# --- ros2_control backend fault taxonomy (audit Phase 6A) ---


_WORKSPACE_ROOT = Path(__file__).parent.parent.parent
_ROS2_CONTROL_FAULT_REASON_HEADERS = (
    _WORKSPACE_ROOT
    / "manyforge_ros2_control_execution"
    / "include"
    / "manyforge_ros2_control_execution"
    / "ros2_control_capability.hpp",
    _WORKSPACE_ROOT
    / "manyforge_roboplan"
    / "include"
    / "manyforge_roboplan"
    / "ros2_control_capability.hpp",
)


class TestRos2ControlBackendFaultTaxonomy:
    """C++/Python parity + external-publication constraints for the 6A
    backend fault taxonomy.

    The C++ header (namespace `ros2_control_fault_reason`) is the emitting
    source of truth; `ROS2_CONTROL_BACKEND_FAULT_CLASSES` is the consuming
    mirror that digest promotion and the Phase 6H/6I external diagnostics
    surfaces read. Drift in either direction must fail loudly here, not
    surface as a silently unpromoted fault in production.
    """

    @staticmethod
    def _header_reason_tokens() -> set[str]:
        import re

        header = next(
            (
                candidate
                for candidate in _ROS2_CONTROL_FAULT_REASON_HEADERS
                if candidate.is_file()
            ),
            None,
        )
        assert header is not None, (
            "ros2_control_capability.hpp not found at any expected location: "
            f"{', '.join(str(path) for path in _ROS2_CONTROL_FAULT_REASON_HEADERS)}; "
            "the taxonomy parity test requires the monorepo layout with the "
            "ros2_control execution package next to manyforge_behavior."
        )
        text = header.read_text(encoding="utf-8")
        match = re.search(
            r"namespace ros2_control_fault_reason \{(.*?)\}\s*//\s*namespace"
            r"\s+ros2_control_fault_reason",
            text,
            flags=re.DOTALL,
        )
        assert match, (
            "namespace ros2_control_fault_reason not found in "
            f"{header} — the 6A taxonomy moved; update "
            "this parity test alongside it."
        )
        tokens = set(re.findall(r'"([a-z0-9_]+)"', match.group(1)))
        assert tokens, "no reason tokens parsed from the C++ namespace"
        return tokens

    def test_python_taxonomy_matches_cpp_header(self):
        cpp_tokens = self._header_reason_tokens()
        py_tokens = set(ROS2_CONTROL_BACKEND_FAULT_CLASSES)
        assert py_tokens == cpp_tokens, (
            f"taxonomy drift: only-in-cpp={sorted(cpp_tokens - py_tokens)}, "
            f"only-in-python={sorted(py_tokens - cpp_tokens)}"
        )

    def test_external_aliases_satisfy_fault_code_constraints(self):
        # medkit/diagnostics fault-code convention: UPPER_SNAKE_CASE,
        # [A-Z0-9_], <=64 chars, globally unique, namespaced to manyforge.
        import re

        aliases = [
            fault.external_alias
            for fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.values()
        ]
        assert len(aliases) == len(set(aliases)), "aliases must be unique"
        for fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.values():
            alias = fault.external_alias
            assert re.fullmatch(r"[A-Z][A-Z0-9_]*", alias), alias
            assert len(alias) <= 64, f"{alias} exceeds 64 chars"
            assert alias == f"MANYFORGE_{fault.reason_code.upper()}", (
                f"{alias} must be the MANYFORGE_-prefixed uppercase of "
                f"{fault.reason_code}"
            )

    def test_severities_use_operational_levels_with_documented_rule(self):
        # spec/520 levels; CRITICAL is reserved for unconfirmed-stop /
        # stuck-lifecycle classes per the taxonomy docstring.
        allowed = {"INFO", "WARN", "ERROR", "CRITICAL"}
        for fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.values():
            assert fault.severity in allowed, fault
        critical = {
            code
            for code, fault in ROS2_CONTROL_BACKEND_FAULT_CLASSES.items()
            if fault.severity == "CRITICAL"
        }
        assert critical == {
            "controller_cancel_timeout",
            "monitor_thread_still_shutting_down",
        }


# --- Cycle-state machine + explicit intervention recovery ---


class TestCycleStateMachine:
    """spec/400 §6.6.2 — cycle state machine."""

    def test_all_documented_states_exist(self):
        names = {state.name for state in CycleState}
        # The baseline operator states plus the stopping and intervention states.
        assert names >= {
            "IDLE",
            "RUNNING",
            "STOPPING",
            "STOPPED",
            "DIRECT_CONTROL",
        }


class TestCycleManagerExplicitInterventionRecovery:
    """Run on STOPPED + kernel INTERVENTION_REQUIRED (spec/400 §6.6.2)."""

    def _make_manager(self) -> tuple[CycleManager, MockKernelClient]:
        kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        return manager, kernel

    def test_run_from_stopped_with_kernel_intervention_requires_explicit_recovery(
        self,
    ):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        # Drive the kernel into INTERVENTION_REQUIRED via direct mutation
        # of the in-memory mock. Production kernels reach this via backend
        # snapshots or reset failure; Run must not silently acknowledge it.
        kernel._state = ExecutionState.INTERVENTION_REQUIRED

        with pytest.raises(RuntimeError, match="Acknowledge and continue"):
            manager.run()
        assert manager.state == CycleState.STOPPED
        assert kernel.get_status().state == ExecutionState.INTERVENTION_REQUIRED
        assert kernel.acknowledge_intervention_count == 0
        assert kernel.acknowledge_intervention_for_retry_count == 0

    def test_acknowledge_intervention_for_retry_preserves_backend_reset_count(
        self,
    ):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        kernel._state = ExecutionState.INTERVENTION_REQUIRED
        manager.acknowledge_intervention_for_retry(
            principal_id="operator:test",
            reason="test_acknowledge_and_continue",
        )

        assert manager.state == CycleState.STOPPED
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.acknowledge_intervention_for_retry_count == 1
        assert kernel.acknowledge_intervention_count == 0
        assert kernel.reset_count == 0

    def test_acknowledge_intervention_for_retry_accepts_held_motion_intervention(
        self,
    ):
        manager, kernel = self._make_manager()
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "Runtime collision detected during execution"
            manager._error_code = "runtime_collision"
            manager._error_details = {
                "motion_soft_stop_reason": "motion_soft_fail_requires_intervention",
                "kernel_state": "intervention_required",
            }
        kernel._state = ExecutionState.INTERVENTION_REQUIRED
        before_reset_count = kernel.reset_count

        manager.acknowledge_intervention_for_retry(
            principal_id="operator:test",
            reason="test_acknowledge_held_motion_intervention",
        )

        assert manager.state == CycleState.STOPPED
        assert manager.motion_stop_state == "idle"
        assert not manager._tick_pause_event.is_set()
        assert manager.error is None
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.acknowledge_intervention_for_retry_count == 1
        assert kernel.reset_count == before_reset_count

    def test_acknowledge_failed_validation_for_retry_preserves_backend_reset_count(
        self,
    ):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - build a resumable tree.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._error = "Observed scene source stale"
            manager._error_code = "scene_changed_during_validation"
            manager._error_details = {
                "reason": "observed_scene_source_stale",
                "object_id": "isaac_cheese_box",
                "source_id": "isaac_scene_objects",
            }
        kernel._state = ExecutionState.FAILED
        kernel._last_error_message = "Observed scene source stale"  # noqa: SLF001
        kernel._last_error_code = "scene_changed_during_validation"  # noqa: SLF001
        kernel._last_error_details = {  # noqa: SLF001
            "reason": "observed_scene_source_stale",
            "object_id": "isaac_cheese_box",
            "source_id": "isaac_scene_objects",
        }
        before_reset_count = kernel.reset_count

        manager.acknowledge_failed_validation_for_retry(
            principal_id="operator:test",
            reason="test_acknowledge_failed_validation",
        )

        assert manager.state == CycleState.STOPPED
        assert manager.motion_stop_state == "idle"
        assert manager.error is None
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.acknowledge_failed_validation_for_retry_count == 1
        assert kernel.reset_count == before_reset_count

    def test_acknowledge_failed_validation_for_retry_rejects_non_validation_failed(
        self,
    ):
        manager, kernel = self._make_manager()
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._error = "Backend failed"
            manager._error_code = "execution_aborted"
            manager._error_details = {"reason": "controller_result_failed"}
        kernel._state = ExecutionState.FAILED
        kernel._last_error_message = "Backend failed"  # noqa: SLF001
        kernel._last_error_code = "execution_aborted"  # noqa: SLF001
        kernel._last_error_details = {  # noqa: SLF001
            "reason": "controller_result_failed",
        }

        with pytest.raises(RuntimeError, match="not a retryable validation gate"):
            manager.acknowledge_failed_validation_for_retry()

        assert kernel.get_status().state == ExecutionState.FAILED
        assert kernel.acknowledge_failed_validation_for_retry_count == 0
        assert kernel.reset_count == 0

    def test_acknowledged_failed_validation_run_bypasses_scene_guard_for_replan(
        self,
        monkeypatch,
    ):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - build a resumable tree.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._resumable_stop_scene_fingerprint = "stale-scene"
            manager._error = "Observed scene source stale"
            manager._error_code = "scene_changed_during_validation"
            manager._error_details = {"reason": "observed_scene_source_stale"}
        kernel._state = ExecutionState.FAILED
        kernel._last_error_message = "Observed scene source stale"  # noqa: SLF001
        kernel._last_error_code = "scene_changed_during_validation"  # noqa: SLF001
        kernel._last_error_details = {"reason": "observed_scene_source_stale"}  # noqa: SLF001

        manager.acknowledge_failed_validation_for_retry(
            principal_id="operator:test",
            reason="test_acknowledge_failed_validation",
        )

        with manager._lock:  # noqa: SLF001 - prove the ordinary guard would fail.
            assert manager._state == CycleState.STOPPED
            assert manager._allow_next_stopped_retry_replan is True
        monkeypatch.setattr(
            manager,
            "_capture_scene_resume_fingerprint",
            lambda: ("changed-live-scene", None),
        )
        monkeypatch.setattr(manager, "_start_tick_thread", lambda _target: None)

        manager.run()

        bb = py_trees.blackboard.Client(name="test_failed_validation_retry_replan")
        bb.register_key(
            key="resume_after_error_requested",
            access=py_trees.common.Access.READ,
        )
        assert manager.state == CycleState.RUNNING
        assert manager.error is None
        assert bb.get("resume_after_error_requested") is True
        with manager._lock:  # noqa: SLF001 - bypass is one-shot.
            assert manager._allow_next_stopped_retry_replan is False

    def test_acknowledged_held_intervention_run_bypasses_scene_guard_for_replan(
        self,
        monkeypatch,
    ):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - build a resumable tree.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "Runtime collision detected during execution"
            manager._error_code = "runtime_collision"
            manager._error_details = {
                "motion_soft_stop_reason": "motion_soft_fail_requires_intervention",
                "kernel_state": "intervention_required",
            }
        kernel._state = ExecutionState.INTERVENTION_REQUIRED

        manager.acknowledge_intervention_for_retry(
            principal_id="operator:test",
            reason="test_acknowledge_held_motion_intervention",
        )

        with manager._lock:  # noqa: SLF001 - prove the ordinary guard would fail.
            assert manager._state == CycleState.STOPPED
            assert manager._resumable_stop_scene_fingerprint is None
            assert manager._allow_next_stopped_retry_replan is True
        monkeypatch.setattr(
            manager,
            "_capture_scene_resume_fingerprint",
            lambda: ("changed-live-scene", None),
        )
        monkeypatch.setattr(manager, "_start_tick_thread", lambda _target: None)

        manager.run()

        bb = py_trees.blackboard.Client(name="test_retry_replan_resume_flag")
        bb.register_key(
            key="resume_after_error_requested",
            access=py_trees.common.Access.READ,
        )
        assert manager.state == CycleState.RUNNING
        assert manager.error is None
        assert bb.get("resume_after_error_requested") is True
        with manager._lock:  # noqa: SLF001 - bypass is one-shot.
            assert manager._allow_next_stopped_retry_replan is False

    def test_run_from_stopped_with_kernel_intervention_fails_closed_if_intervention_session_running(
        self,
    ):
        # An intervention session that the cycle manager still considers
        # running is a direct-control lease per spec/400 §6.6.2 — Run MUST
        # refuse until the operator ends/promotes/aborts that session.
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        kernel._state = ExecutionState.INTERVENTION_REQUIRED
        manager._intervention = InterventionSession(
            session_id="sess-1",
            parent_run_id=None,
            entry_state="STOPPED",
            lease_timeout_ms=60_000,
            lease_expires_at_ns=10**18,
            resume_target="resume",
            principal_id="human:test",
        )
        manager._intervention.status = "running"

        with pytest.raises(RuntimeError, match="intervention_session_running"):
            manager.run()
        assert manager.state == CycleState.STOPPED

    def test_run_from_stopped_refuses_auto_ack_when_stop_did_not_settle(self):
        # A measured-settle intervention means the robot/controller did not
        # prove rest after a confirmed stop. Run must not acknowledge that into a
        # backend reset because commanded-sim and some real backends reset by
        # commanding an initial/home state.
        from types import SimpleNamespace

        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        kernel._state = ExecutionState.INTERVENTION_REQUIRED
        kernel.get_execution_snapshot = lambda: SimpleNamespace(
            error_code="motion_interrupt_backend_stop_failed",
            error_details={"reason": "stop_not_settled_measured_velocity"},
        )

        with pytest.raises(
            RuntimeError,
            match="stop_not_settled_measured_velocity",
        ):
            manager.run()
        assert manager.state == CycleState.STOPPED
        assert kernel.get_status().state == ExecutionState.INTERVENTION_REQUIRED

    def test_run_from_stopped_normal_path_unchanged(self):
        # The explicit recovery path only applies when the kernel is in
        # INTERVENTION_REQUIRED. Normal STOPPED resume must not call
        # acknowledge_intervention, so no new event is emitted.
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        events_before = list(manager.drain_kernel_events_now())
        manager.run()
        assert manager.state == CycleState.RUNNING
        events_after = list(manager.drain_kernel_events_now())
        ack_events = [
            e for e in events_before + events_after
            if e.get("type") == "acknowledge_intervention"
        ]
        assert ack_events == []

    def test_helper_refuses_when_kernel_status_unreadable(self):
        # If the kernel's get_status() raises, the helper MUST fail
        # closed with a structured reason — never silently swallow.
        manager, kernel = self._make_manager()

        def boom():
            raise RuntimeError("kernel went away")

        kernel.get_status = boom  # type: ignore[assignment]
        ok, reason = manager._can_auto_acknowledge_leftover_state()
        assert ok is False
        assert reason.startswith("kernel_status_unreadable:")
        assert "RuntimeError" in reason


class TestGripperFaultDiagnostics:
    """The digest must carry the gripper reason token when a tree fails on a
    gripper fault: the gripper runtime has no kernel-snapshot error surface,
    so the cycle manager reads the structured record the command_gripper
    node mirrors onto the blackboard."""

    def _write_fault(self, details):
        bb = py_trees.blackboard.Client(name="gripper_fault_writer")
        bb.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.WRITE
        )
        bb.set("gripper_fault_details", details)

    def test_classified_gripper_fault_is_promoted(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        self._write_fault(
            {
                "reason_code": "gripper_request_failed",
                "gripper_id": "primary",
                "surface": "command_gripper",
                "message": "Gripper request failed",
            }
        )

        code, details = manager._capture_kernel_error_diagnostics()
        assert code == "gripper_request_failed"
        assert details["gripper_id"] == "primary"
        assert details["surface"] == "command_gripper"

    def test_unknown_reason_token_is_not_promoted(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        self._write_fault({"reason_code": "not_a_real_token", "gripper_id": "g"})

        code, details = manager._capture_kernel_error_diagnostics()
        assert code is None
        assert details == {"reason_code": "not_a_real_token", "gripper_id": "g"}

    def test_snapshot_error_wins_over_gripper_record(self, kernel):
        from types import SimpleNamespace

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        kernel.get_execution_snapshot = lambda: SimpleNamespace(
            error_code="execution_aborted",
            error_details={"reason_code": "tracking_error_exceeded"},
        )
        self._write_fault({"reason_code": "gripper_goal_rejected"})

        code, details = manager._capture_kernel_error_diagnostics()
        assert code == "tracking_error_exceeded"
        assert details == {"reason_code": "tracking_error_exceeded"}

    def test_no_sources_returns_nothing(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        code, details = manager._capture_kernel_error_diagnostics()
        assert code is None
        assert details is None


class TestStopQuiescesPreparationPhase:
    """Stop must halt an in-flight execution even when the kernel is still
    preparing (planning/validating): pausing only EXECUTING let the async
    worker dispatch the motion after the cycle had already reported stopped
    (observed live: plan_completed + execute_started 100ms after Stop, arm
    moving 10.8s post-Stop)."""

    class _StubKernel(MockKernelClient):
        def __init__(self, states):
            super().__init__()
            self._scripted_states = list(states)
            self.pause_calls = 0
            self.stop_calls = 0

        def get_status(self):
            status = super().get_status()
            if self._scripted_states:
                status.state = self._scripted_states.pop(0)
            return status

        def pause(self):
            self.pause_calls += 1
            return CommandResult(accepted=True)

        def stop(self):
            self.stop_calls += 1
            return CommandResult(accepted=True)

    def _quiesce(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._quiesce_runtime(
            client_name="test_stop_quiesce",
            kernel_action="pause_if_executing",
        )

    def test_planning_phase_is_stopped_not_ignored(self, kernel):
        stub = self._StubKernel([
            ExecutionState.PLANNING,  # pre-loop status read
            ExecutionState.PLANNING,  # loop check -> stop()
            ExecutionState.STOPPED,   # re-check -> done
        ])
        self._quiesce(stub)
        assert stub.stop_calls == 1
        assert stub.pause_calls == 0

    def test_executing_phase_is_paused_unchanged_behavior(self, kernel):
        stub = self._StubKernel([
            ExecutionState.EXECUTING,
            ExecutionState.EXECUTING,
        ])
        self._quiesce(stub)
        assert stub.pause_calls == 1
        assert stub.stop_calls == 0

    def test_preparation_to_executing_race_is_closed(self, kernel):
        # Stop lands during VALIDATING; the kernel crosses into EXECUTING
        # between the stop attempt and the re-check.
        stub = self._StubKernel([
            ExecutionState.VALIDATING,  # pre-loop status read
            ExecutionState.VALIDATING,  # loop check -> stop()
            ExecutionState.EXECUTING,   # raced past the stop -> pause()
        ])
        self._quiesce(stub)
        assert stub.stop_calls == 1
        assert stub.pause_calls == 1

    def test_idle_kernel_is_untouched(self, kernel):
        stub = self._StubKernel([
            ExecutionState.IDLE,
            ExecutionState.IDLE,
        ])
        self._quiesce(stub)
        assert stub.stop_calls == 0
        assert stub.pause_calls == 0


class TestTickLoopContainment:
    """A tick-loop exception must never leave motion running unsupervised.

    The containment contract: halt the tree (cancelling owned kernel goals),
    stop the kernel directly as well, and park the cycle on the uniform
    STOPPED + error surface with the tree dropped, so Run is refused with an
    explicit reason and Reset rebuilds from the authored program.
    """

    @staticmethod
    def _await_state(manager, state, timeout_s=2.0):
        deadline = time.time() + timeout_s
        while manager.state != state and time.time() < deadline:
            time.sleep(0.01)
        assert manager.state == state

    @staticmethod
    def _inject_tick_exception(manager):
        def boom():
            raise RuntimeError("injected tick failure")

        # Instance attribute shadows the method for the next tick.
        manager._tree.tick = boom

    def test_tick_exception_stops_kernel_and_parks_stopped(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        # Let a motion dispatch so the kernel is genuinely executing.
        deadline = time.time() + 2.0
        while (
            slow_kernel.get_status().state != ExecutionState.EXECUTING
            and time.time() < deadline
        ):
            time.sleep(0.01)
        assert slow_kernel.get_status().state == ExecutionState.EXECUTING

        self._inject_tick_exception(manager)
        self._await_state(manager, CycleState.STOPPED)

        # The in-flight execution was stopped, not orphaned.
        assert slow_kernel.stop_count >= 1
        assert slow_kernel.get_status().state == ExecutionState.STOPPED
        assert manager.error_code == "tick_loop_exception"
        assert "injected tick failure" in (manager.error or "")

    def test_tick_exception_refuses_run_and_allows_reset(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)
        self._inject_tick_exception(manager)
        self._await_state(manager, CycleState.STOPPED)

        # The post-exception tree cannot be trusted for a resume: Run must
        # be refused with an explicit reason, never silently restart.
        with pytest.raises(RuntimeError, match="without a tree"):
            manager.run()
        assert manager.state == CycleState.STOPPED
        # The refusal must not wipe the diagnostic that explains why the
        # cycle is stopped — the operator still needs the original fault.
        assert manager.error_code == "tick_loop_exception"
        assert "injected tick failure" in (manager.error or "")

        # Reset is the documented recovery path and must work.
        manager.reset()
        assert manager.state == CycleState.IDLE
        assert manager.error is None

    def test_tick_exception_parks_stopped_even_if_kernel_stop_raises(self):
        class StopRaisesKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)

            def stop(self):  # type: ignore[override]
                raise RuntimeError("kernel unreachable")

        kernel = StopRaisesKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        self._inject_tick_exception(manager)

        # Containment steps are independent: kernel stop failing must not
        # prevent the cycle from parking on the STOPPED + error surface.
        self._await_state(manager, CycleState.STOPPED)
        assert manager.error_code == "tick_loop_exception"


class TestStopWatcherEscalation:
    """Stop watchers must be deadline-bounded, never wedge the cycle."""

    class _NeverSettlingKernel(MockKernelClient):
        """Reports STOPPING forever after a stop request."""

        def __init__(self):
            super().__init__(execute_duration_s=5.0)
            self.stop_called = threading.Event()

        def stop(self):  # type: ignore[override]
            self.stop_called.set()
            return CommandResult(accepted=True)

        def get_status(self):  # type: ignore[override]
            state = (
                ExecutionState.STOPPING
                if self.stop_called.is_set()
                else ExecutionState.EXECUTING
            )
            return ExecutionStatus(
                state=state,
                execution_id=self._execution_id,
                progress=0.0,
                active_move_id=self._active_move_id,
            )

    @staticmethod
    def _await_state(manager, state, timeout_s=3.0):
        deadline = time.time() + timeout_s
        while manager.state != state and time.time() < deadline:
            time.sleep(0.01)
        assert manager.state == state

    def test_stop_settle_escalates_after_deadline_and_reset_recovers(
        self, monkeypatch
    ):
        import manyforge_behavior.cycle_manager as cm

        monkeypatch.setattr(cm, "_STOP_SETTLE_DEADLINE_S", 0.2)
        kernel = self._NeverSettlingKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        manager.full_stop()
        assert manager.state == CycleState.STOPPING

        # Wedged kernel: without the deadline this stays STOPPING forever
        # and reset() is refused.
        self._await_state(manager, CycleState.STOPPED)
        assert manager.error_code == "stop_settle_timeout"
        assert manager.error_details["kernel_state"] == "stopping"

        # Run must fail closed (no trusted resume fingerprint) …
        with pytest.raises(RuntimeError):
            manager.run()
        # … and Reset must recover.
        manager.reset()
        assert manager.state == CycleState.IDLE

    def test_stop_settle_escalates_on_unreadable_status(self, monkeypatch):
        import manyforge_behavior.cycle_manager as cm

        monkeypatch.setattr(cm, "_STOP_SETTLE_MAX_STATUS_FAILURES", 3)

        class StatusDiesAfterStopKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.stop_called = threading.Event()
                self.post_stop_reads = 0

            def stop(self):  # type: ignore[override]
                self.stop_called.set()
                return CommandResult(accepted=True)

            def get_status(self):  # type: ignore[override]
                if self.stop_called.is_set():
                    # Allow full_stop()'s own post-stop read to succeed so
                    # the settle watcher starts; the watcher's reads fail.
                    self.post_stop_reads += 1
                    if self.post_stop_reads > 1:
                        raise RuntimeError("kernel status unreachable")
                    return ExecutionStatus(
                        state=ExecutionState.STOPPING,
                        execution_id=self._execution_id,
                        progress=0.0,
                        active_move_id=self._active_move_id,
                    )
                return super().get_status()

        kernel = StatusDiesAfterStopKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        manager.full_stop()

        self._await_state(manager, CycleState.STOPPED)
        assert manager.error_code == "stop_settle_status_unreadable"

    def test_motion_stop_escalates_after_deadline_and_unblocks_reset(
        self, monkeypatch
    ):
        import manyforge_behavior.cycle_manager as cm

        monkeypatch.setattr(cm, "_STOP_SETTLE_DEADLINE_S", 0.2)
        kernel = self._NeverSettlingKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        manager.stop()  # motion stop, cycle stays RUNNING
        assert manager.state == CycleState.RUNNING

        # While the motion stop is in flight, reset is (correctly) refused.
        with pytest.raises(RuntimeError, match="motion stop is in progress"):
            manager.reset()

        # After the deadline the watcher escalates to a full stop, which
        # unblocks reset instead of refusing forever.
        self._await_state(manager, CycleState.STOPPED)
        assert manager.error_code == "motion_stop_timeout"
        manager.reset()
        assert manager.state == CycleState.IDLE

    def test_reset_status_phase_timeout_is_wall_clock_bounded(self, monkeypatch):
        import manyforge_behavior.cycle_manager as cm

        release_status = threading.Event()
        status_entered = threading.Event()

        class BlockingStatusKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                status_entered.set()
                release_status.wait(timeout=5.0)
                return super().get_status()

        monkeypatch.setattr(cm, "_RESET_PHASE_DEADLINE_S", 0.02)
        kernel = BlockingStatusKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        started = time.monotonic()
        try:
            with pytest.raises(RuntimeError, match="kernel_status"):
                manager.reset(request_id="req-blocked-status")
        finally:
            release_status.set()

        assert status_entered.is_set()
        assert time.monotonic() - started < 0.5
        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "reset_kernel_status_timeout"

    def test_full_stop_blocked_status_arms_settle_watcher(self, monkeypatch):
        import manyforge_behavior.cycle_manager as cm

        release_status = threading.Event()

        class BlockingStatusKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                release_status.wait(timeout=5.0)
                return ExecutionStatus(
                    state=ExecutionState.STOPPING,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="blocked_status",
                )

            def stop(self):  # type: ignore[override]
                return CommandResult(accepted=True)

        monkeypatch.setattr(cm, "_KERNEL_CONTROL_CALL_TIMEOUT_S", 0.02)
        kernel = BlockingStatusKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.RUNNING

        try:
            started = time.monotonic()
            manager.full_stop()
            assert time.monotonic() - started < 0.5
            self._await_state(manager, CycleState.STOPPED, timeout_s=1.0)
            assert manager.error_code == "stop_settle_status_timeout"
        finally:
            release_status.set()

    def test_motion_stop_blocked_post_status_arms_watcher(self, monkeypatch):
        import manyforge_behavior.cycle_manager as cm

        release_status = threading.Event()

        class BlockingPostStopStatusKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.reads = 0

            def get_status(self):  # type: ignore[override]
                self.reads += 1
                if self.reads == 1:
                    return ExecutionStatus(
                        state=ExecutionState.EXECUTING,
                        execution_id=1,
                        progress=0.0,
                        active_move_id="blocked_motion_stop",
                    )
                release_status.wait(timeout=5.0)
                return ExecutionStatus(
                    state=ExecutionState.STOPPING,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="blocked_motion_stop",
                )

            def stop(self):  # type: ignore[override]
                return CommandResult(accepted=True)

        monkeypatch.setattr(cm, "_KERNEL_CONTROL_CALL_TIMEOUT_S", 0.02)
        kernel = BlockingPostStopStatusKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.RUNNING

        try:
            started = time.monotonic()
            manager.stop()
            assert time.monotonic() - started < 0.5
            self._await_state(manager, CycleState.STOPPED, timeout_s=1.0)
            assert manager.error_code == "motion_stop_status_timeout"
        finally:
            release_status.set()


class TestCycleManagerStopLivenessResiduals:
    """Regression tests for WS4 F1/F2/F3 stop-liveness residuals."""

    @staticmethod
    def _await_state(manager, state, timeout_s=1.0):
        deadline = time.monotonic() + timeout_s
        while manager.state != state and time.monotonic() < deadline:
            time.sleep(0.01)
        assert manager.state == state

    @staticmethod
    def _stopped_manager(kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.run()
        deadline = time.monotonic() + 1.0
        while kernel.execute_count == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        return manager

    @staticmethod
    def _simple_intervention_tree():
        return {
            "id": "timer",
            "name": "ws4_liveness_timer",
            "params": {"duration_s": 0.05},
        }

    def test_ack_retry_blocked_status_does_not_hold_cycle_lock(self, monkeypatch):
        release_status = threading.Event()
        status_entered = threading.Event()

        class BlockingStatusKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                status_entered.set()
                release_status.wait(timeout=5.0)
                return ExecutionStatus(
                    state=ExecutionState.INTERVENTION_REQUIRED,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="blocked_ack_status",
                )

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingStatusKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.STOPPED

        errors: list[BaseException] = []

        def acknowledge() -> None:
            try:
                manager.acknowledge_intervention_for_retry()
            except BaseException as exc:  # noqa: BLE001 - inspected below.
                errors.append(exc)

        thread = threading.Thread(target=acknowledge, daemon=True)
        thread.start()
        try:
            assert status_entered.wait(timeout=1.0)
            started = time.monotonic()
            assert manager.motion_stop_state == "idle"
            manager.full_stop()
            assert time.monotonic() - started < 0.2
            thread.join(timeout=1.0)
            assert any(isinstance(exc, TimeoutError) for exc in errors)
        finally:
            release_status.set()
            thread.join(timeout=1.0)

    def test_ack_retry_blocked_ack_does_not_hold_cycle_lock(self, monkeypatch):
        release_ack = threading.Event()
        ack_entered = threading.Event()

        class BlockingAckKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                return ExecutionStatus(
                    state=ExecutionState.INTERVENTION_REQUIRED,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="blocked_ack",
                )

            def acknowledge_intervention_for_retry(  # type: ignore[override]
                self,
                principal_id,
                reason,
                *,
                expected_epoch=None,
            ):
                ack_entered.set()
                release_ack.wait(timeout=5.0)
                return super().acknowledge_intervention_for_retry(
                    principal_id,
                    reason,
                    expected_epoch=expected_epoch,
                )

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingAckKernel()
        kernel._state = ExecutionState.INTERVENTION_REQUIRED
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.STOPPED

        errors: list[BaseException] = []

        def acknowledge() -> None:
            try:
                manager.acknowledge_intervention_for_retry()
            except BaseException as exc:  # noqa: BLE001 - inspected below.
                errors.append(exc)

        thread = threading.Thread(target=acknowledge, daemon=True)
        thread.start()
        try:
            assert ack_entered.wait(timeout=1.0)
            started = time.monotonic()
            assert manager.motion_stop_state == "idle"
            manager.full_stop()
            assert time.monotonic() - started < 0.2
            thread.join(timeout=1.0)
            assert any(isinstance(exc, TimeoutError) for exc in errors)
        finally:
            release_ack.set()
            thread.join(timeout=1.0)

    def test_start_intervention_blocked_status_does_not_hold_cycle_lock(self, monkeypatch):
        release_status = threading.Event()
        status_entered = threading.Event()

        class BlockingStatusKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                status_entered.set()
                release_status.wait(timeout=5.0)
                return ExecutionStatus(
                    state=ExecutionState.IDLE,
                    execution_id=0,
                    progress=0.0,
                    active_move_id="blocked_intervention_status",
                )

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingStatusKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        errors: list[BaseException] = []

        def start_intervention() -> None:
            try:
                manager.execute_intervention(
                    self._simple_intervention_tree(),
                    session_kind="manual_intervention",
                    authority_source="human",
                    principal_id="human:test",
                    approval_mode="human_approved",
                )
            except BaseException as exc:  # noqa: BLE001 - inspected below.
                errors.append(exc)

        thread = threading.Thread(target=start_intervention, daemon=True)
        thread.start()
        try:
            assert status_entered.wait(timeout=1.0)
            started = time.monotonic()
            assert manager.motion_stop_state == "idle"
            manager.full_stop()
            assert time.monotonic() - started < 0.2
            thread.join(timeout=1.0)
            assert any(isinstance(exc, TimeoutError) for exc in errors)
        finally:
            release_status.set()
            thread.join(timeout=1.0)

    def test_full_stop_scene_fingerprint_timeout_is_bounded(self, monkeypatch):
        release_scene = threading.Event()
        scene_entered = threading.Event()

        class BlockingSceneKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.block_scene = False

            def get_scene_objects(self):  # type: ignore[override]
                if self.block_scene:
                    scene_entered.set()
                    release_scene.wait(timeout=5.0)
                return super().get_scene_objects()

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingSceneKernel()
        manager = self._stopped_manager(kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        kernel.block_scene = True
        try:
            started = time.monotonic()
            manager.full_stop()
            assert time.monotonic() - started < 0.5
            assert scene_entered.is_set()
            assert manager.state == CycleState.STOPPED
            assert manager._resumable_stop_scene_fingerprint is None
            assert "TimeoutError" in (
                manager._resumable_stop_scene_fingerprint_error or ""
            )
        finally:
            release_scene.set()

    def test_stop_settle_watcher_does_not_wait_for_scene_fingerprint(
        self,
        monkeypatch,
    ):
        release_scene = threading.Event()
        scene_entered = threading.Event()

        class TerminalThenBlockingSceneKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.stop_called = threading.Event()
                self.post_stop_reads = 0

            def stop(self):  # type: ignore[override]
                self.stop_called.set()
                return CommandResult(accepted=True)

            def get_status(self):  # type: ignore[override]
                if not self.stop_called.is_set():
                    return ExecutionStatus(
                        state=ExecutionState.EXECUTING,
                        execution_id=1,
                        progress=0.0,
                        active_move_id="watcher_scene",
                    )
                self.post_stop_reads += 1
                state = (
                    ExecutionState.STOPPING
                    if self.post_stop_reads == 1
                    else ExecutionState.STOPPED
                )
                return ExecutionStatus(
                    state=state,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="watcher_scene",
                )

            def get_scene_objects(self):  # type: ignore[override]
                scene_entered.set()
                release_scene.wait(timeout=5.0)
                return super().get_scene_objects()

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = TerminalThenBlockingSceneKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.RUNNING
        manager._tree = runner.build_tree()

        try:
            manager.full_stop()
            assert manager.state == CycleState.STOPPING
            assert scene_entered.wait(timeout=1.0)
            self._await_state(manager, CycleState.STOPPED, timeout_s=0.5)
            fingerprint_error = manager._resumable_stop_scene_fingerprint_error or ""
            assert (
                fingerprint_error == "scene fingerprint capture pending"
                or "TimeoutError" in fingerprint_error
            )
        finally:
            release_scene.set()

    def test_full_stop_during_run_transition_is_honored(self):
        release_status = threading.Event()
        status_entered = threading.Event()

        class BlockingResumeStatusKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.block_resume_status = False

            def get_status(self):  # type: ignore[override]
                if self.block_resume_status:
                    status_entered.set()
                    release_status.wait(timeout=5.0)
                    self.block_resume_status = False
                    return ExecutionStatus(
                        state=ExecutionState.IDLE,
                        execution_id=0,
                        progress=0.0,
                        active_move_id="resume_full_stop",
                    )
                return super().get_status()

        kernel = BlockingResumeStatusKernel()
        manager = self._stopped_manager(kernel)
        kernel.block_resume_status = True

        run_thread = threading.Thread(target=manager.run, daemon=True)
        run_thread.start()
        assert status_entered.wait(timeout=1.0)

        manager.full_stop()
        release_status.set()
        run_thread.join(timeout=1.0)

        assert not run_thread.is_alive()
        assert manager.state == CycleState.STOPPED
        assert manager._pending_run_stop is None

    def test_motion_stop_during_run_transition_is_honored(self):
        release_status = threading.Event()
        status_entered = threading.Event()

        class BlockingResumeStatusKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.block_resume_status = False

            def get_status(self):  # type: ignore[override]
                if self.block_resume_status:
                    status_entered.set()
                    release_status.wait(timeout=5.0)
                    self.block_resume_status = False
                    return ExecutionStatus(
                        state=ExecutionState.IDLE,
                        execution_id=0,
                        progress=0.0,
                        active_move_id="resume_motion_stop",
                    )
                return super().get_status()

        kernel = BlockingResumeStatusKernel()
        manager = self._stopped_manager(kernel)
        kernel.block_resume_status = True

        run_thread = threading.Thread(target=manager.run, daemon=True)
        run_thread.start()
        assert status_entered.wait(timeout=1.0)

        manager.stop()
        release_status.set()
        run_thread.join(timeout=1.0)

        assert not run_thread.is_alive()
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "held"
        assert manager._pending_run_stop is None
        manager.full_stop()


class TestMotionErrorReadAllocation:
    """The 100 Hz motion-error read must not grow py_trees global state."""

    def test_read_motion_paused_error_does_not_register_new_clients(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager._read_motion_paused_error()
        clients_before = len(py_trees.blackboard.Blackboard.clients)
        for _ in range(200):
            manager._read_motion_paused_error()
        assert len(py_trees.blackboard.Blackboard.clients) == clients_before


class TestCycleShutdown:
    """Process-shutdown quiesce: motion stopped, tick thread joined."""

    def test_shutdown_stops_running_cycle_and_joins_tick_thread(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        deadline = time.time() + 2.0
        while (
            slow_kernel.get_status().state != ExecutionState.EXECUTING
            and time.time() < deadline
        ):
            time.sleep(0.01)
        tick_thread = manager._tick_thread
        assert tick_thread is not None and tick_thread.is_alive()

        assert manager.shutdown(timeout_s=2.0) is True

        assert not tick_thread.is_alive()
        assert slow_kernel.stop_count >= 1
        # The in-flight execution was routed through the kernel stop, not
        # abandoned to interpreter-exit teardown.
        assert slow_kernel.get_status().state in {
            ExecutionState.STOPPED,
            ExecutionState.IDLE,
        }

    def test_shutdown_joins_tick_thread_even_if_kernel_stop_raises(self):
        class StopRaisesKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)

            def stop(self):  # type: ignore[override]
                raise RuntimeError("kernel unreachable")

        kernel = StopRaisesKernel()
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        tick_thread = manager._tick_thread

        # Shutdown steps are independent: a failing kernel stop must not
        # prevent the tick thread from being signalled and joined.
        assert manager.shutdown(timeout_s=2.0) is True
        assert tick_thread is None or not tick_thread.is_alive()

    def test_shutdown_from_idle_is_safe(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.shutdown(timeout_s=1.0) is True


class TestProgramRunIdThreading:
    """spec/500 §2.2: the runner mints a program_run_id once per program
    start; motion nodes attach it to every ExecuteRequest and the kernel
    stamps it on every kernel event, so history joins on ids, not on
    timestamp guesswork."""

    def test_run_mints_id_and_threads_it_to_kernel_and_events(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        deadline = time.time() + 2.0
        while (
            slow_kernel.last_execute_request is None
            and time.time() < deadline
        ):
            time.sleep(0.01)
        assert slow_kernel.last_execute_request is not None

        run_id = manager._program_run_id
        assert run_id, "run() must mint a program_run_id at program start"

        bb = py_trees.blackboard.Client(name="test_program_run_id_read")
        bb.register_key(key="program_run_id", access=py_trees.common.Access.READ)
        assert bb.get("program_run_id") == run_id

        # The motion node attached the id to its ExecuteRequest …
        assert slow_kernel.last_execute_request.program_run_id == run_id
        # … and the kernel stamps it on every event for the execution.
        events = manager.drain_kernel_events_now() or manager.get_kernel_events_since(0)
        stamped = [e for e in events if e.get("program_run_id") == run_id]
        assert stamped, f"no kernel event carried program_run_id: {events[:3]}"

        manager.shutdown(timeout_s=2.0)

    def test_fresh_start_mints_new_id_resume_keeps_it(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        first = manager._program_run_id
        assert first
        time.sleep(0.05)

        manager.full_stop()
        deadline = time.time() + 2.0
        while manager.state != CycleState.STOPPED and time.time() < deadline:
            time.sleep(0.01)
        manager.run()  # resume: same program run, same id
        assert manager._program_run_id == first

        manager.reset()
        manager.run()  # fresh program start: new id
        assert manager._program_run_id and manager._program_run_id != first
        manager.shutdown(timeout_s=2.0)


class TestControlCommandEpoch:
    def test_timed_out_resume_invalidates_abandoned_worker(self, monkeypatch):
        release_resume = threading.Event()
        resume_entered = threading.Event()
        resume_done = threading.Event()

        class BlockingResumeKernel(MockKernelClient):
            def get_status(self):  # type: ignore[override]
                return ExecutionStatus(
                    state=ExecutionState.PAUSED,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="late_resume",
                )

            def resume(self, *, expected_epoch=None):  # type: ignore[override]
                resume_entered.set()
                release_resume.wait(timeout=5.0)
                try:
                    return super().resume(expected_epoch=expected_epoch)
                finally:
                    resume_done.set()

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingResumeKernel(execute_duration_s=5.0)
        kernel._state = ExecutionState.PAUSED  # noqa: SLF001
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._tree = runner.build_tree()  # noqa: SLF001
        manager._state = CycleState.STOPPED  # noqa: SLF001
        with manager._lock:  # noqa: SLF001
            manager._store_resumable_stop_scene_fingerprint_locked(  # noqa: SLF001
                _scene_resume_fingerprint_from_objects(kernel.get_scene_objects()),
                None,
            )

        with pytest.raises(TimeoutError):
            manager.run()

        assert resume_entered.is_set()
        assert kernel.control_command_epoch() == 1
        release_resume.set()
        assert resume_done.wait(timeout=1.0)
        assert kernel.resume_count == 0
        assert kernel.get_status().state == ExecutionState.PAUSED
        assert manager.state == CycleState.STOPPED

    def test_ack_retry_during_wedged_run_transition_is_bounded(self, monkeypatch):
        release_status = threading.Event()
        status_entered = threading.Event()

        class FirstRunStatusBlocksKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self._status_calls = 0
                self._status_lock = threading.Lock()

            def get_status(self):  # type: ignore[override]
                with self._status_lock:
                    self._status_calls += 1
                    status_call = self._status_calls
                if status_call == 1:
                    status_entered.set()
                    release_status.wait(timeout=5.0)
                    return ExecutionStatus(
                        state=ExecutionState.PAUSED,
                        execution_id=1,
                        progress=0.0,
                        active_move_id="wedged_run",
                    )
                return ExecutionStatus(
                    state=ExecutionState.INTERVENTION_REQUIRED,
                    execution_id=1,
                    progress=0.0,
                    active_move_id="ack_after_wedged_run",
                )

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = FirstRunStatusBlocksKernel()
        kernel._state = ExecutionState.INTERVENTION_REQUIRED  # noqa: SLF001
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._tree = runner.build_tree()  # noqa: SLF001
        manager._state = CycleState.STOPPED  # noqa: SLF001
        with manager._lock:  # noqa: SLF001
            manager._store_resumable_stop_scene_fingerprint_locked(  # noqa: SLF001
                _scene_resume_fingerprint_from_objects(kernel.get_scene_objects()),
                None,
            )

        run_errors: list[BaseException] = []

        def run() -> None:
            try:
                manager.run()
            except BaseException as exc:  # noqa: BLE001 - inspected below.
                run_errors.append(exc)

        run_thread = threading.Thread(target=run, daemon=True)
        run_thread.start()
        try:
            assert status_entered.wait(timeout=1.0)
            ack_errors: list[BaseException] = []
            ack_elapsed: list[float] = []

            def acknowledge() -> None:
                started = time.monotonic()
                try:
                    manager.acknowledge_intervention_for_retry()
                except BaseException as exc:  # noqa: BLE001 - inspected below.
                    ack_errors.append(exc)
                finally:
                    ack_elapsed.append(time.monotonic() - started)

            ack_thread = threading.Thread(target=acknowledge, daemon=True)
            ack_thread.start()
            ack_thread.join(timeout=0.5)
            assert not ack_thread.is_alive()
            assert not ack_errors
            assert ack_elapsed and ack_elapsed[0] < 0.3
            run_thread.join(timeout=1.0)
            assert not run_thread.is_alive()
            assert any(isinstance(exc, TimeoutError) for exc in run_errors)
            assert kernel.acknowledge_intervention_for_retry_count == 1
            assert manager.state == CycleState.STOPPED
        finally:
            release_status.set()
            run_thread.join(timeout=1.0)

    def test_timed_out_reset_invalidates_abandoned_worker(self, monkeypatch):
        release_reset = threading.Event()
        reset_entered = threading.Event()
        reset_done = threading.Event()

        class BlockingResetKernel(MockKernelClient):
            def reset(self, *, expected_epoch=None):  # type: ignore[override]
                reset_entered.set()
                release_reset.wait(timeout=5.0)
                try:
                    return super().reset(expected_epoch=expected_epoch)
                finally:
                    reset_done.set()

        monkeypatch.setattr(cycle_manager_module, "_RESET_PHASE_DEADLINE_S", 0.02)
        kernel = BlockingResetKernel()
        kernel._state = ExecutionState.STOPPED  # noqa: SLF001
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        with pytest.raises(RuntimeError, match="kernel_reset"):
            manager.reset(request_id="reset-epoch-timeout")

        assert reset_entered.is_set()
        assert kernel.control_command_epoch() == 1
        release_reset.set()
        assert reset_done.wait(timeout=1.0)
        assert kernel.reset_count == 0
        assert kernel.get_status().state == ExecutionState.STOPPED
        assert manager.state == CycleState.STOPPED


class TestKernelEventPipelineStatus:
    """OW1: pipeline loss accounting is monotonic and observable."""

    def test_collector_and_manager_drops_are_reported(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        status = manager.kernel_event_pipeline_status()
        assert status == {
            "collector_dropped_total": 0,
            "manager_dropped_total": 0,
            "last_sequence_seen": 0,
        }

        # Overflow the mock collector ring (maxlen 4096) without draining:
        # the collector must count its own drop-oldest evictions.
        for i in range(5000):
            kernel._emit_kernel_event("state_transition")
        manager.drain_kernel_events_now()
        status = manager.kernel_event_pipeline_status()
        assert status["collector_dropped_total"] == 5000 - 4096
        assert status["last_sequence_seen"] == 5000

        # Overflow the manager-side deque with undrained events: evictions
        # count as manager drops.
        for i in range(5000):
            kernel._emit_kernel_event("state_transition")
        manager.drain_kernel_events_now()
        status = manager.kernel_event_pipeline_status()
        assert status["manager_dropped_total"] > 0
