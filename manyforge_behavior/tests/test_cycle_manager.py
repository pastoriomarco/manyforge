"""Tests for the CycleManager."""

from pathlib import Path
from manyforge_behavior.node_environment import build_node_environment
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
    canonical_recovery_trigger_values,
)

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
NODE_ENVIRONMENT = build_node_environment(CATALOG_PATH)
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)


def _script_ready_recovery(
    kernel: MockKernelClient,
    *,
    action: str = "replan_from_stopped_state",
    recovery_class: str = "ordinary_operator_stop",
    status: str = "retry_ready",
    execution_state: str = "stopped",
) -> dict:
    """Install one explicit kernel assessment/result pair for integration tests.

    The mock deliberately does not reproduce the C++ admission matrix. Tests
    script the authority verdict they need and then verify CycleManager binds
    and forwards every exact token from that verdict.
    """
    kernel._state = ExecutionState(execution_state)
    assessment = {
        "schema_version": "manyforge.kernel_recovery_assessment.v1",
        "assessment_revision": 11,
        "assessed_at_ns": time.monotonic_ns(),
        "control_command_epoch": kernel.control_command_epoch(),
        "execution_id": kernel.get_status().execution_id,
        "execution_state": execution_state,
        "recovery_class": recovery_class,
        "status": status,
        "latch_id": "latch-test" if recovery_class != "ordinary_operator_stop" else "",
        "source_id": "test",
        "policy_id": "test",
        "preemption_result": "succeeded",
        "stop_owner_active": False,
        "stop_owner_generation": 7,
        "backend_stop_confirmed": True,
        "stop_certificate_id": "stop-cert-test",
        "observed_state_available": True,
        "observed_state_sequence": 19,
        "observed_state_source_id": "mock",
        "observed_state_authority_profile": "mock_measured",
        "observed_state_measured": True,
        "observed_state_valid_until_ns": time.monotonic_ns() + 5_000_000_000,
        "observed_state_fresh": True,
        "observed_state_at_rest": True,
        "stronger_authority_active": False,
        "queued_latch_present": False,
        "retry_admissible": status == "retry_ready",
        "allowed_actions": [action, "reset_cycle"],
        "reason_code": "recovery_stop_certified",
        "evidence_refs": ["stop-cert-test", "kernel-event:19"],
    }
    resulting = dict(assessment)
    resulting.update(
        {
            "assessment_revision": 12,
            "execution_state": "idle",
            "recovery_class": "none",
            "status": "unavailable",
            "latch_id": "",
            "stop_certificate_id": "",
            "retry_admissible": False,
            "allowed_actions": [],
            "reason_code": "no_recovery_active",
            "evidence_refs": [],
        }
    )
    kernel.script_recovery_assessment(assessment)
    kernel.script_recovery_action_result(
        {
            "request_id": "scripted",
            "action": action,
            "accepted": True,
            "error": None,
            "resulting_assessment": resulting,
        }
    )
    return assessment


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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.state == CycleState.IDLE
    
    def test_velocity_scale_default_1_0(self, kernel):
        """Velocity scale defaults to 1.0."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.velocity_scale == 1.0


class TestCycleManagerRun:
    """Test run() method."""
    
    def test_run_from_idle_starts_ticking(self, kernel):
        """run() from IDLE builds tree and starts tick thread."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.run()
        
        assert manager.state == CycleState.RUNNING
        # Give tick thread time to run
        time.sleep(0.2)
        # Tree should complete quickly, so state may change
        # But we know it started running

    def test_final_run_admission_rejection_never_starts_tick_thread(
        self,
        kernel,
    ):
        """A changed execution environment cannot escape through Run."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        attempted_run_ids = []

        def reject() -> None:
            attempted_run_ids.append(manager.program_run_id)
            raise RuntimeError("execution_environment_not_admitted")

        manager.set_on_run_from_idle(reject)

        with pytest.raises(
            RuntimeError,
            match="execution_environment_not_admitted",
        ):
            manager.run()

        assert attempted_run_ids == [manager.program_run_id]
        assert manager.program_run_id is not None
        assert manager.state == CycleState.IDLE
        assert (
            manager._tick_thread is None
            or not manager._tick_thread.is_alive()
        )

    def test_run_from_idle_after_reset_reuses_prepared_tree_and_scene(self, kernel):
        """Idle start after Reset uses the prepared baseline without rebuilding scene/tree again."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.reset()
        initial_tree = manager._tree
        initial_scene_edit_count = len(slow_kernel.scene_edits)

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert manager._tree is not None
        assert manager._tree is initial_tree
        assert len(slow_kernel.scene_edits) == initial_scene_edit_count

    def test_reset_clears_generation_scoped_runtime_faults(self, kernel):
        """Reset rebuilds the cycle and invalidates every old node fault handle."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        fault = runner.runtime_pause_registry.raise_fault(
            node_name="stale_motion",
            node_id="move_manipulator_action",
            domain="motion",
            reason_code="runtime_collision",
            message="stale collision",
        )

        manager.reset()

        assert runner.runtime_pause_registry.get(fault.fault_id) is None
        assert runner.runtime_pause_registry.active_faults() == ()
    
    def test_run_from_running_is_noop(self, kernel):
        """run() from RUNNING state is a no-op."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        # Call run again — should be no-op
        manager.run()
        assert manager.state == CycleState.RUNNING
    
    def test_run_from_stopped_consumes_exact_replan_offer(self, kernel):
        """A terminal stop resumes only through the current certified offer."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            # Run then stop
            manager.run()
            time.sleep(0.2)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED

            assessment = _script_ready_recovery(slow_kernel)
            reset_count = slow_kernel.reset_count
            result = manager.run(principal_id="operator-test")

            assert result is not None
            assert result["accepted"] is True
            assert manager.state == CycleState.RUNNING
            assert slow_kernel.reset_count == reset_count
            request = slow_kernel.apply_recovery_action_requests[-1]
            assert request["action"] == "replan_from_stopped_state"
            assert request["principal_id"] == "operator-test"
            assert (
                request["expected_assessment_revision"]
                == assessment["assessment_revision"]
            )
            assert request["expected_execution_id"] == assessment["execution_id"]
            assert (
                request["expected_stop_certificate_id"]
                == assessment["stop_certificate_id"]
            )
        finally:
            manager.reset()

    def test_structural_scene_change_rejects_rendered_offer_and_returns_new_one(
        self, kernel
    ):
        """A request cannot cross a structural scene-admission generation."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        try:
            manager.run()
            time.sleep(0.2)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED
            _script_ready_recovery(slow_kernel)
            rendered_offer = manager.refresh_recovery_offer()
            assert rendered_offer is not None
            action_events: list[tuple[str, dict]] = []
            manager.set_on_recovery_action_event(
                lambda action, details: action_events.append((action, details))
            )

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

            response = manager.apply_recovery_action(
                {
                    "request_id": "stale-scene-request",
                    "offer_id": rendered_offer["offerId"],
                    "offer_revision": rendered_offer["offerRevision"],
                    "action_id": rendered_offer["primaryAction"],
                },
                principal_id="operator-test",
            )

            assert response["accepted"] is False
            assert response["error"]["details"]["reason"] == "recovery_offer_stale"
            assert response["recoveryOffer"] is not None
            assert (
                response["recoveryOffer"]["sceneAdmissionEpoch"]
                > rendered_offer["sceneAdmissionEpoch"]
            )
            assert manager.state == CycleState.STOPPED
            assert not getattr(slow_kernel, "apply_recovery_action_requests", [])
            assert [action for action, _ in action_events] == [
                "requested",
                "rejected",
            ]
            for _action, details in action_events:
                assert details["requested_offer_id"] == rendered_offer["offerId"]
                assert (
                    details["requested_offer_revision"]
                    == rendered_offer["offerRevision"]
                )
                assert details["offer_id"] == response["recoveryOffer"]["offerId"]
                assert (
                    details["offer_revision"]
                    == response["recoveryOffer"]["offerRevision"]
                )
        finally:
            manager.reset()

    def test_reset_recovery_request_is_idempotent_across_response_retry(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.STOPPED
        manager._error = "cycle fault"
        manager._error_code = "cycle_fault"
        offer = manager.refresh_recovery_offer()
        assert offer is not None
        assert offer["primaryAction"] == "reset_cycle"
        callbacks: list[str] = []
        events: list[tuple[str, dict]] = []
        manager.set_on_recovery_action_event(
            lambda action, details: events.append((action, details))
        )
        request = {
            "request_id": "reset-idempotency-1",
            "offer_id": offer["offerId"],
            "offer_revision": offer["offerRevision"],
            "action_id": "reset_cycle",
        }

        first = manager.apply_recovery_action(
            request,
            principal_id="operator-test",
            reset_callback=callbacks.append,
        )
        replay = manager.apply_recovery_action(
            request,
            principal_id="operator-test",
            reset_callback=callbacks.append,
        )

        assert first == replay
        assert first["accepted"] is True
        assert callbacks == ["reset-idempotency-1"]
        assert [action for action, _ in events] == ["requested", "accepted"]

        conflict = manager.apply_recovery_action(
            {**request, "reason": "different payload"},
            principal_id="operator-test",
            reset_callback=callbacks.append,
        )
        assert conflict["accepted"] is False
        assert (
            conflict["error"]["details"]["reason"]
            == "recovery_request_id_reused"
        )
        assert callbacks == ["reset-idempotency-1"]
        assert [action for action, _ in events] == [
            "requested",
            "accepted",
            "requested",
            "rejected",
        ]

    def test_recovery_offer_projects_within_server_deadline_after_kernel_event(
        self, kernel
    ):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.STOPPED
        _script_ready_recovery(kernel)
        publication_times: list[float] = []
        manager.set_on_recovery_offer_published(
            lambda _offer: publication_times.append(time.monotonic())
        )
        waiter = threading.Thread(
            target=lambda: manager.wait_for_kernel_events_and_project(0.05),
        )
        waiter.start()
        time.sleep(0.005)
        emitted_at = time.monotonic()
        kernel._emit_kernel_event("recovery_assessment_changed")
        waiter.join(timeout=0.2)

        assert not waiter.is_alive()
        assert publication_times
        assert (publication_times[0] - emitted_at) < 0.050

    def test_concurrent_duplicate_recovery_request_never_repeats_reset(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.STOPPED
        manager._error = "cycle fault"
        manager._error_code = "cycle_fault"
        offer = manager.refresh_recovery_offer()
        request = {
            "request_id": "reset-singleflight-1",
            "offer_id": offer["offerId"],
            "offer_revision": offer["offerRevision"],
            "action_id": "reset_cycle",
        }
        entered = threading.Event()
        release = threading.Event()
        callback_count = 0
        first_result: list[dict] = []

        def blocked_reset(_request_id: str) -> None:
            nonlocal callback_count
            callback_count += 1
            entered.set()
            assert release.wait(timeout=1.0)

        first = threading.Thread(
            target=lambda: first_result.append(
                manager.apply_recovery_action(
                    request,
                    principal_id="operator-test",
                    reset_callback=blocked_reset,
                )
            )
        )
        first.start()
        assert entered.wait(timeout=1.0)
        duplicate = manager.apply_recovery_action(
            request,
            principal_id="operator-test",
            reset_callback=blocked_reset,
        )
        release.set()
        first.join(timeout=1.0)

        assert not first.is_alive()
        assert callback_count == 1
        assert duplicate["accepted"] is False
        assert duplicate["resultCode"] == "recovery_request_in_progress"
        assert first_result[0]["accepted"] is True

    def test_unexpected_recovery_exception_consumes_request_id(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        calls = 0

        def ambiguous_once(*_args, **_kwargs):
            nonlocal calls
            calls += 1
            raise RuntimeError("effect outcome unknown")

        manager._apply_recovery_action_once = ambiguous_once  # type: ignore[method-assign]
        request = {
            "request_id": "ambiguous-recovery-1",
            "offer_id": "offer-1",
            "offer_revision": 1,
            "action_id": "reset_cycle",
        }
        with pytest.raises(RuntimeError, match="effect outcome unknown"):
            manager.apply_recovery_action(
                request,
                principal_id="operator-test",
            )

        replay = manager.apply_recovery_action(
            request,
            principal_id="operator-test",
        )
        assert calls == 1
        assert replay["accepted"] is False
        assert replay["resultCode"] == "recovery_action_outcome_unknown"

    def test_run_from_held_runtime_fault_consumes_exact_cycle_offer(self, kernel):
        """Run can consume a certified automatic runtime-step offer only."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        try:
            manager.reset()
            fault = runner.runtime_pause_registry.raise_fault(
                node_name="signal_bridge",
                node_id="cycle_signal_condition",
                domain="signal",
                reason_code="runtime_signal_fault",
                message="signal provider unavailable",
                resume_owner="cycle_manager",
            )
            with manager._lock:
                manager._state = CycleState.RUNNING
                manager._motion_stop_state = "held"
                manager._error = fault.message
                manager._error_code = fault.reason_code

            result = manager.run(principal_id="operator-test")

            assert result is not None
            assert result["accepted"] is True
            assert result["resultCode"] == "accepted"
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert runner.runtime_pause_registry.get(fault.fault_id) is None
            assert not getattr(slow_kernel, "apply_recovery_action_requests", [])
        finally:
            manager.reset()

    def test_run_keeps_held_recovery_unchanged_while_offer_is_waiting(self):
        """Run cannot infer or execute an action from a waiting assessment."""
        retry_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        manager.reset()
        _script_ready_recovery(
            retry_kernel,
            action="retry_motion_from_current_state",
            recovery_class="runtime_collision",
            status="waiting",
            execution_state="interrupted",
        )
        with manager._lock:
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "runtime collision"
            manager._error_code = "runtime_collision"

        with pytest.raises(RuntimeError, match="status=waiting"):
            manager.run()

        assert retry_kernel.get_status().state == ExecutionState.INTERRUPTED
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "held"
        assert manager.error == "runtime collision"
        assert manager.error_code == "runtime_collision"
        assert not getattr(retry_kernel, "apply_recovery_action_requests", [])

    def test_run_requires_explicit_choice_for_intervention_offer(self):
        """Run cannot choose acknowledge versus reset for an intervention."""
        retry_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        manager.reset()
        _script_ready_recovery(
            retry_kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        with manager._lock:
            manager._state = CycleState.STOPPED
            manager._error = "controlled stop requires intervention"
            manager._error_code = "backend_preempt_failed"

        with pytest.raises(RuntimeError, match="status=action_required"):
            manager.run()

        assert retry_kernel.get_status().state == ExecutionState.INTERVENTION_REQUIRED
        assert manager.state == CycleState.STOPPED
        assert not getattr(retry_kernel, "apply_recovery_action_requests", [])

    def test_unoffered_recovery_action_has_no_kernel_side_effect(self):
        retry_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, retry_kernel)
        manager = CycleManager(runner, retry_kernel)
        manager.reset()
        _script_ready_recovery(
            retry_kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        with manager._lock:
            manager._state = CycleState.STOPPED
        offer = manager.refresh_recovery_offer()
        assert offer is not None

        result = manager.apply_recovery_action(
            {
                "request_id": "unoffered-action",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "retry_motion_from_current_state",
            },
            principal_id="operator-test",
        )

        assert result["accepted"] is False
        assert result["error"]["details"]["reason"] == "action_not_offered"
        assert manager.state == CycleState.STOPPED
        assert not getattr(retry_kernel, "apply_recovery_action_requests", [])

    def test_run_consumes_registry_fault_before_tick_loop_promotes_hold(self, kernel):
        """An exact node fault closes the pre-promotion successful-no-op race."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        manager.reset()
        fault = runner.runtime_pause_registry.raise_fault(
            node_name="move_to_pick_approach",
            node_id="move_manipulator_action",
            domain="motion",
            reason_code="motion_execution_failed",
            message="runtime collision",
        )
        with manager._lock:
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "idle"

        result = manager.run(principal_id="operator-test")

        assert result is not None
        assert result["accepted"] is True
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "idle"
        assert manager.error is None
        assert manager.error_code is None
        resumed_fault = runner.runtime_pause_registry.get(fault.fault_id)
        assert resumed_fault is not None
        assert resumed_fault.state == "resume_requested"
        assert resumed_fault.accepted_recovery_request_id == result["requestId"]
        # This test owns registry-fault admission, not shutdown latency. Use
        # shutdown's production budget because cleanup may traverse multiple
        # independently bounded control RPCs before joining the tick thread;
        # dedicated shutdown tests retain the tighter timing assertions.
        assert manager.shutdown() is True

    def test_node_failure_feedback_is_exposed_as_cycle_error(self, kernel, tmp_path):
        """Clean node failures should surface node feedback in manager.error."""
        failing_program = tmp_path / "missing_bb.program.yaml"
        failing_program.write_text(
            """api_version: manyforge.program.v0
program_profile: v0.0
name: missing_bb_failure
description: Motion node fails on missing blackboard key.
catalog_version: "0.5.0"

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
        motion_type: pose
        pose_goal:
          field_ref:
            namespace: missing_pose
            field: pose
        planning_timeout_s: 0.5
        execution_timeout_s: 0.5
""",
            encoding="utf-8",
        )
        runner = OrchestratorRunner(NODE_ENVIRONMENT, str(failing_program), kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        time.sleep(0.05)
        
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

    def test_execution_environment_loss_is_terminal_until_reset(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(
            NODE_ENVIRONMENT,
            FIXTURE_PATH,
            slow_kernel,
        )
        manager = CycleManager(runner, slow_kernel)
        manager.run()
        assert manager.state == CycleState.RUNNING

        manager.contain_execution_environment_loss(
            reason_code="execution_environment_component_epoch_changed:owner",
            details={"component_id": "owner"},
        )

        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "execution_environment_changed"
        assert manager.error_details == {
            "component_id": "owner",
            "reason": (
                "execution_environment_component_epoch_changed:owner"
            ),
            "requires_reset": True,
        }
        assert slow_kernel.stop_count == 1

    def test_environment_loss_retires_paused_kernel_from_stopped_cycle(
        self,
        kernel,
    ):
        """STOPPED is not proof that a retained resumable Kernel is harmless."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        with manager._lock:
            manager._state = CycleState.STOPPED
        kernel._state = ExecutionState.PAUSED

        manager.contain_execution_environment_loss(
            reason_code="execution_environment_authority_epoch_changed",
        )

        assert kernel.stop_count == 1
        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "execution_environment_changed"

    def test_retired_containment_generation_cannot_touch_cycle(
        self,
        kernel,
    ):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        contained = manager.contain_execution_environment_loss(
            reason_code="retired_environment",
            is_current=lambda: False,
        )

        assert contained is False
        assert manager.state == CycleState.IDLE
        assert manager.error is None
        assert kernel.stop_count == 0

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        assert manager.state == CycleState.IDLE
        manager.full_stop()
        assert manager.state == CycleState.IDLE
    
    def test_stop_routes_through_controlled_stop_not_pause(self, kernel):
        """Operator stop() routes the in-flight motion through the kernel's
        controlled stop (kernel.stop()), not a resumable pause. The controlled
        stop is the industrial default operator-stop path."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
            "catalog_version": "0.5.0",
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
                            "motion_type": "joint",
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
            manager.reset()

    def test_motion_stop_resume_redispatches_held_motion(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
            _script_ready_recovery(slow_kernel)

            manager.run()

            deadline = time.time() + 1.0
            while slow_kernel.execute_count == previous_execute_count and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert slow_kernel.execute_count > previous_execute_count
        finally:
            manager.reset()

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
            _script_ready_recovery(kernel)

            manager.run()

            deadline = time.time() + 1.0
            while kernel.execute_count == previous_execute_count and time.time() < deadline:
                time.sleep(0.01)
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert kernel.execute_count > previous_execute_count
            assert manager.error is None
        finally:
            manager.reset()

    def test_parallel_continue_branch_ticks_while_motion_is_held(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.2)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
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

    def test_persistent_runtime_fault_retains_bounded_tick_cadence(self):
        """A held runtime fault must not turn the 100 Hz tick into a busy loop."""
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=5.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        tick_count = 0
        tick_count_lock = threading.Lock()
        recovery_boundaries: list[str] = []
        original_refresh = manager._refresh_signal_registry

        def counted_refresh() -> None:
            nonlocal tick_count
            # Keep the probe deterministic: the old unbounded branch would
            # still execute hundreds of these calls in the observation window.
            time.sleep(0.0005)
            original_refresh()
            with tick_count_lock:
                tick_count += 1

        manager._refresh_signal_registry = counted_refresh
        manager._refresh_recovery_offer_best_effort = recovery_boundaries.append

        try:
            manager.run()
            deadline = time.monotonic() + 1.0
            while slow_kernel.execute_count == 0 and time.monotonic() < deadline:
                time.sleep(0.01)
            assert slow_kernel.execute_count == 1

            runner.runtime_pause_registry.raise_fault(
                node_name="runtime_collision_probe",
                node_id="move_manipulator_action",
                domain="motion",
                reason_code="runtime_collision",
                message="runtime collision",
                details={"body_id": "probe"},
            )
            deadline = time.monotonic() + 1.0
            while manager.motion_stop_state != "held" and time.monotonic() < deadline:
                time.sleep(0.005)
            assert manager.motion_stop_state == "held"

            with tick_count_lock:
                count_before = tick_count
            time.sleep(0.20)
            with tick_count_lock:
                ticks_while_held = tick_count - count_before

            # Loaded CI may delay an NRT thread, hence the permissive lower
            # bound. The upper bound distinguishes the intended cadence from
            # the former unbounded loop by a wide margin.
            assert 2 <= ticks_while_held <= 45
            assert recovery_boundaries.count("runtime fault") == 1

            runner.runtime_pause_registry.raise_fault(
                node_name="second_fault_probe",
                node_id="move_manipulator_action",
                domain="motion",
                reason_code="runtime_collision",
                message="second runtime collision",
            )
            deadline = time.monotonic() + 0.5
            while (
                recovery_boundaries.count("runtime fault") < 2
                and time.monotonic() < deadline
            ):
                time.sleep(0.005)
            assert recovery_boundaries.count("runtime fault") == 2
            time.sleep(0.05)
            assert recovery_boundaries.count("runtime fault") == 2
        finally:
            manager.reset()
            os.unlink(program_path)

    def test_motion_soft_fail_keeps_continue_branch_ticking(self):
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        program_path = self._parallel_motion_timer_program(timer_duration_s=0.3)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
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
            timer_duration_s=60.0,
            success_policy="success_on_one",
        )
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
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

            assert manager._tree is not None
            background_wait = next(
                child
                for child in manager._tree.root.children
                if child.name == "background_wait"
            )
            assert isinstance(background_wait, py_trees.timers.Timer)
            background_wait.finish_time = time.time() - 1.0

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, program_path, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        
        manager.run()
        assert manager.state == CycleState.RUNNING
        
        manager.reset()
        assert manager.state == CycleState.IDLE

    def test_reset_completion_callback_observes_fresh_idle_cycle(
        self,
        kernel,
    ):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        observations = []
        manager.set_on_reset_completed(
            lambda: observations.append(
                (
                    manager.state,
                    manager.error,
                    manager._tree is not None,
                )
            )
        )

        manager.reset()

        assert observations == [(CycleState.IDLE, None, True)]
    
    def test_reset_from_stopped(self, kernel):
        """reset() from STOPPED resets to IDLE."""
        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)
        
        manager.run()
        time.sleep(0.2)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        
        manager.reset()
        assert manager.state == CycleState.IDLE
        assert manager.error is None
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()

    def test_reset_quiesces_running_intervention_without_false_timeout(self, kernel):
        """Every tick target must signal the generation reset is waiting on."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        runner.load()

        session = manager.execute_intervention(
            {
                "id": "timer",
                "name": "reset_intervention_timer",
                "params": {"duration_s": 10.0},
            },
            session_kind="manual_intervention",
            authority_source="human",
            principal_id="human:test-operator",
            approval_mode="human_approved",
        )
        intervention_exit = manager._tick_exit_event

        manager.reset()

        assert session.status == "aborted"
        assert intervention_exit.is_set()
        assert manager.state == CycleState.IDLE
        assert manager.error_code != "tick_thread_quiesce_timeout"
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()

    def test_reset_waits_for_inflight_nrt_tick_before_claiming_idle(self, kernel):
        """A bounded scene/adapter call may outlive 25 ms without being a zombie."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.reset()
        assert manager._tree is not None

        tick_entered = threading.Event()
        original_tick = manager._tree.tick

        def delayed_tick(*args, **kwargs):
            tick_entered.set()
            time.sleep(0.08)
            return original_tick(*args, **kwargs)

        manager._tree.tick = delayed_tick
        manager.run()
        assert tick_entered.wait(timeout=1.0)

        started = time.monotonic()
        manager.reset()
        elapsed_s = time.monotonic() - started

        assert elapsed_s >= 0.05
        assert manager.state == CycleState.IDLE
        assert manager.error_code != "tick_thread_quiesce_timeout"
        assert manager._tick_thread is None or not manager._tick_thread.is_alive()
        assert manager._tick_exit_event.is_set()

    def test_reset_timeout_never_claims_idle_with_live_tick(self, kernel, monkeypatch):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.reset()
        assert manager._tree is not None

        tick_entered = threading.Event()
        release_tick = threading.Event()

        def blocked_tick(*_args, **_kwargs):
            tick_entered.set()
            release_tick.wait(timeout=1.0)

        manager._tree.tick = blocked_tick
        monkeypatch.setattr(cycle_manager_module, "_TICK_JOIN_DEADLINE_S", 0.05)
        manager.run()
        assert tick_entered.wait(timeout=1.0)

        try:
            manager.reset()
            assert manager.state == CycleState.STOPPED
            assert manager.error_code == "tick_thread_quiesce_timeout"
            assert manager._tick_thread is not None
            assert manager._tick_thread.is_alive()
        finally:
            release_tick.set()
            assert manager._tick_exit_event.wait(timeout=1.0)
    
    def test_reset_from_idle_is_noop(self, kernel):
        """reset() from IDLE prepares a fresh software/runtime cycle state."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, refused_kernel)
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

        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        kernel._execution_id = 1

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        kernel._execution_id = 1

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

    def test_event_harvest_never_reuses_collision_from_older_execution(
        self, kernel
    ):
        """A rich collision from execution 2 must not mask execution 5's
        lookahead deadline error after the object has already disappeared."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        kernel._execution_id = 5

        with manager._lock:
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "collision_detected",
                "execution_id": 2,
                "timestamp_ns": 1_000,
                "event_index": manager._kernel_event_index,
                "details": {
                    "collision_result_first_failure_pair":
                        "object/removed_wall <-> robot/wrist_1_link",
                    "reason_code": "runtime_collision_monitor",
                },
            })
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "error_raised",
                "execution_id": 5,
                "timestamp_ns": 2_000,
                "event_index": manager._kernel_event_index,
                "details": {
                    "program_run_id": "run-current",
                    "node_instance_id": "move_to_drop_approach",
                },
                "error": {
                    "code": "runtime_lookahead_deadline_exceeded",
                    "message": "Runtime collision lookahead exceeded its deadline",
                    "details": {
                        "reason": "runtime_lookahead_deadline_exceeded",
                        "cycle_duration_ns": "250474540",
                        "worker_active_ns": "159705599",
                    },
                },
            })
            manager._kernel_event_index += 1
            manager._kernel_events.append({
                "type": "interrupt_latched",
                "execution_id": 5,
                "timestamp_ns": 3_000,
                "event_index": manager._kernel_event_index,
                "details": {
                    "latch_id": "interrupt-5",
                    "source_id": "manyforge.kernel.runtime_fault",
                },
            })

        code, details = manager._capture_kernel_error_diagnostics()

        assert code == "runtime_lookahead_deadline_exceeded"
        assert details is not None
        assert details["worker_active_ns"] == "159705599"
        assert details["node_instance_id"] == "move_to_drop_approach"
        assert "collision_result_first_failure_pair" not in details

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, fault_kernel)
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

        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
            manager._error_code = "stale_test_error"

        # A terminal cycle error is reset-only; explicitly consume that action.
        offer = manager.refresh_recovery_offer()
        result = manager.apply_recovery_action(
            {
                "request_id": "clear-stale-error",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "reset_cycle",
            },
            principal_id="operator:test",
        )
        assert result["accepted"] is True
        assert manager.error is None


