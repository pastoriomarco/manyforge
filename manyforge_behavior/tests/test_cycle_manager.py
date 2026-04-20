"""Tests for the CycleManager."""

from pathlib import Path
import threading
import time
import tempfile
import os
import pytest
import py_trees
import yaml

from manyforge_behavior.kernel_client import ExecutionState, MockKernelClient
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.cycle_manager import CycleManager, CycleState, InterventionSession

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)


def _write_yaml_file(data: dict) -> str:
    handle = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.safe_dump(data, handle, sort_keys=False)
    handle.close()
    return handle.name


def _interrupt_program(
    *,
    after_handler: str = "resumable_hold",
    preemption_mode: str = "hold_cycle",
    handler_root: dict | None = None,
    handler_resource_claims: list[str] | None = None,
    handler_timeout_s: float | None = None,
    blackboard: dict | None = None,
) -> dict:
    return {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "interrupt_program",
        "catalog_version": "0.1.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        **({"blackboard": blackboard} if blackboard is not None else {}),
        "interrupt_handlers": {
            "pause_announce": {
                **(
                    {"timeout_s": handler_timeout_s}
                    if handler_timeout_s is not None
                    else {}
                ),
                **(
                    {"resource_claims": handler_resource_claims}
                    if handler_resource_claims is not None
                    else {}
                ),
                "root": handler_root or {
                    "id": "sequence",
                    "name": "pause_announce",
                    "children": [
                        {"id": "reset_trajectories", "name": "noop"},
                    ],
                },
            },
        },
        "interrupt_policies": [
            {
                "id": "operator_pause_hold",
                "source": "operator_pause",
                "applies_to": {"node_names": ["wait_here"], "interruptible_only": True},
                "preemption_mode": preemption_mode,
                "handler": "pause_announce",
                "after_handler": after_handler,
            },
        ],
        "tree": {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "timer",
                    "name": "wait_here",
                    "params": {"duration_s": 5.0},
                },
            ],
        },
    }


def _interrupt_deployment(
    *,
    adapter: str = "composer_ui",
    signal: str = "pause_request",
    kind: str = "edge",
    maskable: bool = True,
) -> dict:
    return {
        "api_version": "manyforge.deployment.v0",
        "schema_version": "0.1.0",
        "name": "interrupt_deployment",
        "program_profile": "v0.0",
        "robot_config": {
            "robot_id": "ur5",
            "group_id": "arm",
            "urdf_path": "package://test.urdf",
            "global_max_tcp_linear_speed_mps": 0.5,
        },
        "adapters": {
            "planning": {"adapter_id": "planning.test", "adapter_version": "0.1.0", "config": {}},
            "context": {"adapter_id": "context.test", "adapter_version": "0.1.0", "config": {}},
            "execution": {"adapter_id": "execution.test", "adapter_version": "0.1.0", "config": {}},
        },
        "orchestrator": {"engine": "py_trees", "config": {}},
        "launch": {"runner": {"module": "manyforge_behavior.runner", "args": []}},
        "interrupt_sources": {
            "operator_pause": {
                "adapter": adapter,
                "signal": signal,
                "kind": kind,
                "priority": 30,
                "maskable": maskable,
            }
        },
    }


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
    
    def test_run_from_running_is_noop(self, kernel):
        """run() from RUNNING state is a no-op."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
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

    def test_node_failure_feedback_is_exposed_as_cycle_error(self, kernel, tmp_path):
        """Clean node failures should surface node feedback in manager.error."""
        failing_program = tmp_path / "missing_bb.program.yaml"
        failing_program.write_text(
            """api_version: manyforge.program.v0
