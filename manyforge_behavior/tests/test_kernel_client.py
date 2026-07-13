"""Tests for the mock kernel client."""

import sys
import threading

import pytest

from manyforge_behavior.kernel_client import (
    ActionSpan,
    ExecuteRequest,
    ExecutionState,
    GripperCommandRequest,
    GripperCommandState,
    JointDynamicsLimit,
    MockKernelClient,
    ObjectState,
    SceneObjectSnapshot,
    SceneEditRequest,
)


@pytest.fixture
def kernel():
    return MockKernelClient()


def make_request(move_id: str = "move_1") -> ExecuteRequest:
    return ExecuteRequest(
        robot_id="ur5",
        group_id="arm",
        move_id=move_id,
        base_frame="base_link",
        tcp_frame="tool0",
        motion_type="joint_goal",
        joint_goal_names=["j1", "j2"],
        joint_goal_positions_rad=[0.5, 1.0],
    )


class TestMockKernelExecute:
    def test_execute_from_idle(self, kernel):
        result = kernel.execute(make_request())
        assert result.ok
        assert result.execution_id == 1
        assert kernel.get_status().state == ExecutionState.COMPLETED

    def test_execute_increments_id(self, kernel):
        kernel.execute(make_request())
        kernel.reset()
        result = kernel.execute(make_request("move_2"))
        assert result.execution_id == 2

    def test_execute_from_terminal_state_requires_acknowledgement(self, kernel):
        first = kernel.execute(make_request())
        assert kernel.get_status().state == ExecutionState.COMPLETED
        result = kernel.execute(make_request("move_2"))
        assert not result.ok

        stale_ack = kernel.acknowledge_completed(first.execution_id + 1)
        assert not stale_ack.accepted
        assert kernel.get_status().state == ExecutionState.COMPLETED

        ack = kernel.acknowledge_completed(first.execution_id)
        assert ack.accepted
        assert kernel.get_status().state == ExecutionState.IDLE
        snapshot = kernel.get_execution_snapshot()
        assert snapshot.state == ExecutionState.IDLE
        assert snapshot.joint_positions_rad == [0.5, 1.0]

        result = kernel.execute(make_request("move_2"))
        assert result.ok

    def test_execute_from_non_terminal_non_idle_fails(self, kernel):
        kernel._state = ExecutionState.EXECUTING
        result = kernel.execute(make_request())
        assert not result.ok

    def test_execute_failure_injection(self, kernel):
        kernel.fail_next_execute = True
        result = kernel.execute(make_request())
        assert not result.ok
        assert kernel.get_status().state == ExecutionState.FAILED

    def test_execute_stamps_action_span_and_contact_policy(self, kernel):
        request = make_request()
        request.program_run_id = "run-001"
        request.action_span = ActionSpan(
            action_source="program_node",
            action_span_id="attempt-001",
            program_run_id="run-001",
            node_instance_id="sequence/move_to_pick",
            node_attempt_id="attempt-001",
            parent_node_attempt_id="plan-attempt-001",
        )
        request.contact_policy_id = "contact_grasp/lite6_box"

        result = kernel.execute(request)

        assert result.ok
        events = kernel.drain_kernel_events()
        assert events
        event_details = [event.get("details", {}) for event in events]
        assert any(
            details.get("action_span_id") == "attempt-001"
            and details.get("contact_policy_id") == "contact_grasp/lite6_box"
            and details.get("parent_node_attempt_id") == "plan-attempt-001"
            for details in event_details
        )
        snapshot = kernel.get_execution_snapshot()
        assert snapshot.action_span is not None
        assert snapshot.action_span.action_span_id == "attempt-001"
        assert snapshot.contact_policy_id == "contact_grasp/lite6_box"
        outcome = kernel.get_execution_outcome(result.execution_id)
        assert outcome is not None
        assert outcome.action_span is not None
        assert outcome.action_span.action_span_id == "attempt-001"
        assert outcome.contact_policy_id == "contact_grasp/lite6_box"

    def test_kernel_event_drain_is_atomic_with_concurrent_emit(self):
        kernel = MockKernelClient()
        event_count = 2000
        drained: list[dict] = []
        errors: list[BaseException] = []
        start = threading.Event()
        done = threading.Event()

        def emit_events() -> None:
            try:
                start.wait(timeout=1.0)
                for index in range(event_count):
                    kernel._emit_kernel_event(
                        "fixture_event",
                        details={"sequence": index},
                    )
            except BaseException as exc:  # pragma: no cover - test aid
                errors.append(exc)
            finally:
                done.set()

        def drain_events() -> None:
            try:
                start.wait(timeout=1.0)
                while not done.is_set():
                    drained.extend(kernel.drain_kernel_events())
                drained.extend(kernel.drain_kernel_events())
            except BaseException as exc:  # pragma: no cover - test aid
                errors.append(exc)

        emitter = threading.Thread(target=emit_events, name="emit-kernel-events")
        drainer = threading.Thread(target=drain_events, name="drain-kernel-events")
        emitter.start()
        drainer.start()
        start.set()
        emitter.join(timeout=2.0)
        drainer.join(timeout=2.0)

        assert not emitter.is_alive()
        assert not drainer.is_alive()
        assert errors == []
        sequences = sorted(
            int(event.get("details", {}).get("sequence"))
            for event in drained
            if event.get("type") == "fixture_event"
        )
        assert sequences == list(range(event_count))


