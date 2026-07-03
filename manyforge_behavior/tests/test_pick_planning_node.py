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
from manyforge_behavior.geometry import (
    quat_rotate_vector,
    quats_close,
    rpy_deg_to_xyzw,
)
from manyforge_behavior.kernel_client import (
    MockKernelClient,
    MotionPreviewResult,
    RobotStateSnapshot,
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


def _normalized_xy(vector: list[float]) -> list[float]:
    length = (vector[0] ** 2 + vector[1] ** 2) ** 0.5
    assert length > 1e-9
    return [vector[0] / length, vector[1] / length, 0.0]


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
        retreat_pose = plan_value["retreat_pose"]
        assert approach_pose["frame_id"] == "base_link"
        assert pick_pose["frame_id"] == "base_link"
        assert retreat_pose["frame_id"] == "base_link"
        assert approach_pose["position_m"][2] > pick_pose["position_m"][2]
        assert retreat_pose == approach_pose
        assert pick_pose["position_m"] == pytest.approx([0.40, -0.14, 0.059], abs=1e-6)
        assert pick_pose["orientation_xyzw"] == pytest.approx([1.0, 0.0, 0.0, 0.0], abs=1e-6)
        assert plan_value["planning_score"] == pytest.approx(3.06)
        assert plan_bundle["meta"]["status"] == "planned"
        metrics = kernel.get_metrics()
        assert metrics["plans_succeeded"] == 3
        assert metrics["validations_passed"] == 3

    def test_force_vertical_pick_preserves_object_heading_and_ignores_rpy_offsets(
        self,
        kernel,
        bb_with_kernel,
    ):
        object_orientation = rpy_deg_to_xyzw([20.0, 35.0, 45.0])
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="tilted_part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=object_orientation,
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_vertical",
            selected_object_id="tilted_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.02],
                "position_max_m": [0.0, 0.0, 0.02],
                # These inverted RPY bounds prove force_vertical_pick disables
                # rotational envelope sampling instead of using stale tilt.
                "orientation_rpy_min_deg": [45.0, 15.0, 30.0],
                "orientation_rpy_max_deg": [-45.0, -15.0, -30.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [90.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [-90.0, 0.0, 0.0],
            },
            force_vertical_pick=True,
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        pick_orientation = plan_value["pick_pose"]["orientation_xyzw"]
        approach_orientation = plan_value["approach_pose"]["orientation_xyzw"]
        assert quats_close(pick_orientation, approach_orientation)
        assert quat_rotate_vector(pick_orientation, [0.0, 0.0, 1.0]) == pytest.approx(
            [0.0, 0.0, -1.0],
            abs=1e-6,
        )

        expected_x = _normalized_xy(
            quat_rotate_vector(object_orientation, [1.0, 0.0, 0.0])
        )
        actual_x = quat_rotate_vector(pick_orientation, [1.0, 0.0, 0.0])
        assert actual_x == pytest.approx(expected_x, abs=1e-6)
        assert plan_value["attempted_candidates"] == 1
        assert plan_value["force_vertical_pick"] is True

    def test_pick_offset_reference_object_rotates_translation_into_base(
        self,
        kernel,
        bb_with_kernel,
    ):
        object_orientation = rpy_deg_to_xyzw([0.0, 0.0, 90.0])
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="yawed_part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=object_orientation,
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_object_ref",
            selected_object_id="yawed_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.02, 0.0, 0.0],
                "position_max_m": [0.02, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            pick_offset_reference={"type": "object", "id": "yawed_part"},
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "frame", "id": "base_link"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.35, -0.10, 0.05],
            abs=1e-6,
        )
        assert plan_value["approach_pose"]["position_m"] == pytest.approx(
            [0.35, -0.10, 0.10],
            abs=1e-6,
        )
        assert plan_value["pick_offset_reference"] == {
            "type": "object",
            "id": "yawed_part",
        }
        assert plan_value["approach_offset_reference"] == {
            "type": "frame",
            "id": "base_link",
        }

    def test_pick_offset_reference_robot_link_rotates_translation_into_base(
        self,
        bb_with_kernel,
    ):
        class LinkYawKernel(MockKernelClient):
            def get_link_pose(self, target_frame, base_frame):
                self.link_pose_calls.append((target_frame, base_frame))
                if target_frame == "link_tcp":
                    return {
                        "position_m": [0.5, 0.0, 0.6],
                        "orientation_xyzw": rpy_deg_to_xyzw([0.0, 0.0, 90.0]),
                    }
                return super().get_link_pose(target_frame, base_frame)

        link_kernel = LinkYawKernel()
        bb_with_kernel.kernel = link_kernel
        result = link_kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_link_ref",
            selected_object_id="part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.02, 0.0, 0.0],
                "position_max_m": [0.02, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            pick_offset_reference={"type": "robot_link", "id": "link_tcp"},
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "frame", "id": "base_link"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.35, -0.10, 0.05],
            abs=1e-6,
        )
        assert plan_value["approach_pose"]["position_m"] == pytest.approx(
            [0.35, -0.10, 0.10],
            abs=1e-6,
        )
        assert ("link_tcp", "base_link") in link_kernel.link_pose_calls
        assert plan_value["pick_offset_reference"] == {
            "type": "robot_link",
            "id": "link_tcp",
        }

    def test_pick_offset_reference_tcp_frame_uses_candidate_pick_orientation(
        self,
        bb_with_kernel,
    ):
        class LiveTcpYawKernel(MockKernelClient):
            def get_link_pose(self, target_frame, base_frame):
                self.link_pose_calls.append((target_frame, base_frame))
                if target_frame == "link_tcp":
                    return {
                        "position_m": [0.5, 0.0, 0.6],
                        "orientation_xyzw": rpy_deg_to_xyzw([0.0, 0.0, 90.0]),
                    }
                return super().get_link_pose(target_frame, base_frame)

        link_kernel = LiveTcpYawKernel()
        bb_with_kernel.kernel = link_kernel
        result = link_kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_candidate_tcp_ref",
            selected_object_id="part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="link_tcp",
            force_vertical_pick=True,
            pick_offset_envelope={
                "position_min_m": [0.02, 0.0, 0.0],
                "position_max_m": [0.02, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            pick_offset_reference={"type": "robot_link", "id": "link_tcp"},
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.05],
                "position_max_m": [0.0, 0.0, 0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "frame", "id": "base_link"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.37, -0.12, 0.05],
            abs=1e-6,
        )
        assert ("link_tcp", "base_link") not in link_kernel.link_pose_calls

    def test_approach_offset_reference_object_rotates_translation_into_base(
        self,
        kernel,
        bb_with_kernel,
    ):
        object_orientation = rpy_deg_to_xyzw([0.0, 0.0, 90.0])
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="yawed_part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=object_orientation,
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_approach_object_ref",
            selected_object_id="yawed_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.0],
                "position_max_m": [0.0, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.05, 0.0, 0.0],
                "position_max_m": [0.05, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "object", "id": "yawed_part"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.35, -0.12, 0.05],
            abs=1e-6,
        )
        assert plan_value["approach_pose"]["position_m"] == pytest.approx(
            [0.35, -0.07, 0.05],
            abs=1e-6,
        )

    def test_approach_offset_reference_robot_link_rotates_translation_into_base(
        self,
        bb_with_kernel,
    ):
        class LinkYawKernel(MockKernelClient):
            def get_link_pose(self, target_frame, base_frame):
                self.link_pose_calls.append((target_frame, base_frame))
                if target_frame == "link_tcp":
                    return {
                        "position_m": [0.5, 0.0, 0.6],
                        "orientation_xyzw": rpy_deg_to_xyzw([0.0, 0.0, 90.0]),
                    }
                return super().get_link_pose(target_frame, base_frame)

        link_kernel = LinkYawKernel()
        bb_with_kernel.kernel = link_kernel
        result = link_kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_approach_link_ref",
            selected_object_id="part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.0],
                "position_max_m": [0.0, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.05, 0.0, 0.0],
                "position_max_m": [0.05, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "robot_link", "id": "link_tcp"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.35, -0.12, 0.05],
            abs=1e-6,
        )
        assert plan_value["approach_pose"]["position_m"] == pytest.approx(
            [0.35, -0.07, 0.05],
            abs=1e-6,
        )
        assert ("link_tcp", "base_link") in link_kernel.link_pose_calls
        assert plan_value["approach_offset_reference"] == {
            "type": "robot_link",
            "id": "link_tcp",
        }

    def test_approach_offset_reference_tcp_frame_uses_candidate_pick_orientation(
        self,
        bb_with_kernel,
    ):
        class LiveTcpYawKernel(MockKernelClient):
            def get_link_pose(self, target_frame, base_frame):
                self.link_pose_calls.append((target_frame, base_frame))
                if target_frame == "link_tcp":
                    return {
                        "position_m": [0.5, 0.0, 0.6],
                        "orientation_xyzw": rpy_deg_to_xyzw([0.0, 0.0, 90.0]),
                    }
                return super().get_link_pose(target_frame, base_frame)

        link_kernel = LiveTcpYawKernel()
        bb_with_kernel.kernel = link_kernel
        result = link_kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="part",
                shape_type="box",
                box_dimensions_m=[0.10, 0.02, 0.02],
                pose_position_m=[0.35, -0.12, 0.04],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True

        node = PlanPickSequenceNode(
            name="plan_pick_approach_candidate_tcp_ref",
            selected_object_id="part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="link_tcp",
            force_vertical_pick=True,
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.0],
                "position_max_m": [0.0, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            pick_offset_reference={"type": "frame", "id": "base_link"},
            approach_offset_envelope={
                "position_min_m": [0.05, 0.0, 0.0],
                "position_max_m": [0.05, 0.0, 0.0],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_reference={"type": "robot_link", "id": "link_tcp"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"] == pytest.approx(
            [0.35, -0.12, 0.05],
            abs=1e-6,
        )
        assert plan_value["approach_pose"]["position_m"] == pytest.approx(
            [0.40, -0.12, 0.05],
            abs=1e-6,
        )
        assert ("link_tcp", "base_link") not in link_kernel.link_pose_calls

    def test_min_tcp_z_clamps_generated_pick_and_reports_trace(
        self,
        kernel,
        bb_with_kernel,
    ):
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="low_part",
                shape_type="sphere",
                sphere_radius_m=0.01,
                pose_position_m=[0.30, 0.12, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True
        node = PlanPickSequenceNode(
            name="plan_pick_min_z",
            selected_object_id="low_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, -0.05],
                "position_max_m": [0.0, 0.0, -0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.02],
                "position_max_m": [0.0, 0.0, 0.02],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            min_tcp_z_m=0.03,
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"][2] == pytest.approx(0.03)
        assert plan_value["approach_pose"]["position_m"][2] == pytest.approx(0.05)
        assert plan_value["enable_min_tcp_z"] is True
        assert plan_value["min_tcp_z_m"] == pytest.approx(0.03)
        assert plan_value["min_tcp_z_applied"] is True

    def test_min_tcp_z_disabled_keeps_threshold_but_does_not_clamp(
        self,
        kernel,
        bb_with_kernel,
    ):
        result = kernel.scene_edit(
            SceneEditRequest(
                operation="add",
                object_id="low_part",
                shape_type="sphere",
                sphere_radius_m=0.01,
                pose_position_m=[0.30, 0.12, 0.0],
                pose_orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
                frame_id="base_link",
            )
        )
        assert result.success is True
        node = PlanPickSequenceNode(
            name="plan_pick_min_z_disabled",
            selected_object_id="low_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_envelope={
                "position_min_m": [0.0, 0.0, -0.05],
                "position_max_m": [0.0, 0.0, -0.05],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            approach_offset_envelope={
                "position_min_m": [0.0, 0.0, 0.02],
                "position_max_m": [0.0, 0.0, 0.02],
                "orientation_rpy_min_deg": [0.0, 0.0, 0.0],
                "orientation_rpy_max_deg": [0.0, 0.0, 0.0],
            },
            enable_min_tcp_z=False,
            min_tcp_z_m=0.03,
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.SUCCESS
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["pick_pose"]["position_m"][2] == pytest.approx(-0.04)
        assert plan_value["approach_pose"]["position_m"][2] == pytest.approx(-0.02)
        assert plan_value["enable_min_tcp_z"] is False
        assert plan_value["min_tcp_z_m"] is None
        assert plan_value["min_tcp_z_applied"] is False

    def test_invalid_min_tcp_z_fails_closed(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        node = PlanPickSequenceNode(
            name="plan_pick_bad_min_z",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            min_tcp_z_m=-0.01,
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_value = read_namespace_value(reader, "planning/pick_plan")
        assert plan_value["outcome"] == "invalid_min_tcp_z"

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

    def test_invalid_pick_offset_reference_fails_closed(self, kernel, bb_with_kernel):
        _seed_scene(kernel)
        node = PlanPickSequenceNode(
            name="plan_pick_bad_reference",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            pick_offset_reference={"type": "object", "id": "missing_part"},
        )

        status = _tick_node(node)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_bundle = read_namespace_bundle(reader, "planning/pick_plan")
        assert plan_bundle["value"]["outcome"] == "invalid_pick_offset_reference"
        assert "missing_part" in plan_bundle["value"]["failure_detail"]
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
        assert plan_value["approach_pose"] is not None
        assert plan_value["pick_pose"] is not None
        assert plan_value["retreat_pose"] == plan_value["approach_pose"]
        assert "start_state_in_collision=true" in plan_value["failure_detail"]

    def test_support_contact_preview_failure_surfaces_policy_numbers(
        self, kernel, bb_with_kernel
    ):
        class SupportContactKernel(MockKernelClient):
            def preview_motion(self, request, start_state=None):
                if request.move_id == "pick_approach_preview":
                    return MotionPreviewResult(ok=True, final_state=RobotStateSnapshot())
                return MotionPreviewResult(
                    ok=False,
                    error="Collision validation rejected trajectory",
                    error_details={
                        "collision_result_first_failure_pair": (
                            "object/ground/shape/0 <-> "
                            "robot/robot0/attached/graspable/shape/0"
                        ),
                        "collision_result_first_failure_classification": (
                            "support_contact_start"
                        ),
                        "collision_result_waypoint_index": "0",
                        "collision_result_raw_contact_release_current_penetration_m": (
                            "0.021667"
                        ),
                        "collision_result_raw_contact_release_max_start_penetration_m": (
                            "0.020000"
                        ),
                        "collision_result_raw_contact_release_enforce_max_start_penetration": (
                            "true"
                        ),
                    },
                )

        collision_kernel = SupportContactKernel()
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
        assert plan_value["approach_pose"] is not None
        assert plan_value["pick_pose"] is not None
        assert plan_value["retreat_pose"] == plan_value["approach_pose"]
        detail = plan_value["failure_detail"]
        assert "classification=support_contact_start" in detail
        assert "waypoint=0" in detail
        assert "current_penetration_m=0.021667" in detail
        assert "max_start_penetration_m=0.020000" in detail

    def test_failure_populates_linked_move_pose_goals(self, kernel, bb_with_kernel):
        class PickFailureKernel(MockKernelClient):
            def preview_motion(self, request, start_state=None):
                if request.move_id == "pick_approach_preview":
                    return MotionPreviewResult(ok=True, final_state=RobotStateSnapshot())
                return MotionPreviewResult(ok=False, error="IK failed")

        class MoveStub(py_trees.behaviour.Behaviour):
            def __init__(self, name: str):
                super().__init__(name=name)
                self._params = {"motion_type": "pose_goal", "pose_goal": None}

            def update(self):
                return py_trees.common.Status.SUCCESS

        failure_kernel = PickFailureKernel()
        bb_with_kernel.kernel = failure_kernel
        _seed_scene(failure_kernel)
        planner = PlanPickSequenceNode(
            name="plan_pick",
            selected_object_id="sphere_part",
            output_key_prefix="planning/pick_plan",
            object_source="backend_scene",
            image_source="none",
            robot_id="ur5",
            group_id="arm",
            base_frame="base_link",
            tcp_frame="tool0",
            approach_move_node_name="move_to_approach",
            pick_move_node_name="move_to_pick",
            retreat_move_node_name="move_to_retreat",
        )
        approach_move = MoveStub("move_to_approach")
        pick_move = MoveStub("move_to_pick")
        retreat_move = MoveStub("move_to_retreat")
        root = py_trees.composites.Sequence(name="root", memory=True)
        root.add_children([planner, approach_move, pick_move, retreat_move])

        status = _tick_tree_until_done(root)

        assert status == py_trees.common.Status.FAILURE
        reader = py_trees.blackboard.Client(name="reader")
        plan_bundle = read_namespace_bundle(reader, "planning/pick_plan")
        plan_value = plan_bundle["value"]
        assert plan_bundle["meta"]["status"] == "failed"
        assert plan_value["outcome"] == "no_feasible_plan"
        assert approach_move._params["pose_goal"] == plan_value["approach_pose"]
        assert pick_move._params["pose_goal"] == plan_value["pick_pose"]
        assert retreat_move._params["pose_goal"] == plan_value["retreat_pose"]