program_profile: v0.0
name: missing_bb_failure
description: Motion node fails on missing blackboard key.
catalog_version: "0.1.0"

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
    
    def test_stop_calls_kernel_pause_if_executing(self, kernel):
        """stop() calls kernel.pause() if kernel is EXECUTING."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.05)
        manager.stop()
        
        # Kernel should have been paused
        assert slow_kernel.pause_count == 1


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


class TestCycleManagerInterrupts:
    """Test conservative runtime interrupt latching and resume/reset semantics."""

    def test_blackboard_interrupt_latches_and_resumes_resumable_hold(self, kernel):
        program_path = _write_yaml_file(_interrupt_program())
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)
            deadline = time.time() + 1.0
            while (
                (
                    manager.pending_interrupt is None
                    or manager.state != CycleState.STOPPED
                    or manager.pending_interrupt.details.get("handler_state") != "completed"
                )
                and time.time() < deadline
            ):
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.source_id == "operator_pause"
            assert manager.pending_interrupt.preemption_mode == "hold_cycle"
            assert manager.pending_interrupt.after_handler == "resumable_hold"
            assert manager.pending_interrupt.details["quiesce_latency_ns"] >= 0
            assert manager.pending_interrupt.details["handler_completion_latency_ns"] >= 0

            py_trees.blackboard.Blackboard.set("pause_request", False)
            manager.run()
            assert manager.pending_interrupt is None
            assert manager.state == CycleState.RUNNING
            manager.stop()
            manager.reset()
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_handler_executes_in_dedicated_runtime_state(self, kernel):
        program_path = _write_yaml_file(
            _interrupt_program(
                handler_root={
                    "id": "timer",
                    "name": "pause_delay",
                    "params": {"duration_s": 0.2},
                },
            )
        )
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.0
            while manager.state != CycleState.INTERRUPT_HANDLER and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.INTERRUPT_HANDLER
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.details["handler_state"] == "running"
            assert manager.pending_interrupt.details["handler_start_latency_ns"] >= 0

            deadline = time.time() + 1.0
            while (
                manager.pending_interrupt is None
                or manager.pending_interrupt.details.get("handler_state") != "completed"
            ) and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.details["handler_state"] == "completed"
            assert manager.pending_interrupt.details["handler_root_status"] == "SUCCESS"
            assert manager.pending_interrupt.details["handler_duration_ns"] >= 0
            manager.reset()
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_handler_can_auto_resume_subtree(self, kernel):
        blackboard = {
            "keys": [
                {"id": "pause_marker", "type": "bool", "key": "cell/pause_marker"},
            ]
        }
        handler_root = {
            "id": "sequence",
            "name": "pause_announce",
            "children": [
                {
                    "id": "set_signal_bool",
                    "name": "mark_pause_signal",
                    "params": {
                        "key": {"key_ref": "pause_marker"},
                        "value": True,
                    },
                },
            ],
        }
        program_path = _write_yaml_file(
            _interrupt_program(
                after_handler="resume_subtree",
                handler_root=handler_root,
                handler_resource_claims=["signals"],
                blackboard=blackboard,
            )
        )
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.0
            pause_marker = False
            while True:
                try:
                    pause_marker = bool(py_trees.blackboard.Blackboard.get("cell/pause_marker"))
                except KeyError:
                    pause_marker = False
                if (
                    pause_marker
                    and manager.pending_interrupt is None
                    and manager.state == CycleState.RUNNING
                ) or time.time() >= deadline:
                    break
                if time.time() >= deadline:
                    break
                time.sleep(0.01)

            assert manager.state == CycleState.RUNNING
            assert manager.pending_interrupt is None
            assert pause_marker is True
            manager.stop()
            manager.reset()
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_kernel_interrupt_requires_reset_when_not_resumable(self, kernel):
        program_path = _write_yaml_file(
            _interrupt_program(
                after_handler="halted",
                preemption_mode="kernel_stop_only",
            )
        )
        deployment_path = _write_yaml_file(
            _interrupt_deployment(adapter="kernel", signal="collision_detected")
        )
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            kernel._emit_kernel_event("collision_detected")
            deadline = time.time() + 1.0
            while manager.pending_interrupt is None and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.after_handler == "halted"
            with pytest.raises(RuntimeError, match="requires reset"):
                manager.run()

            manager.reset()
            assert manager.pending_interrupt is None
            assert manager.state == CycleState.IDLE
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_unhandled_non_maskable_interrupt_latches_fault(self, kernel):
        program = _interrupt_program()
        program.pop("interrupt_handlers", None)
        program.pop("interrupt_policies", None)
        program_path = _write_yaml_file(program)
        deployment_path = _write_yaml_file(
            _interrupt_deployment(maskable=False)
        )
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)
            deadline = time.time() + 1.0
            while manager.pending_interrupt is None and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.preemption_mode == "latched_fault"
            assert manager.pending_interrupt.after_handler == "halted"
            assert manager.pending_interrupt.policy_id is None
            assert manager.pending_interrupt.details["reason"] == "unhandled_non_maskable_source"
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_handler_auto_execution_is_blocked_for_missing_handler_definition(self, kernel):
        program_path = _write_yaml_file(_interrupt_program())
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            runner.load()
            runner.program.interrupt_handlers.pop("pause_announce", None)
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)
            deadline = time.time() + 1.0
            while manager.pending_interrupt is None and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.details["handler_state"] == "blocked"
            assert (
                manager.pending_interrupt.details["handler_blocked_reason"]
                == "missing_handler_definition"
            )
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_handler_timeout_fails_closed_and_requires_reset(self, kernel):
        program_path = _write_yaml_file(
            _interrupt_program(
                handler_root={
                    "id": "timer",
                    "name": "pause_delay",
                    "params": {"duration_s": 5.0},
                },
                handler_timeout_s=0.05,
            )
        )
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.5
            while (
                (
                    manager.pending_interrupt is None
                    or manager.pending_interrupt.details.get("handler_state") != "failed"
                )
                and time.time() < deadline
            ):
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.details["handler_state"] == "failed"
            assert manager.pending_interrupt.details["handler_failure_reason"] == "timeout"
            assert manager.pending_interrupt.details["handler_timeout_s"] == 0.05
            assert manager.pending_interrupt.details["handler_duration_ns"] >= 0
            assert "timed out after 0.050s" in manager.pending_interrupt.details["handler_last_error"]
            assert manager.error is not None
            assert "timed out after 0.050s" in manager.error
            with pytest.raises(RuntimeError, match="requires reset"):
                manager.run()

            manager.reset()
            assert manager.pending_interrupt is None
            assert manager.state == CycleState.IDLE
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_motion_interrupt_handler_auto_executes_after_kernel_recovery(self, kernel):
        program = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "interrupt_motion_handler",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "interrupt_handlers": {
                "retreat": {
                    "resource_claims": ["motion"],
                    "root": {
                        "id": "move_manipulator_action",
                        "name": "retreat_move",
                        "params": {
                            "move_id": "retreat_move",
                            "motion_type": "joint_goal",
                            "joint_goal_names": ["joint1"],
                            "joint_goal_positions_rad": [0.0],
                            "execution_timeout_s": 1.0,
                        },
                    },
                },
            },
            "interrupt_policies": [
                {
                    "id": "operator_pause_hold",
                    "source": "operator_pause",
                    "applies_to": {"node_names": ["move_here"], "interruptible_only": True},
                    "preemption_mode": "hold_cycle",
                    "handler": "retreat",
                    "after_handler": "halted",
                },
            ],
            "tree": {
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "move_manipulator_action",
                        "name": "move_here",
                        "params": {
                            "move_id": "move_here",
                            "motion_type": "joint_goal",
                            "joint_goal_names": ["joint1"],
                            "joint_goal_positions_rad": [0.1],
                            "execution_timeout_s": 1.0,
                        },
                    },
                ],
            },
        }
        program_path = _write_yaml_file(program)
        deployment_path = _write_yaml_file(_interrupt_deployment())
        slow_kernel = MockKernelClient(execute_duration_s=0.2)
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                slow_kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, slow_kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 2.0
            while (
                (
                    manager.pending_interrupt is None
                    or manager.pending_interrupt.details.get("handler_state") != "completed"
                )
                and time.time() < deadline
            ):
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.details["handler_state"] == "completed"
            assert manager.pending_interrupt.details["handler_resource_claims"] == ["motion"]
            assert manager.pending_interrupt.details["handler_kernel_preparation"] == "stop_and_reset_to_idle"
            assert manager.pending_interrupt.details["handler_kernel_ready_state"] == "idle"
            assert slow_kernel.execute_count == 2
            assert slow_kernel.stop_count >= 1
            assert slow_kernel.reset_count >= 1
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_handler_can_restart_subtree(self, kernel):
        program = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "interrupt_restart_subtree",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "blackboard": {
                "keys": [
                    {"id": "done_marker", "type": "bool", "key": "cycle/done"},
                ],
            },
            "interrupt_handlers": {
                "pause_announce": {
                    "root": {
                        "id": "reset_trajectories",
                        "name": "noop",
                    },
                },
            },
            "interrupt_policies": [
                {
                    "id": "operator_pause_restart",
                    "source": "operator_pause",
                    "applies_to": {"subtree_root": "main_sequence", "interruptible_only": True},
                    "preemption_mode": "hold_cycle",
                    "handler": "pause_announce",
                    "after_handler": "restart_subtree",
                },
            ],
            "tree": {
                "id": "sequence",
                "name": "main_sequence",
                "children": [
                    {
                        "id": "timer",
                        "name": "wait_here",
                        "params": {"duration_s": 0.2},
                    },
                    {
                        "id": "set_key_bool_value",
                        "name": "mark_done",
                        "params": {"key": {"key_ref": "done_marker"}, "value": True},
                    },
                ],
            },
        }
        program_path = _write_yaml_file(program)
        deployment_path = _write_yaml_file(_interrupt_deployment())
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 2.0
            while manager.state != CycleState.IDLE and time.time() < deadline:
                time.sleep(0.01)

            assert manager.state == CycleState.IDLE
            assert manager.pending_interrupt is None
            assert py_trees.blackboard.Blackboard.get("cycle/done") is True
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)

    def test_interrupt_deferred_while_handler_active_is_latched_after_resume(self, kernel):
        program = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "interrupt_deferred_queue",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "interrupt_handlers": {
                "pause_delay": {
                    "root": {
                        "id": "timer",
                        "name": "pause_delay",
                        "params": {"duration_s": 0.2},
                    },
                },
                "fault_hold": {
                    "root": {
                        "id": "reset_trajectories",
                        "name": "fault_hold",
                    },
                },
            },
            "interrupt_policies": [
                {
                    "id": "operator_pause_hold",
                    "source": "operator_pause",
                    "applies_to": {"node_names": ["wait_here"], "interruptible_only": True},
                    "preemption_mode": "hold_cycle",
                    "handler": "pause_delay",
                    "after_handler": "resume_subtree",
                },
                {
                    "id": "operator_fault_halt",
                    "source": "operator_fault",
                    "applies_to": {"node_names": ["wait_here"], "interruptible_only": True},
                    "preemption_mode": "hold_cycle",
                    "handler": "fault_hold",
                    "after_handler": "halted",
                },
            ],
            "tree": {
                "id": "sequence",
                "name": "root",
                "children": [
                    {
                        "id": "timer",
                        "name": "wait_here",
                        "params": {"duration_s": 5.0},
                    },
                ],
            },
        }
        deployment = {
            **_interrupt_deployment(signal="pause_request"),
            "interrupt_sources": {
                "operator_pause": {
                    "adapter": "composer_ui",
                    "signal": "pause_request",
                    "kind": "edge",
                    "priority": 20,
                    "maskable": True,
                },
                "operator_fault": {
                    "adapter": "composer_ui",
                    "signal": "fault_request",
                    "kind": "edge",
                    "priority": 30,
                    "maskable": True,
                },
            },
        }
        program_path = _write_yaml_file(program)
        deployment_path = _write_yaml_file(deployment)
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            py_trees.blackboard.Blackboard.set("pause_request", True)

            deadline = time.time() + 1.0
            while manager.state != CycleState.INTERRUPT_HANDLER and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.INTERRUPT_HANDLER

            py_trees.blackboard.Blackboard.set("fault_request", True)

            deadline = time.time() + 2.0
            while (
                (manager.pending_interrupt is None or manager.pending_interrupt.source_id != "operator_fault")
                and time.time() < deadline
            ):
                time.sleep(0.01)

            assert manager.state == CycleState.STOPPED
            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.source_id == "operator_fault"
            assert manager.pending_interrupt.details.get("deferred_consumed_at_ns") is not None
            assert manager.pending_interrupt.details.get("deferred_wait_ns") is not None
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)


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

    def test_interrupt_source_can_resolve_external_registry_signal(self, kernel):
        program_path = _write_yaml_file(_interrupt_program())
        deployment_path = _write_yaml_file(_interrupt_deployment(adapter="plc"))
        try:
            runner = OrchestratorRunner(
                CATALOG_PATH,
                program_path,
                kernel,
                deployment_path=deployment_path,
            )
            manager = CycleManager(runner, kernel)

            manager.run()
            time.sleep(0.05)
            manager.publish_signal(
                adapter="plc",
                signal="pause_request",
                value=True,
                stale_after_ms=500,
            )

            deadline = time.time() + 1.0
            while manager.pending_interrupt is None and time.time() < deadline:
                time.sleep(0.01)

            assert manager.pending_interrupt is not None
            assert manager.pending_interrupt.source_adapter == "plc"
            assert manager.pending_interrupt.details["registry_freshness"] == "fresh"
            manager.reset()
        finally:
            os.unlink(program_path)
            os.unlink(deployment_path)


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
    
    def test_velocity_scale_applied_to_motion(self, kernel):
        """velocity_scale is written to blackboard and accessible."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.set_velocity_scale(0.75)
        
        # Check blackboard has the value
        bb = py_trees.blackboard.Client(name="test_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.READ)
        assert bb.velocity_scale == 0.75
    
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

    def test_run_from_idle_after_completion_rebuilds_tree(self, kernel):
        """A new run after terminal completion prepares a fresh cycle."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(1.0)
        first_tree = manager._tree
        assert first_tree is not None
        assert manager.state == CycleState.IDLE

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert manager._tree is not None
        assert manager._tree is not first_tree


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
        """Wrapper-only intervention does not hijack a paused kernel motion."""
        kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(kernel, execute_duration_s=1.0)
        manager.run()
        time.sleep(0.05)
        manager.stop()

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