class TestMockKernelStop:
    def test_stop_from_idle_rejected(self, kernel):
        result = kernel.stop()
        assert not result.accepted

    def test_stop_from_completed_rejected(self, kernel):
        kernel.execute(make_request())
        result = kernel.stop()
        assert not result.accepted

    def test_stop_count(self, kernel):
        kernel.stop()
        assert kernel.stop_count == 1


class TestMockKernelPauseResume:
    def test_pause_from_idle_rejected(self, kernel):
        result = kernel.pause()
        assert not result.accepted

    def test_resume_from_idle_rejected(self, kernel):
        result = kernel.resume()
        assert not result.accepted


class TestMockKernelReset:
    def test_reset_from_idle_rejected(self, kernel):
        result = kernel.reset()
        assert not result.accepted

    def test_reset_from_completed(self, kernel):
        kernel.execute(make_request())
        result = kernel.reset()
        assert result.accepted
        assert kernel.get_status().state == ExecutionState.IDLE

    def test_reset_from_failed(self, kernel):
        kernel.fail_next_execute = True
        kernel.execute(make_request())
        result = kernel.reset()
        assert result.accepted
        assert kernel.get_status().state == ExecutionState.IDLE


class TestMockKernelSceneEdit:
    def test_scene_edit_records(self, kernel):
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(req)
        assert result.success
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].object_id == "box1"

    def test_scene_edit_records_support_contacts(self, kernel):
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            support_contacts=[
                {
                    "support_object_id": "table",
                    "max_start_penetration_m": 0.005,
                    "require_separation_by_end": True,
                }
            ],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(req)
        assert result.success
        assert kernel.scene_edits[0].support_contacts == [
            {
                "support_object_id": "table",
                "max_start_penetration_m": 0.005,
                "require_separation_by_end": True,
            }
        ]
        snapshot = kernel.get_scene_objects()[0]
        assert snapshot.support_contacts == [
            {
                "support_object_id": "table",
                "max_start_penetration_m": 0.005,
                "require_separation_by_end": True,
            }
        ]

    def test_scene_edit_records_support_contact_without_start_penetration_limit(
        self, kernel
    ):
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            support_contacts=[
                {
                    "support_object_id": "table",
                    "enforce_max_start_penetration": False,
                    "require_separation_by_end": True,
                }
            ],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(req)
        assert result.success
        assert kernel.scene_edits[0].support_contacts == [
            {
                "support_object_id": "table",
                "enforce_max_start_penetration": False,
                "require_separation_by_end": True,
            }
        ]

    def test_scene_edit_rejects_invalid_support_contact_policy_field(self, kernel):
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                support_contacts=[
                    {
                        "support_object_id": "table",
                        "allow_recontact": "no",
                    }
                ],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            )
        )
        assert result.success is False
        assert result.error == "support_contacts[0].allow_recontact must be boolean"

    def test_scene_edit_rejects_partial_pose_source_freshness_metadata(self, kernel):
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="update_pose",
                object_id="box1",
                pose_position_m=[0.0, 0.0, 0.0],
                source_timestamp_ns=123,
            )
        )

        assert result.success is False
        assert result.error == (
            "update_pose source_timestamp_ns and source_freshness_budget_ns "
            "must be provided together"
        )

    def test_scene_edit_rejects_invalid_scene_admission_metadata(self, kernel):
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="update_pose",
                object_id="box1",
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                scene_admission_role=" ",
            )
        )
        assert result.success is False
        assert result.error == "scene_admission_role must be non-empty when provided"

        result = kernel.scene_edit(
            SceneEditRequest(
                operation="update_pose",
                object_id="box1",
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                scene_admission_lease_max_age_ns=0,
            )
        )
        assert result.success is False
        assert (
            result.error
            == (
                "scene_admission_role is required when scene admission "
                "metadata is provided"
            )
        )

        result = kernel.scene_edit(
            SceneEditRequest(
                operation="update_pose",
                object_id="box1",
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                scene_admission_role="snapshot_obstacle",
                scene_admission_target_drift_tolerance_m=0.01,
            )
        )
        assert result.success is False
        assert (
            result.error
            == (
                "scene_admission target drift tolerances are only valid "
                "for observed_target"
            )
        )

    def test_scene_edit_failure_injection(self, kernel):
        kernel.fail_next_scene_edit = True
        req = SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        result = kernel.scene_edit(req)
        assert not result.success

    def test_scene_edit_invalid_operation_fails(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(operation="teleport", object_id="box1"))
        assert result.success is False
        assert "Unsupported scene edit operation" in (result.error or "")

    def test_scene_edit_remove_missing_fails(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(operation="remove", object_id="missing"))
        assert result.success is False
        assert result.error == "Object not found"

    def test_scene_edit_upsert_inserts_or_replaces_object(self, kernel):
        insert = SceneEditRequest(
            operation="upsert",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.1, 0.2, 0.3],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        assert kernel.scene_edit(insert).success is True
        replace = SceneEditRequest(
            operation="upsert",
            object_id="box1",
            shape_type="sphere",
            sphere_radius_m=0.04,
            pose_position_m=[0.4, 0.5, 0.6],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        )
        assert kernel.scene_edit(replace).success is True
        snapshot = kernel.get_scene_objects()[0]
        assert snapshot.shape is not None
        assert snapshot.shape.type == "sphere"
        assert snapshot.pose_in_universe is not None
        assert snapshot.pose_in_universe["position_m"] == pytest.approx([0.4, 0.5, 0.6])

    def test_scene_edit_adds_mesh_object(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="mesh1",
            shape_type="mesh",
            mesh_resource_uri="file:///tmp/mesh.obj",
            mesh_scale=[1.0, 2.0, 3.0],
            pose_position_m=[0.1, 0.2, 0.3],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        ))
        assert result.success is True
        snapshot = kernel.get_scene_objects()[0]
        assert snapshot.shape is not None
        assert snapshot.shape.type == "mesh"
        assert snapshot.shape.mesh_resource_uri == "file:///tmp/mesh.obj"
        assert snapshot.shape.mesh_scale == [1.0, 2.0, 3.0]

    def test_scene_trace_records_scene_mutations(self, kernel):
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))
        kernel.scene_edit(SceneEditRequest(
            operation="attach",
            object_id="box1",
            frame_id="tool0",
        ))
        trace = kernel.get_scene_trace()
        assert [event["event"] for event in trace][-2:] == ["add", "attach"]
        assert trace[-1]["success"] is True
        assert trace[-1]["frame_id"] == "tool0"
        assert trace[-1]["attached"] is True
        assert trace[-1]["pose_in_frame"] is not None
        assert trace[-1]["pose_in_universe"] is not None


    def test_scene_edit_add_rejects_missing_orientation(self, kernel):
        """Spec 420 §4A.2: the sink never invents an orientation."""
        result = kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="no_orient",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
        ))
        assert not result.success
        assert "pose_orientation_xyzw" in (result.error or "")

    def test_scene_edit_upsert_rejects_missing_position(self, kernel):
        result = kernel.scene_edit(SceneEditRequest(
            operation="upsert",
            object_id="no_pos",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
        ))
        assert not result.success
        assert "pose_position_m" in (result.error or "")

    def test_scene_edit_authored_update_pose_rejects_missing_orientation(self, kernel):
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="upd1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=[0.0, 0.0, -0.382683432365, 0.923879532511],
        ))
        result = kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id="upd1",
            pose_position_m=[0.5, 0.0, 0.0],
        ))
        assert not result.success
        assert "authored" in (result.error or "")

    def test_scene_edit_pose_source_update_keeps_last_known_orientation(self, kernel):
        """A pose-source (live mirroring) position-only update keeps the last
        known orientation — never identity (spec 420 §4A.2)."""
        rotated = [0.0, 0.0, -0.382683432365, 0.923879532511]
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="tracked1",
            shape_type="box",
            box_dimensions_m=[0.1, 0.1, 0.1],
            pose_position_m=[0.0, 0.0, 0.0],
            pose_orientation_xyzw=rotated,
        ))
        result = kernel.scene_edit(SceneEditRequest(
            operation="update_pose",
            object_id="tracked1",
            pose_position_m=[0.5, 0.1, 0.0],
            source_timestamp_ns=1,
            source_freshness_budget_ns=1_000_000_000,
        ))
        assert result.success, result.error
        tracked = kernel._objects["tracked1"]
        assert tracked["position_m"] == [0.5, 0.1, 0.0]
        assert tracked["orientation_xyzw"] == rotated  # NOT identity

    def test_scene_edit_attachment_precondition_rejects_stale_pose_source(
        self, kernel
    ):
        assert kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="tracked_attachment",
                shape_type="box",
                box_dimensions_m=[0.1, 0.1, 0.1],
                pose_position_m=[0.0, 0.0, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        ).success
        assert kernel.scene_edit(
            SceneEditRequest(
                operation="attach",
                object_id="tracked_attachment",
                frame_id="tool0",
            )
        ).success

        result = kernel.scene_edit(
            SceneEditRequest(
                operation="update_pose",
                object_id="tracked_attachment",
                pose_position_m=[1.0, 2.0, 3.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
                expected_attached=False,
            )
        )

        assert result.success is False
        assert result.error_code == "scene_object_state_conflict"
        assert result.error_details["reason"] == (
            "scene_object_attachment_precondition_failed"
        )
        state = kernel.get_object_state("tracked_attachment")
        assert state.attached is True
        assert state.frame_id == "tool0"


class TestMockKernelMisc:
    def test_invalidate_trajectories(self, kernel):
        kernel.invalidate_trajectories()
        kernel.invalidate_trajectories()
        assert kernel.invalidate_count == 2

    def test_get_link_pose(self, kernel):
        pose = kernel.get_link_pose("tool0", "base_link")
        assert pose is not None
        assert "position_m" in pose
        assert "orientation_xyzw" in pose
        assert len(kernel.link_pose_calls) == 1

    def test_get_object_pose_and_state_follow_backend_scene_edits(self, kernel):
        kernel.scene_edit(SceneEditRequest(
            operation="add",
            object_id="box1",
            shape_type="box",
            box_dimensions_m=[0.05, 0.05, 0.05],
            pose_position_m=[0.4, 0.1, 0.2],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="universe",
        ))

        pose = kernel.get_object_pose("box1", "base_link")
        assert pose is not None
        assert pose["position_m"] == [0.4, 0.1, 0.2]

        state = kernel.get_object_state("box1")
        assert isinstance(state, ObjectState)
        assert state.exists is True
        assert state.attached is False
        assert state.frame_id == "universe"

    def test_get_object_state_reports_missing_object(self, kernel):
        state = kernel.get_object_state("missing_box")
        assert state.object_id == "missing_box"
        assert state.exists is False
        assert kernel.get_object_pose("missing_box", "base_link") is None

    def test_get_scene_objects_returns_authoritative_shape_and_pose(self, kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="sphere1",
                shape_type="sphere",
                sphere_radius_m=0.05,
                pose_position_m=[0.1, 0.2, 0.3],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        )

        objects = kernel.get_scene_objects()
        assert len(objects) == 1
        assert isinstance(objects[0], SceneObjectSnapshot)
        assert objects[0].object_id == "sphere1"
        assert objects[0].shape is not None
        assert objects[0].shape.type == "sphere"
        assert objects[0].shape.sphere_radius_m == pytest.approx(0.05)
        assert objects[0].pose_in_universe is not None
        assert objects[0].pose_in_universe["position_m"] == pytest.approx([0.1, 0.2, 0.3])

    def test_attach_and_detach_preserve_world_pose_with_stable_object_id(self, kernel):
        kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="box1",
                shape_type="box",
                box_dimensions_m=[0.05, 0.05, 0.05],
                pose_position_m=[0.4, 0.1, 0.2],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="universe",
            )
        )
        pose_before = kernel.get_object_pose("box1", "universe")
        assert pose_before is not None

        attach_result = kernel.scene_edit(
            SceneEditRequest(operation="attach", object_id="box1", frame_id="tool0")
        )
        assert attach_result.success is True
        attached_state = kernel.get_object_state("box1")
        assert attached_state.attached is True
        assert attached_state.frame_id == "tool0"
        pose_attached = kernel.get_object_pose("box1", "universe")
        assert pose_attached is not None
        assert pose_attached["position_m"] == pytest.approx(pose_before["position_m"])

        detach_result = kernel.scene_edit(
            SceneEditRequest(operation="detach", object_id="box1", frame_id="universe")
        )
        assert detach_result.success is True
        detached_state = kernel.get_object_state("box1")
        assert detached_state.attached is False
        assert detached_state.frame_id == "universe"
        pose_detached = kernel.get_object_pose("box1", "universe")
        assert pose_detached is not None
        assert pose_detached["position_m"] == pytest.approx(pose_attached["position_m"])

    def test_initial_status(self, kernel):
        status = kernel.get_status()
        assert status.state == ExecutionState.IDLE
        assert status.execution_id == 0
        assert status.progress == 0.0


