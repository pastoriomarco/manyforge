"""Tests for deterministic pick-planning nodes."""

from __future__ import annotations

import py_trees
import pytest

from manyforge_behavior.blackboard_namespace import (
    make_runtime_field_ref,
    read_namespace_bundle,
    read_namespace_value,
    write_namespace_bundle,
)
from manyforge_behavior.kernel_client import (
    MockKernelClient,
    MotionPreviewResult,
    SceneEditRequest,
)
from manyforge_behavior.nodes.pick_planning import PlanPickSequenceNode


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel() -> MockKernelClient:
    return MockKernelClient()


@pytest.fixture
def bb_with_kernel(kernel: MockKernelClient):
    client = py_trees.blackboard.Client(name="test_setup")
    client.register_key(key="kernel", access=py_trees.common.Access.WRITE)
    client.kernel = kernel
    return client


def _tick_node(node: py_trees.behaviour.Behaviour) -> py_trees.common.Status:
    node.setup()
    node.initialise()
    return node.update()


def _tick_tree_until_done(
    node: py_trees.behaviour.Behaviour, max_ticks: int = 30
) -> py_trees.common.Status:
    tree = py_trees.trees.BehaviourTree(root=node)
    tree.setup()
    for _ in range(max_ticks):
        tree.tick()
        if tree.root.status != py_trees.common.Status.RUNNING:
            return tree.root.status
    return tree.root.status


def _seed_scene(kernel: MockKernelClient) -> None:
    requests = [
        SceneEditRequest(
            operation="add",
            object_id="table",
            shape_type="box",
            box_dimensions_m=[0.8, 0.6, 0.04],
            pose_position_m=[0.45, 0.0, -0.021],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ),
        SceneEditRequest(
            operation="add",
            object_id="sphere_part",
            shape_type="sphere",
            sphere_radius_m=0.01,
            pose_position_m=[0.40, -0.14, 0.009],
            pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
            frame_id="base_link",
        ),
    ]
    for request in requests:
        result = kernel.scene_edit(request)
        assert result.success is True


class TestPlanPickSequenceNode:
    def test_plans_pick_from_backend_scene(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        bb_with_kernel.register_key(
            key="perception/shape_target",
            access=py_trees.common.Access.WRITE,
        )
        write_namespace_bundle(
            bb_with_kernel,
            "perception/shape_target",
            value={"selected_object_id": "sphere_part"},
            status="selected",
        )

        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id=make_runtime_field_ref(
                "perception/shape_target",
                "selected_object_id",
            ),
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.04],
                "position_max_m": [0.0, 0.0, 0.04],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            search_strategy="best_cost_grid",
            translation_step_m=0.01,
            rotation_step_deg=15.0,
            planning_timeout_s=2.0,
            velocity_scale=0.30,
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_bundle = read_namespace_bundle(reader, "planning/pick_plan")
        plan_value = plan_bundle["value"]
        assert plan_value["selected_object_id"] == "sphere_part"
        approach_pose = plan_value["approach_pose"]
        pick_pose = plan_value["pick_pose"]
        assert approach_pose["position_m"][2] > pick_pose["position_m"][2]
        assert pick_pose["position_m"] == pytest.approx([0.40, -0.14, 0.059], abs=1e-6)
        assert pick_pose["orientation_xyzw"] == pytest.approx([1.0, 0.0, 0.0, 0.0], abs=1e-6)
        assert plan_value["planning_score"] == pytest.approx(3.06)
        assert plan_bundle["meta"]["status"] == "planned"
        metrics = kernel.get_metrics()
        assert metrics["plans_succeeded"] == 3
        assert metrics["validations_passed"] == 3

    def test_missing_viewer_snapshot_provider_fails_closed(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="viewer_snapshot",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_bundle = read_namespace_bundle(reader, "planning/pick_plan")
        assert plan_bundle["value"]["outcome"] == "image_source_unavailable"
        assert plan_bundle["meta"]["status"] == "failed"

    def test_viewer_snapshot_waits_for_cached_frame_without_blocking(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        renders = [None, b"fake-image"]
        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="viewer_snapshot",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.04],
                "position_max_m": [0.0, 0.0, 0.04],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            scene_snapshot_provider=lambda width, height, format: renders.pop(0),
        )

        status = _tick_tree_until_done(node)

        assert status == py_trees.common.Status.SUCCESS

    def test_unknown_object_fails_closed(self, kernel, bb_with_kernel):
        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="missing_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        assert read_namespace_value(reader, "planning/pick_plan")["outcome"] == "object_not_found"

    def test_invalid_pick_offset_envelope_fails_closed(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.04],
                "position_max_m": [0.0, 0.0, -0.02],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_bundle = read_namespace_bundle(reader, "planning/pick_plan")
        assert plan_bundle["value"]["outcome"] == "invalid_pick_offset_envelope"
        assert plan_bundle["meta"]["status"] == "failed"

    def test_start_state_collision_surfaces_detailed_failure(self, kernel, bb_with_kernel):
        class StartCollisionKernel(MockKernelClient):
            def preview_motion(self, request, start_state=None):
                return MotionPreviewResult(
                    ok=False,
                    error="Planning preview rejected: start state is already in collision",
                    error_details={
                        "failure_stage": "start_state",
                        "start_in_collision": "true",
                    },
                )

        collision_kernel = StartCollisionKernel()
        bb_with_kernel.kernel = collision_kernel
        _seed_scene(collision_kernel)
        node = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["outcome"] == "start_state_in_collision"
        assert "start_state_in_collision=true" in plan_value["failure_detail"]