# =========================================================================
# PRD-4 Qualification: Zombie thread detection (Item 8)
# =========================================================================

class TestZombieThread:
    """Verify fail-closed guards for a tick that cannot quiesce promptly."""

    def test_run_refused_while_zombie_thread_alive(self, kernel):
        """run() raises RuntimeError if the previous tick thread is still alive."""
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        assert entered_tick.wait(timeout=2.0)

        # Reset must remain bounded without claiming the old tree is gone.
        manager.reset()

        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "tick_thread_quiesce_timeout"
        assert manager.error == "behavior tree did not quiesce before reset deadline"
        old_tick_thread = manager._tick_thread
        assert old_tick_thread is not None and old_tick_thread.is_alive()

        # Run may surface the exact reset-only recovery offer rather than the
        # lower-level thread guard, but it must never start a replacement tree.
        with pytest.raises(RuntimeError):
            manager.run()
        assert manager.state == CycleState.STOPPED
        assert manager._tick_thread is old_tick_thread

        # Clean up — unblock the thread so it can exit
        block_event.set()
        assert manager._tick_exit_event.wait(timeout=1.0)

    def test_reset_detects_zombie_and_sets_error(self, kernel):
        """reset() sets error when tick thread does not exit within timeout."""
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        assert entered_tick.wait(timeout=2.0)

        manager.reset()

        assert manager.state == CycleState.STOPPED
        assert manager.error_code == "tick_thread_quiesce_timeout"
        assert manager.error == "behavior tree did not quiesce before reset deadline"
        assert manager._tick_thread is not None
        assert manager._tick_thread.is_alive()

        # Once the old tree really exits, a new Reset can safely rebuild and
        # publish IDLE. The cleanup watcher must not leave stale ownership.
        block_event.set()
        assert manager._tick_exit_event.wait(timeout=1.0)
        deadline = time.monotonic() + 1.0
        while manager._tick_thread is not None and time.monotonic() < deadline:
            time.sleep(0.005)
        assert manager._tick_thread is None
        manager.reset()
        assert manager.state == CycleState.IDLE
        assert manager.error is None