class TestPerJointDynamicsLimits:
    def test_default_request_has_empty_caps(self):
        req = make_request()
        assert req.per_joint_dynamics_limits == {}

    def test_request_carries_per_joint_caps(self):
        req = make_request()
        req.per_joint_dynamics_limits = {
            "j1": JointDynamicsLimit(max_velocity_rad_s=0.5),
            "j2": JointDynamicsLimit(
                max_velocity_rad_s=0.4, max_acceleration_rad_s2=1.2
            ),
        }
        assert set(req.per_joint_dynamics_limits) == {"j1", "j2"}
        assert req.per_joint_dynamics_limits["j1"].max_velocity_rad_s == 0.5
        assert req.per_joint_dynamics_limits["j1"].max_acceleration_rad_s2 is None
        assert req.per_joint_dynamics_limits["j2"].max_acceleration_rad_s2 == 1.2
        assert req.per_joint_dynamics_limits["j2"].max_jerk_rad_s3 is None

    def test_caps_do_not_break_mock_execute(self, kernel):
        req = make_request()
        req.per_joint_dynamics_limits = {
            "j1": JointDynamicsLimit(max_velocity_rad_s=0.5)
        }
        result = kernel.execute(req)
        assert result.ok

    def test_joint_dynamics_limit_defaults_are_none(self):
        limit = JointDynamicsLimit()
        assert limit.max_velocity_rad_s is None
        assert limit.max_acceleration_rad_s2 is None
        assert limit.max_jerk_rad_s3 is None


