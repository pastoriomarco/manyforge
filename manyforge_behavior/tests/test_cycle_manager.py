"""Tests for the CycleManager."""

from pathlib import Path
import threading
import time
import tempfile
import os
import pytest
import py_trees
import yaml

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
        
        # Run then stop
        manager.run()
        time.sleep(0.2)
        manager.stop()
        assert manager.state == CycleState.STOPPED
        
        # Run again — should resume
        manager.run()
        assert manager.state == CycleState.RUNNING

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
            manager.stop()
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
            manager.stop()
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

    def test_run_from_stopped_motion_soft_fail_clears_interrupted_kernel_retry(
        self, kernel
    ):
        """Run from STOPPED is an explicit retry after a motion soft-fail.

        Runtime collision faults leave the kernel in INTERRUPTED with an
        active latch. Removing the obstacle fixes the scene but does not clear
        that execution-local latch. The operator pressing Run means "retry
        this held motion"; CycleManager must acknowledge the interrupted
        kernel execution boundary first so the motion node can submit a fresh
        execute request.
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
            while manager.state != CycleState.STOPPED and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.STOPPED
            assert bb.get("motion_paused_due_to_error") is True
            assert bb.get("motion_error_message") is not None
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
        
        manager.stop()
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

        stop_thread = threading.Thread(target=manager.stop, daemon=True)
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

        manager.stop()
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
        manager.stop()
        assert manager.state == CycleState.STOPPED
        assert wait_timeouts, (
            "tick loop must sleep via _tick_stop_event.wait(timeout=...), "
            "not time.sleep()"
        )

    def test_stop_from_stopped_is_noop(self, kernel):
        """stop() from STOPPED is a no-op."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.stop()
        assert manager.state == CycleState.STOPPED
        
        # Stop again — should be no-op
        manager.stop()
        assert manager.state == CycleState.STOPPED
    
    def test_stop_from_idle_is_noop(self, kernel):
        """stop() from IDLE is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.stop()
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
        manager.stop()

        # Controlled stop, not pause: kernel.stop() invoked, kernel.pause() not.
        assert slow_kernel.stop_count == 1
        assert slow_kernel.pause_count == 0


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
        manager.stop()
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
        
        manager.stop()
        
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

    def test_tick_exception_sets_error_and_transitions_to_idle(self, kernel):
        """A tick() exception must populate manager.error and go to IDLE."""
        slow_kernel = MockKernelClient(execute_duration_s=2.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)

        # Inject a fault: replace the tree's tick with one that raises
        original_tick = manager._tree.tick

        def exploding_tick():
            raise RuntimeError("sensor fault injected")

        manager._tree.tick = exploding_tick

        # Wait for the tick loop to encounter the exception
        time.sleep(0.15)

        assert manager.state == CycleState.IDLE
        assert manager.error is not None
        assert "sensor fault injected" in manager.error

    def test_error_cleared_on_next_run(self, kernel):
        """run() after an error clears the error field."""
        slow_kernel = MockKernelClient(execute_duration_s=2.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)

        manager._tree.tick = lambda: (_ for _ in ()).throw(RuntimeError("boom"))
        time.sleep(0.15)

        assert manager.error is not None

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

            manager.stop()
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
                manager.stop()
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

        manager.stop()
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
        manager.stop()
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
            manager.stop()
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

        manager.stop()
        manager.reset()

    def test_execute_from_stopped_rejects_paused_kernel(self, kernel):
        """Wrapper-only intervention does not hijack a paused kernel motion.

        Operator stop() now preempts via the controlled stop (kernel STOPPED,
        no held motion), so it no longer produces a paused kernel. The guard
        still protects the motion-soft-fail hold case, where the kernel is
        genuinely PAUSED with the cycle STOPPED — recreate that condition
        directly and assert the guard still rejects the wrapper-only
        intervention."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        manager.stop()
        # Recreate the soft-fail held-motion condition: cycle STOPPED + kernel
        # PAUSED (the only state the guard protects against now). _make_manager
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
        manager.stop()
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


_ROS2_CONTROL_FAULT_REASON_HEADER = (
    Path(__file__).parent.parent.parent
    / "manyforge_roboplan"
    / "include"
    / "manyforge_roboplan"
    / "ros2_control_capability.hpp"
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

        assert _ROS2_CONTROL_FAULT_REASON_HEADER.is_file(), (
            "ros2_control_capability.hpp not found at "
            f"{_ROS2_CONTROL_FAULT_REASON_HEADER}; the taxonomy parity test "
            "requires the monorepo layout (manyforge_roboplan next to "
            "manyforge_behavior)."
        )
        text = _ROS2_CONTROL_FAULT_REASON_HEADER.read_text(encoding="utf-8")
        match = re.search(
            r"namespace ros2_control_fault_reason \{(.*?)\}\s*//\s*namespace"
            r"\s+ros2_control_fault_reason",
            text,
            flags=re.DOTALL,
        )
        assert match, (
            "namespace ros2_control_fault_reason not found in "
            "ros2_control_capability.hpp — the 6A taxonomy moved; update "
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
        manager.stop()
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
        manager.stop()
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

    def test_run_from_stopped_with_kernel_intervention_fails_closed_if_intervention_session_running(
        self,
    ):
        # An intervention session that the cycle manager still considers
        # running is a direct-control lease per spec/400 §6.6.2 — Run MUST
        # refuse until the operator ends/promotes/aborts that session.
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.stop()
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
        manager.stop()
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
        manager.stop()
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
                "reason_code": "gripper_goal_rejected",
                "gripper_id": "primary",
                "surface": "command_gripper",
                "message": "Gripper action goal was rejected",
            }
        )

        code, details = manager._capture_kernel_error_diagnostics()
        assert code == "gripper_goal_rejected"
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