# =========================================================================
# PRD-4 Qualification: Concurrent rapid cycles (Item 4)
# =========================================================================

class TestConcurrentRapidCycles:
    """Verify stability under rapid run/stop/reset sequences."""

    def test_rapid_run_stop_reset_cycles(self, kernel):
        """20 rapid run/stop/reset cycles must not crash or deadlock."""
        slow_kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

    def test_completed_intervention_offer_binds_post_intervention_scene_epoch(
        self, kernel
    ):
        """Recovery after direct control binds the current structural scene."""

        slow_kernel = MockKernelClient(execute_duration_s=1.0)
        manager = self._make_manager(slow_kernel)

        try:
            manager.run()
            time.sleep(0.05)
            manager.full_stop()
            assert manager.state == CycleState.STOPPED
            previous_scene_epoch = slow_kernel.get_scene_authority_status()[
                "scene_admission_epoch"
            ]

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
            current_scene_epoch = slow_kernel.get_scene_authority_status()[
                "scene_admission_epoch"
            ]
            assert current_scene_epoch > previous_scene_epoch

            _script_ready_recovery(slow_kernel)
            offer = manager.refresh_recovery_offer()
            assert offer["sceneAdmissionEpoch"] == current_scene_epoch
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
            motion_type="joint",
            joint_goal_names=["joint1"],
            joint_goal_positions_rad=[0.0],
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
            motion_type="joint",
            joint_goal_names=["joint1"],
            joint_goal_positions_rad=[0.0],
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
            motion_type="joint",
            joint_goal_names=["joint1"],
            joint_goal_positions_rad=[0.0],
        )
        kernel.execute(req)
        first = kernel.drain_kernel_events()
        assert len(first) > 0
        second = kernel.drain_kernel_events()
        assert len(second) == 0

    def test_cycle_manager_collects_kernel_events(self, kernel):
        """CycleManager collects kernel events during execution."""
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        from manyforge_behavior.kernel_client import ExecuteRequest
        req = ExecuteRequest(
            robot_id="r",
            group_id="g",
            move_id="direct",
            base_frame="base_link",
            tcp_frame="tool0",
            motion_type="joint",
            joint_goal_names=["joint1"],
            joint_goal_positions_rad=[0.0],
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
            "controller_binding_lost",
            "execution_admission_lost",
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


class TestCycleManagerRecoveryActions:
    """One exact recovery-offer endpoint (spec/400 section 6.6.2)."""

    def _make_manager(self) -> tuple[CycleManager, MockKernelClient]:
        kernel = MockKernelClient(execute_duration_s=0.5)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        return manager, kernel

    @staticmethod
    def _assessment_refresh_manager(
        *,
        change_stop_certificate: bool = False,
    ) -> tuple[CycleManager, MockKernelClient, dict]:
        class AssessmentRefreshKernel(MockKernelClient):
            def apply_recovery_action(self, request):  # type: ignore[override]
                self.apply_recovery_action_requests = getattr(
                    self, "apply_recovery_action_requests", []
                )
                self.apply_recovery_action_requests.append(dict(request))
                current = dict(self.get_recovery_assessment())
                if len(self.apply_recovery_action_requests) == 1:
                    refreshed = dict(current)
                    refreshed.update(
                        {
                            "assessment_revision": (
                                int(current["assessment_revision"]) + 1
                            ),
                            "observed_state_sequence": (
                                int(current["observed_state_sequence"]) + 1
                            ),
                            "observed_state_valid_until_ns": (
                                time.monotonic_ns() + 5_000_000_000
                            ),
                        }
                    )
                    if change_stop_certificate:
                        refreshed["stop_certificate_id"] = "replacement-cert"
                    self.script_recovery_assessment(refreshed)
                    return {
                        "request_id": str(request.get("request_id") or ""),
                        "action": str(request.get("action") or ""),
                        "accepted": False,
                        "error": {
                            "code": "recovery_action_rejected",
                            "message": "Recovery action rejected",
                            "details": {"reason": "assessment_stale"},
                        },
                        "resulting_assessment": refreshed,
                    }

                resulting = dict(current)
                resulting.update(
                    {
                        "assessment_revision": (
                            int(current["assessment_revision"]) + 1
                        ),
                        "execution_state": "idle",
                        "recovery_class": "none",
                        "status": "unavailable",
                        "latch_id": "",
                        "stop_certificate_id": "",
                        "retry_admissible": False,
                        "allowed_actions": [],
                        "reason_code": "no_recovery_active",
                        "evidence_refs": [],
                    }
                )
                self.script_recovery_assessment(resulting)
                self._state = ExecutionState.IDLE  # noqa: SLF001
                return {
                    "request_id": str(request.get("request_id") or ""),
                    "action": str(request.get("action") or ""),
                    "accepted": True,
                    "error": None,
                    "resulting_assessment": resulting,
                }

        kernel = AssessmentRefreshKernel(execute_duration_s=0.5)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable fixture.
        with manager._lock:  # noqa: SLF001 - exact held recovery fixture.
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
        _script_ready_recovery(kernel)
        offer = manager.refresh_recovery_offer()
        return manager, kernel, offer

    def test_equivalent_assessment_refresh_retries_one_operator_action_once(
        self,
    ):
        manager, kernel, offer = self._assessment_refresh_manager()
        request = {
            "request_id": "replan-after-sample-refresh",
            "offer_id": offer["offerId"],
            "offer_revision": offer["offerRevision"],
            "action_id": "replan_from_stopped_state",
        }
        try:
            result = manager.apply_recovery_action(
                request,
                principal_id="operator:test",
            )

            assert result["accepted"] is True
            assert result["cycleResumed"] is True
            calls = kernel.apply_recovery_action_requests
            assert len(calls) == 2
            assert calls[0]["request_id"] == calls[1]["request_id"]
            assert calls[0]["expected_assessment_revision"] == 11
            assert calls[1]["expected_assessment_revision"] == 12

            replay = manager.apply_recovery_action(
                request,
                principal_id="operator:test",
            )
            assert replay == result
            assert len(kernel.apply_recovery_action_requests) == 2
        finally:
            manager.reset()

    def test_assessment_refresh_identity_change_is_not_retried(self):
        manager, kernel, offer = self._assessment_refresh_manager(
            change_stop_certificate=True
        )

        result = manager.apply_recovery_action(
            {
                "request_id": "replan-after-certificate-change",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "replan_from_stopped_state",
            },
            principal_id="operator:test",
        )

        assert result["accepted"] is False
        assert result["resultCode"] == "kernel_rejected"
        assert (
            result["error"]["details"]["reason"]
            == "assessment_stale"
        )
        assert len(kernel.apply_recovery_action_requests) == 1

    def test_run_from_stopped_with_kernel_intervention_requires_explicit_recovery(
        self,
    ):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )

        with pytest.raises(RuntimeError, match="status=action_required"):
            manager.run()
        assert manager.state == CycleState.STOPPED
        assert kernel.get_status().state == ExecutionState.INTERVENTION_REQUIRED
        assert not getattr(kernel, "apply_recovery_action_requests", [])

    def test_acknowledge_and_continue_preserves_backend_reset_count(self):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        assessment = _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        offer = manager.refresh_recovery_offer()
        result = manager.apply_recovery_action(
            {
                "request_id": "ack-intervention",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "acknowledge_and_continue",
                "reason": "controlled stop inspected",
            },
            principal_id="operator:test",
        )

        assert result["accepted"] is True
        assert result["cycleResumed"] is False
        assert result["resultCode"] == "accepted_resume_ready"
        assert manager.state == CycleState.STOPPED
        assert manager.motion_stop_state == "held"
        assert not manager._tick_pause_event.is_set()
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.reset_count == 0
        request = kernel.apply_recovery_action_requests[-1]
        assert request["action"] == "acknowledge_and_continue"
        assert request["principal_id"] == "operator:test"
        assert request["expected_latch_id"] == assessment["latch_id"]
        assert (
            request["expected_stop_owner_generation"]
            == assessment["stop_owner_generation"]
        )

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "idle"
        assert manager._tick_pause_event.is_set()

    def test_acknowledge_and_continue_accepts_held_motion_intervention(self):
        manager, kernel = self._make_manager()
        manager.reset()
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "Runtime collision detected during execution"
            manager._error_code = "runtime_collision"
            manager._error_details = {
                "motion_soft_stop_reason": "motion_soft_fail_requires_intervention",
                "kernel_state": "intervention_required",
            }
        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        offer = manager.refresh_recovery_offer()
        before_reset_count = kernel.reset_count

        result = manager.apply_recovery_action(
            {
                "request_id": "ack-held-intervention",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "acknowledge_and_continue",
            },
            principal_id="operator:test",
        )

        assert result["accepted"] is True
        assert result["cycleResumed"] is False
        assert result["resultCode"] == "accepted_resume_ready"
        assert manager.state == CycleState.STOPPED
        assert manager.motion_stop_state == "held"
        assert not manager._tick_pause_event.is_set()
        assert manager.error is None
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.reset_count == before_reset_count

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "idle"
        assert manager._tick_pause_event.is_set()

    def test_retry_motion_from_current_state_consumes_exact_collision_offer(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable tree fixture.
        with manager._lock:  # noqa: SLF001 - exact held-motion fixture.
            manager._state = CycleState.RUNNING
            manager._motion_stop_state = "held"
            manager._error = "Runtime collision detected during execution"
            manager._error_code = "runtime_collision"
        assessment = _script_ready_recovery(
            kernel,
            action="retry_motion_from_current_state",
            recovery_class="runtime_collision",
            status="retry_ready",
            execution_state="interrupted",
        )
        offer = manager.refresh_recovery_offer()
        before_reset_count = kernel.reset_count
        try:
            result = manager.apply_recovery_action(
                {
                    "request_id": "retry-runtime-collision",
                    "offer_id": offer["offerId"],
                    "offer_revision": offer["offerRevision"],
                    "action_id": "retry_motion_from_current_state",
                },
                principal_id="operator:test",
            )

            assert result["accepted"] is True
            assert result["cycleResumed"] is True
            assert manager.state == CycleState.RUNNING
            assert manager.motion_stop_state == "idle"
            assert kernel.reset_count == before_reset_count
            request = kernel.apply_recovery_action_requests[-1]
            assert request["action"] == "retry_motion_from_current_state"
            assert request["expected_execution_id"] == assessment["execution_id"]
            assert request["expected_latch_id"] == assessment["latch_id"]
        finally:
            manager.reset()

    def test_retry_after_scene_repair_preserves_backend_reset_count(self):
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
        _script_ready_recovery(
            kernel,
            action="retry_after_scene_repair",
            recovery_class="failed_validation",
            execution_state="failed",
        )
        offer = manager.refresh_recovery_offer()
        before_reset_count = kernel.reset_count

        result = manager.apply_recovery_action(
            {
                "request_id": "retry-validation",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "retry_after_scene_repair",
            },
            principal_id="operator:test",
        )

        assert result["accepted"] is True
        assert manager.state == CycleState.RUNNING
        assert manager.motion_stop_state == "idle"
        assert manager.error is None
        assert kernel.get_status().state == ExecutionState.IDLE
        assert kernel.reset_count == before_reset_count

    def test_retry_after_scene_repair_is_rejected_when_not_offered(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable test context.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._error = "Backend failed"
            manager._error_code = "execution_aborted"
            manager._error_details = {"reason": "controller_result_failed"}
        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="unknown_fault",
            status="action_required",
            execution_state="failed",
        )
        offer = manager.refresh_recovery_offer()
        result = manager.apply_recovery_action(
            {
                "request_id": "invalid-scene-retry",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "retry_after_scene_repair",
            },
            principal_id="operator:test",
        )

        assert result["accepted"] is False
        assert result["error"]["details"]["reason"] == "action_not_offered"
        assert kernel.get_status().state == ExecutionState.FAILED
        assert kernel.reset_count == 0
        assert not getattr(kernel, "apply_recovery_action_requests", [])

    def test_reset_action_uses_explicit_callback_without_kernel_recovery_call(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - build a resumable tree.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._error = "Observed scene source stale"
            manager._error_code = "scene_changed_during_validation"
            manager._error_details = {"reason": "observed_scene_source_stale"}
        _script_ready_recovery(
            kernel,
            action="retry_after_scene_repair",
            recovery_class="failed_validation",
            execution_state="failed",
        )
        offer = manager.refresh_recovery_offer()
        callback_requests = []
        result = manager.apply_recovery_action(
            {
                "request_id": "reset-recovery",
                "offer_id": offer["offerId"],
                "offer_revision": offer["offerRevision"],
                "action_id": "reset_cycle",
            },
            principal_id="operator:test",
            reset_callback=callback_requests.append,
        )

        assert result["accepted"] is True
        assert result["resultCode"] == "reset_completed"
        assert callback_requests == ["reset-recovery"]
        assert not getattr(kernel, "apply_recovery_action_requests", [])

    def test_active_intervention_blocks_continue_but_keeps_reset_available(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - build a resumable tree.
        with manager._lock:  # noqa: SLF001 - targeted recovery-state fixture.
            manager._state = CycleState.STOPPED
            manager._intervention = InterventionSession(
                session_id="active-intervention",
                parent_run_id=None,
                entry_state="STOPPED",
                lease_timeout_ms=60_000,
                lease_expires_at_ns=10**18,
                resume_target="resume",
                principal_id="operator:test",
            )
            manager._intervention.status = "running"
        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        offer = manager.refresh_recovery_offer()

        actions = {action["actionId"]: action for action in offer["actions"]}
        assert actions["acknowledge_and_continue"]["enabled"] is False
        assert (
            actions["acknowledge_and_continue"]["disabledReasonCode"]
            == "stronger_cycle_authority_active"
        )
        assert actions["reset_cycle"]["enabled"] is True

    def test_pending_lifecycle_command_temporarily_disables_all_actions(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable fixture.
        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        with manager._lock:
            manager._state = CycleState.STOPPED
            manager._run_transition_in_progress = True

        offer = manager.refresh_recovery_offer()

        assert offer["status"] == "waiting"
        assert offer["reasonCode"] == "cycle_lifecycle_command_pending"
        assert all(action["enabled"] is False for action in offer["actions"])

    def test_unsettled_stop_offer_exposes_reset_only_and_run_does_not_choose(self):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED

        assessment = _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="waiting",
            execution_state="intervention_required",
        )
        assessment["allowed_actions"] = ["reset_cycle"]
        assessment["reason_code"] = "stop_not_settled_measured_velocity"
        kernel.script_recovery_assessment(assessment)

        with pytest.raises(RuntimeError, match="stop_not_settled_measured_velocity"):
            manager.run()
        offer = manager.get_recovery_offer()

        assert offer["primaryAction"] is None
        assert [action["actionId"] for action in offer["actions"]] == [
            "reset_cycle"
        ]
        assert manager.state == CycleState.STOPPED
        assert kernel.get_status().state == ExecutionState.INTERVENTION_REQUIRED

    def test_plain_paused_lifecycle_resume_bypasses_recovery_endpoint(self):
        manager, kernel = self._make_manager()
        manager.run()
        time.sleep(0.1)
        manager.full_stop()
        assert manager.state == CycleState.STOPPED
        kernel._state = ExecutionState.PAUSED

        manager.run()

        assert manager.state == CycleState.RUNNING
        assert kernel.resume_count == 1
        assert not getattr(kernel, "apply_recovery_action_requests", [])

    def test_unreadable_kernel_assessment_fails_closed_to_reset_only(self):
        manager, kernel = self._make_manager()
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable fixture.
        with manager._lock:
            manager._state = CycleState.STOPPED

        def boom():
            raise RuntimeError("kernel went away")

        kernel.get_recovery_assessment = boom  # type: ignore[assignment]
        offer = manager.refresh_recovery_offer()

        assert offer["status"] == "waiting"
        assert offer["reasonCode"] == "recovery_assessment_unreadable:RuntimeError"
        assert [action["actionId"] for action in offer["actions"]] == [
            "reset_cycle"
        ]


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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        self._write_fault({"reason_code": "not_a_real_token", "gripper_id": "g"})

        code, details = manager._capture_kernel_error_diagnostics()
        assert code is None
        assert details == {"reason_code": "not_a_real_token", "gripper_id": "g"}

    def test_snapshot_error_wins_over_gripper_record(self, kernel):
        from types import SimpleNamespace

        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        time.sleep(0.05)
        self._inject_tick_exception(manager)
        self._await_state(manager, CycleState.STOPPED)

        # The post-exception tree cannot be trusted for a resume. The unified
        # offer must expose reset-only recovery rather than an absent offer.
        with pytest.raises(RuntimeError, match="cycle_error_requires_reset"):
            manager.run()
        assert manager.state == CycleState.STOPPED
        offer = manager.get_recovery_offer()
        assert offer["status"] == "reset_required"
        assert [action["actionId"] for action in offer["actions"]] == [
            "reset_cycle"
        ]
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

    def test_blocked_recovery_assessment_does_not_block_full_stop(self):
        release_assessment = threading.Event()
        assessment_entered = threading.Event()

        class BlockingAssessmentKernel(MockKernelClient):
            block_assessment = False

            def get_recovery_assessment(self):  # type: ignore[override]
                if self.block_assessment:
                    assessment_entered.set()
                    release_assessment.wait(timeout=5.0)
                return super().get_recovery_assessment()

        kernel = BlockingAssessmentKernel()
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable fixture.
        with manager._lock:
            manager._state = CycleState.STOPPED
        _script_ready_recovery(kernel)
        offer = manager.refresh_recovery_offer()
        kernel.script_recovery_action_result(
            {
                "accepted": False,
                "error": {
                    "code": "recovery_action_rejected",
                    "message": "superseded by full stop",
                    "details": {"reason": "control_epoch_mismatch"},
                },
                "resulting_assessment": kernel.get_recovery_assessment(),
            }
        )
        kernel.block_assessment = True
        responses = []

        def recover() -> None:
            responses.append(
                manager.apply_recovery_action(
                    {
                        "request_id": "blocked-assessment",
                        "offer_id": offer["offerId"],
                        "offer_revision": offer["offerRevision"],
                        "action_id": offer["primaryAction"],
                    },
                    principal_id="operator:test",
                )
            )

        thread = threading.Thread(target=recover, daemon=True)
        thread.start()
        try:
            assert assessment_entered.wait(timeout=1.0)
            started = time.monotonic()
            assert manager.motion_stop_state == "idle"
            manager.full_stop()
            assert time.monotonic() - started < 0.2
        finally:
            release_assessment.set()
            thread.join(timeout=1.0)
        assert not thread.is_alive()
        assert responses and responses[0]["accepted"] is False

    def test_blocked_recovery_command_times_out_without_blocking_full_stop(
        self, monkeypatch
    ):
        release_command = threading.Event()
        command_entered = threading.Event()

        class BlockingRecoveryKernel(MockKernelClient):
            def apply_recovery_action(self, request):  # type: ignore[override]
                command_entered.set()
                release_command.wait(timeout=5.0)
                if int(request.get("expected_control_command_epoch") or -1) != (
                    self.control_command_epoch()
                ):
                    return {
                        "accepted": False,
                        "error": {
                            "code": "recovery_action_rejected",
                            "message": "stale recovery command",
                            "details": {"reason": "control_epoch_mismatch"},
                        },
                        "resulting_assessment": self.get_recovery_assessment(),
                    }
                return super().apply_recovery_action(request)

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = BlockingRecoveryKernel()
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager._prepare_fresh_cycle()  # noqa: SLF001 - resumable fixture.
        with manager._lock:
            manager._state = CycleState.STOPPED
        _script_ready_recovery(
            kernel,
            action="acknowledge_and_continue",
            recovery_class="stop_fault_intervention",
            status="action_required",
            execution_state="intervention_required",
        )
        offer = manager.refresh_recovery_offer()
        responses = []

        def recover() -> None:
            responses.append(
                manager.apply_recovery_action(
                    {
                        "request_id": "blocked-command",
                        "offer_id": offer["offerId"],
                        "offer_revision": offer["offerRevision"],
                        "action_id": "acknowledge_and_continue",
                    },
                    principal_id="operator:test",
                )
            )

        thread = threading.Thread(target=recover, daemon=True)
        thread.start()
        try:
            assert command_entered.wait(timeout=1.0)
            started = time.monotonic()
            assert manager.motion_stop_state == "idle"
            manager.full_stop()
            assert time.monotonic() - started < 0.2
            thread.join(timeout=1.0)
            assert responses and responses[0]["resultCode"] == "transport_failed"
        finally:
            release_command.set()
            thread.join(timeout=1.0)
        assert not thread.is_alive()

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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

    def test_full_stop_never_reads_scene_objects(self):
        scene_reads = 0

        class BlockingSceneKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.reject_scene_reads = False

            def get_scene_objects(self):  # type: ignore[override]
                nonlocal scene_reads
                scene_reads += 1
                if self.reject_scene_reads:
                    raise AssertionError("full_stop must not snapshot scene objects")
                return super().get_scene_objects()

        kernel = BlockingSceneKernel()
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        manager.run()
        deadline = time.monotonic() + 1.0
        while kernel.execute_count == 0 and time.monotonic() < deadline:
            time.sleep(0.01)
        baseline_reads = scene_reads
        kernel.reject_scene_reads = True

        started = time.monotonic()
        manager.full_stop()

        assert time.monotonic() - started < 0.5
        assert manager.state == CycleState.STOPPED
        assert scene_reads == baseline_reads

    def test_stop_settle_watcher_does_not_read_scene_objects(self, monkeypatch):
        scene_read = threading.Event()

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
                scene_read.set()
                return super().get_scene_objects()

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = TerminalThenBlockingSceneKernel()
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._state = CycleState.RUNNING
        manager._tree = runner.build_tree()

        manager.full_stop()
        assert manager.state == CycleState.STOPPING
        self._await_state(manager, CycleState.STOPPED, timeout_s=0.5)
        assert not scene_read.is_set()

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


class TestRuntimePauseReadAllocation:
    """Runtime-fault observation must not grow py_trees global state."""

    def test_runtime_fault_reads_do_not_register_blackboard_clients(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        runner.runtime_pause_registry.raise_fault(
            node_name="allocation_probe",
            node_id="test",
            domain="test",
            reason_code="test_fault",
            message="test fault",
        )

        manager._active_runtime_fault_facts()
        clients_before = len(py_trees.blackboard.Blackboard.clients)
        for _ in range(200):
            manager._active_runtime_fault_facts()
        assert len(py_trees.blackboard.Blackboard.clients) == clients_before

    def test_active_fault_snapshot_is_revision_coherent_and_defensive(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        registry = runner.runtime_pause_registry

        initial_revision, initial_faults = registry.active_fault_snapshot()
        assert initial_faults == ()

        fault = registry.raise_fault(
            node_name="snapshot_probe",
            node_id="test",
            domain="test",
            reason_code="test_fault",
            message="test fault",
            details={"nested": {"value": 1}},
        )
        active_revision, active_faults = registry.active_fault_snapshot()
        assert active_revision > initial_revision
        assert [item.fault_id for item in active_faults] == [fault.fault_id]

        active_faults[0].details["nested"]["value"] = 99
        stored = registry.get(fault.fault_id)
        assert stored is not None
        assert stored.details == {"nested": {"value": 1}}

        assert registry.request_resume(
            (fault.fault_id,),
            recovery_request_id="snapshot-probe-resume",
        )
        resumed_revision, resumed_faults = registry.active_fault_snapshot()
        assert resumed_revision > active_revision
        assert resumed_faults[0].state == "resume_requested"


class TestCycleShutdown:
    """Process-shutdown quiesce: motion stopped, tick thread joined."""

    def test_shutdown_stops_running_cycle_and_joins_tick_thread(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        manager.run()
        time.sleep(0.05)
        tick_thread = manager._tick_thread

        # Shutdown steps are independent: a failing kernel stop must not
        # prevent the tick thread from being signalled and joined.
        assert manager.shutdown(timeout_s=2.0) is True
        assert tick_thread is None or not tick_thread.is_alive()

    def test_shutdown_from_idle_is_safe(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)
        assert manager.shutdown(timeout_s=1.0) is True


class TestProgramRunIdThreading:
    """spec/500 §2.2: the runner mints a program_run_id once per program
    start; motion nodes attach it to every ExecuteRequest and the kernel
    stamps it on every kernel event, so history joins on ids, not on
    timestamp guesswork."""

    def test_run_mints_id_and_threads_it_to_kernel_and_events(self):
        slow_kernel = MockKernelClient(execute_duration_s=5.0)
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, slow_kernel)
        manager = CycleManager(runner, slow_kernel)

        manager.run()
        first = manager._program_run_id
        assert first
        time.sleep(0.05)

        manager.full_stop()
        deadline = time.time() + 2.0
        while manager.state != CycleState.STOPPED and time.time() < deadline:
            time.sleep(0.01)
        _script_ready_recovery(slow_kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._tree = runner.build_tree()  # noqa: SLF001
        manager._state = CycleState.STOPPED  # noqa: SLF001

        with pytest.raises(TimeoutError):
            manager.run()

        assert resume_entered.is_set()
        assert kernel.control_command_epoch() == 1
        release_resume.set()
        assert resume_done.wait(timeout=1.0)
        assert kernel.resume_count == 0
        assert kernel.get_status().state == ExecutionState.PAUSED
        assert manager.state == CycleState.STOPPED

    def test_recovery_action_during_wedged_run_transition_is_bounded(
        self, monkeypatch
    ):
        release_status = threading.Event()
        status_entered = threading.Event()

        class FirstRunStatusBlocksKernel(MockKernelClient):
            def __init__(self):
                super().__init__(execute_duration_s=5.0)
                self.block_status = True

            def get_status(self):  # type: ignore[override]
                if self.block_status:
                    status_entered.set()
                    release_status.wait(timeout=5.0)
                return super().get_status()

        monkeypatch.setattr(
            cycle_manager_module,
            "_KERNEL_CONTROL_CALL_TIMEOUT_S",
            0.02,
        )
        kernel = FirstRunStatusBlocksKernel()
        kernel._state = ExecutionState.PAUSED  # noqa: SLF001
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        runner.load()
        manager = CycleManager(runner, kernel)
        manager._tree = runner.build_tree()  # noqa: SLF001
        manager._state = CycleState.STOPPED  # noqa: SLF001

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
            kernel.block_status = False
            _script_ready_recovery(
                kernel,
                action="acknowledge_and_continue",
                recovery_class="stop_fault_intervention",
                status="action_required",
                execution_state="intervention_required",
            )
            offer = manager.refresh_recovery_offer()
            recovery_responses: list[dict] = []
            recovery_elapsed: list[float] = []

            def recover() -> None:
                started = time.monotonic()
                recovery_responses.append(
                    manager.apply_recovery_action(
                        {
                            "request_id": "recovery-after-wedged-run",
                            "offer_id": offer["offerId"],
                            "offer_revision": offer["offerRevision"],
                            "action_id": "acknowledge_and_continue",
                        },
                        principal_id="operator:test",
                    )
                )
                recovery_elapsed.append(time.monotonic() - started)

            recovery_thread = threading.Thread(target=recover, daemon=True)
            recovery_thread.start()
            recovery_thread.join(timeout=0.5)
            assert not recovery_thread.is_alive()
            assert recovery_responses[0]["accepted"] is False
            assert (
                recovery_responses[0]["error"]["details"]["reason"]
                == "recovery_offer_stale"
            )
            assert recovery_elapsed and recovery_elapsed[0] < 0.3
            run_thread.join(timeout=1.0)
            assert not run_thread.is_alive()
            assert any(isinstance(exc, TimeoutError) for exc in run_errors)

            current = recovery_responses[0]["recoveryOffer"]
            accepted = manager.apply_recovery_action(
                {
                    "request_id": "recovery-after-stale-refresh",
                    "offer_id": current["offerId"],
                    "offer_revision": current["offerRevision"],
                    "action_id": "acknowledge_and_continue",
                },
                principal_id="operator:test",
            )
            assert accepted["accepted"] is True
            assert len(kernel.apply_recovery_action_requests) == 1
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
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

    def test_sequence_high_water_does_not_regress_on_reordered_delivery(self, kernel):
        runner = OrchestratorRunner(NODE_ENVIRONMENT, FIXTURE_PATH, kernel)
        manager = CycleManager(runner, kernel)

        with kernel._kernel_events_lock:
            kernel._kernel_events.extend(
                [
                    {"type": "state_transition", "sequence": 1260},
                    {
                        "type": "motion_execution_outcome_recorded",
                        "sequence": 1261,
                    },
                    {"type": "recovery_assessment_changed", "sequence": 1259},
                ]
            )
        manager.drain_kernel_events_now()

        assert manager.kernel_event_pipeline_status()["last_sequence_seen"] == 1261