class TestMockKernelGripperConcurrency:
    """Gripper command/position state must be lock-protected: the executing
    tree commands the gripper while status polls (and failure-injecting test
    threads) read it concurrently.
    """

    @staticmethod
    def _request(duration_s: float) -> GripperCommandRequest:
        return GripperCommandRequest(
            gripper_id="primary",
            group_id="gripper",
            joint_name="finger_joint",
            target_position=0.7,
            duration_s=duration_s,
        )

    def test_exactly_one_concurrent_command_wins_per_idle_gripper(self):
        # A tiny switch interval makes the pre-lock check-then-act race and
        # counter lost-updates observable within few rounds.
        switch_interval = sys.getswitchinterval()
        sys.setswitchinterval(1e-5)
        try:
            for _ in range(25):
                kernel = MockKernelClient()
                thread_count = 8
                barrier = threading.Barrier(thread_count)
                accepted: list[bool] = []
                errors: list[Exception] = []

                def worker() -> None:
                    try:
                        barrier.wait()
                        result = kernel.command_gripper(self._request(duration_s=30.0))
                        kernel.get_gripper_status("primary")
                        if result.accepted:
                            accepted.append(True)
                    except Exception as exc:  # noqa: BLE001
                        errors.append(exc)

                threads = [
                    threading.Thread(target=worker) for _ in range(thread_count)
                ]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()

                assert errors == []
                assert len(accepted) == 1
                assert kernel.gripper_command_count == thread_count
        finally:
            sys.setswitchinterval(switch_interval)

    def test_stop_fences_command_and_new_execute_reauthorizes(self):
        kernel = MockKernelClient(execute_duration_s=30.0)
        assert kernel.execute(make_request("first_motion")).ok
        first = kernel.command_gripper(self._request(duration_s=30.0))
        assert first.accepted
        first_status = kernel.get_gripper_status("primary")

        assert kernel.stop().accepted
        stopped_status = kernel.get_gripper_status("primary")
        assert stopped_status.state == GripperCommandState.IDLE
        assert stopped_status.generation != first_status.generation
        blocked = kernel.command_gripper(self._request(duration_s=0.0))
        assert not blocked.accepted
        assert blocked.reason_code == "gripper_command_not_authorized"

        assert kernel.execute(make_request("next_motion")).ok
        assert kernel.command_gripper(self._request(duration_s=0.0)).accepted


class TestMockKernelGripperSemantics:
    def test_pneumatic_command_resolves_target_from_srdf_state_ref(self):
        kernel = MockKernelClient()
        result = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="lite_gripper",
                joint_name="right_finger_joint",
                target_position=0.0081000001,
                duration_s=0.0,
                actuation_kind="pneumatic",
                state_authority="native_backend",
                completion_policy="dwell_elapsed",
                grasp_confirmation_policy="none_open_loop",
                transport_kind="native_kinematic",
                open_state_ref="open",
                closed_state_ref="closed",
                srdf_presets={"open": 0.0081, "closed": 0.0},
            )
        )

        assert result.accepted is True
        status = kernel.get_gripper_status("primary")
        assert status.state == GripperCommandState.COMPLETED
        assert status.commanded_state == "open"
        assert status.current_position == pytest.approx(0.0081)
        assert status.target_position == pytest.approx(0.0081)
        assert status.observed_position == pytest.approx(0.0081)
        assert kernel.last_gripper_command_request is not None
        assert kernel.last_gripper_command_request.target_position == pytest.approx(
            0.0081
        )

    def test_pneumatic_numeric_target_rejected_when_it_matches_no_state_ref(self):
        kernel = MockKernelClient()
        result = kernel.command_gripper(
            GripperCommandRequest(
                gripper_id="primary",
                group_id="lite_gripper",
                joint_name="right_finger_joint",
                target_position=0.004,
                duration_s=0.0,
                actuation_kind="pneumatic",
                state_authority="native_backend",
                completion_policy="dwell_elapsed",
                grasp_confirmation_policy="none_open_loop",
                transport_kind="native_kinematic",
                open_state_ref="open",
                closed_state_ref="closed",
                srdf_presets={"open": 0.0081, "closed": 0.0},
            )
        )

        assert result.accepted is False
        assert result.reason_code == "invalid_gripper_semantics"
        assert "open_state_ref" in (result.error or "")


def test_mock_recovery_default_is_complete_and_fail_closed():
    kernel = MockKernelClient()

    assessment = kernel.get_recovery_assessment()
    assert assessment["schema_version"] == (
        "manyforge.kernel_recovery_assessment.v1"
    )
    assert assessment["status"] == "unavailable"
    assert assessment["allowed_actions"] == []
    assert assessment["retry_admissible"] is False
    assert assessment["observed_state_available"] is False
    assert assessment["observed_state_authority_profile"] == ""
    assert assessment["observed_state_measured"] is False
    assert assessment["stronger_authority_active"] is False
    assert assessment["queued_latch_present"] is False

    result = kernel.apply_recovery_action(
        {
            "request_id": "mock-recovery-probe",
            "action": "retry_motion_from_current_state",
        }
    )
    assert result["accepted"] is False
    assert result["resulting_assessment"] == assessment
