"""Tests for the orchestrator runner."""

from pathlib import Path
from unittest.mock import patch

import pytest
import py_trees
import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.deployment import Deployment
from manyforge_behavior.kernel_client import ExecutionState, MockKernelClient
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_behavior.runner import OrchestratorRunner

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)
DEPLOYMENT_FIXTURE = str(
    Path(__file__).parent / "fixtures" / "test.deployment.yaml"
)
VISION_PROGRAM_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "vision_guided_bin_pick.program.yaml"
)
VISION_DEPLOYMENT_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "vision_guided_bin_pick.deployment.yaml"
)
VISION_POSE_PROGRAM_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "vision_guided_pose_pick.program.yaml"
)
VISION_POSE_DEPLOYMENT_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "vision_guided_pose_pick.deployment.yaml"
)
SEMANTIC_SHAPE_PROGRAM_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "semantic_shape_pick.program.yaml"
)
SEMANTIC_SHAPE_DEPLOYMENT_EXAMPLE = str(
    Path(__file__).parent.parent.parent / "examples" / "semantic_shape_pick.deployment.yaml"
)


@pytest.fixture(autouse=True)
def reset_blackboard():
    py_trees.blackboard.Blackboard.enable_activity_stream()
    yield
    py_trees.blackboard.Blackboard.clear()


@pytest.fixture
def kernel():
    return MockKernelClient()


class TestRunnerLoading:
    def test_load_succeeds(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        assert runner.catalog is not None
        assert runner.program is not None
        assert runner.program.name == "test_pick_and_place"

    def test_build_tree(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        tree = runner.build_tree()
        assert tree is not None
        assert tree.root.name == "main_sequence"


class TestRunnerSceneApplication:
    def test_apply_scene(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.apply_scene()
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].operation == "add"
        assert kernel.scene_edits[0].object_id == "table"

    def test_apply_scene_failure(self, kernel):
        kernel.fail_next_scene_edit = True
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        with pytest.raises(RuntimeError, match="Failed to add scene"):
            runner.apply_scene()


class TestRunnerExecution:
    def test_full_run(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        result = runner.run(max_ticks=100)
        assert result is True
        # apply_scene() adds the static table, and the tree removes it after the move.
        assert len(kernel.scene_edits) >= 2
        assert kernel.execute_count >= 1  # move_to_pick

    def test_run_with_override(self, kernel):
        runner = OrchestratorRunner(
            CATALOG_PATH,
            FIXTURE_PATH,
            kernel,
            overrides={"pick_velocity_scale": 0.8},
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        result = runner.run(max_ticks=100)
        assert result is True

    def test_run_fails_on_execute_failure(self, kernel):
        kernel.fail_next_execute = True
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        runner.apply_scene()
        result = runner.run(max_ticks=100)
        assert result is False

    def test_run_without_load_raises(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        with pytest.raises(RuntimeError):
            runner.build_tree()

    def test_run_without_build_raises(self, kernel):
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        with pytest.raises(RuntimeError):
            runner.run()

    def test_max_ticks_returns_false(self, kernel):
        """If tree doesn't complete within max_ticks, return False."""
        # Normal run completes quickly, so use max_ticks=0
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        runner.build_tree()
        result = runner.run(max_ticks=0)
        assert result is False

    def test_run_with_skill_authored_program(self, kernel, tmp_path):
        """Runner expands shipped skills into executable runtime nodes."""
        program_path = tmp_path / "skill.program.yaml"
        program_path.write_text(
            yaml.safe_dump(
                {
                    "api_version": "manyforge.program.v0",
                    "program_profile": "v0.0",
                    "name": "skill_program",
                    "description": "skill-authored test program",
                    "robot": {
                        "robot_id": "ur5",
                        "group_id": "arm",
                        "base_frame": "base_link",
                        "tcp_frame": "tool0",
                    },
                    "scene": [],
                    "parameters": [],
                    "tree": {
                        "id": "move_to_named_config",
                        "name": "go_home",
                        "params": {
                            "config_name": "home",
                            "joint_names": [
                                "shoulder_pan_joint",
                                "shoulder_lift_joint",
                                "elbow_joint",
                                "wrist_1_joint",
                                "wrist_2_joint",
                                "wrist_3_joint",
                            ],
                            "joint_positions_rad": [0.0, -1.0, 1.0, -1.0, -1.0, 0.0],
                            "velocity_scale": 0.25,
                        },
                    },
                },
                sort_keys=False,
            ),
            encoding="utf-8",
        )

        runner = OrchestratorRunner(CATALOG_PATH, str(program_path), kernel)
        runner.load()
        tree = runner.build_tree()

        assert tree.root.name == "go_home"
        result = runner.run(max_ticks=10)
        assert result is True
        assert kernel.execute_count == 1
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.move_id == "move_to_home"


class TestRunnerChildCardinality:
    def test_runner_rejects_leaf_with_children(self, kernel):
        catalog = NodeCatalog(CATALOG_PATH)
        program = Program.from_dict(
            {
                "api_version": "manyforge.program.v0",
                "program_profile": "v0.0",
                "name": "invalid_leaf_children",
                "catalog_version": "0.1.0",
                "robot": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                },
                "tree": {
                    "id": "timer",
                    "name": "timer_leaf",
                    "params": {"duration_s": 0.1},
                },
            },
            catalog,
        )
        program.tree.children.append(
            Program.from_dict(
                {
                    "api_version": "manyforge.program.v0",
                    "program_profile": "v0.0",
                    "name": "single_wait",
                    "catalog_version": "0.1.0",
                    "robot": {
                        "robot_id": "ur5",
                        "group_id": "arm",
                        "base_frame": "base_link",
                        "tcp_frame": "tool0",
                    },
                    "tree": {
                        "id": "timer",
                        "name": "unexpected_child",
                        "params": {"duration_s": 0.1},
                    },
                },
                catalog,
            ).tree
        )

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel, program=program)
        runner.load()

        with pytest.raises(ValueError, match="allows at most 0 children"):
            runner.build_tree()

    def test_runner_rejects_decorator_with_multiple_children(self, kernel):
        catalog = NodeCatalog(CATALOG_PATH)
        program = Program.from_dict(
            {
                "api_version": "manyforge.program.v0",
                "program_profile": "v0.0",
                "name": "invalid_decorator_children",
                "catalog_version": "0.1.0",
                "robot": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                },
                "tree": {
                    "id": "inverter",
                    "name": "invert_once",
                    "children": [
                        {"id": "timer", "name": "first", "params": {"duration_s": 0.1}},
                    ],
                },
            },
            catalog,
        )
        program.tree.children.append(
            Program.from_dict(
                {
                    "api_version": "manyforge.program.v0",
                    "program_profile": "v0.0",
                    "name": "second_wait",
                    "catalog_version": "0.1.0",
                    "robot": {
                        "robot_id": "ur5",
                        "group_id": "arm",
                        "base_frame": "base_link",
                        "tcp_frame": "tool0",
                    },
                    "tree": {
                        "id": "timer",
                        "name": "second",
                        "params": {"duration_s": 0.2},
                    },
                },
                catalog,
            ).tree
        )

        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel, program=program)
        runner.load()

        with pytest.raises(ValueError, match="allows at most 1 child"):
            runner.build_tree()


class TestRelativeMoveValidation:
    @staticmethod
    def _base_program_dict(relative_move: dict) -> dict:
        return {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "relative_move_validation",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": {
                "id": "move_manipulator_action",
                "name": "relative_move_step",
                "params": {
                    "move_id": "move_1",
                    "motion_type": "relative_move",
                    "relative_move": relative_move,
                },
            },
        }

    def test_program_rejects_relative_move_pose_reference_without_pose(self):
        catalog = NodeCatalog(CATALOG_PATH)

        with pytest.raises(
            ProgramValidationError,
            match="relative_move.target_reference.pose is required when relative_move.target_reference.type='pose'",
        ):
            Program.from_dict(
                self._base_program_dict(
                    {
                        "target_reference": {"type": "pose"},
                        "offset": {
                            "position_m": [0.0, 0.0, 0.08],
                            "orientation_rpy_deg": [180.0, 0.0, 0.0],
                        },
                    }
                ),
                catalog,
            )

    def test_program_rejects_relative_move_frame_reference_without_id(self):
        catalog = NodeCatalog(CATALOG_PATH)

        with pytest.raises(
            ProgramValidationError,
            match="relative_move.target_reference.id is required when relative_move.target_reference.type='frame'",
        ):
            Program.from_dict(
                self._base_program_dict(
                    {
                        "target_reference": {"type": "frame"},
                        "offset": {
                            "position_m": [0.0, 0.0, 0.08],
                            "orientation_rpy_deg": [180.0, 0.0, 0.0],
                        },
                    }
                ),
                catalog,
            )

    def test_program_rejects_relative_move_without_offset(self):
        catalog = NodeCatalog(CATALOG_PATH)

        with pytest.raises(ProgramValidationError, match="offset is required"):
            Program.from_dict(
                self._base_program_dict(
                    {
                        "target_reference": {"type": "object", "id": "target_object"},
                    }
                ),
                catalog,
            )


class TestRunnerDeployment:
    def test_load_with_deployment(self, kernel):
        """Runner loads deployment and validates against program."""
        runner = OrchestratorRunner(
            CATALOG_PATH, FIXTURE_PATH, kernel,
            deployment_path=DEPLOYMENT_FIXTURE)
        runner.load()
        assert runner.deployment is not None
        assert runner.deployment.robot_config.global_max_tcp_linear_speed_mps == 0.25

    def test_deployment_profile_mismatch_raises(self, kernel):
        """Deployment with wrong profile raises during load."""
        import tempfile
        import yaml
        data = {
            "api_version": "manyforge.deployment.v0",
            "schema_version": "0.1.0",
            "name": "bad",
            "program_profile": "v99.0",  # mismatch
            "robot_config": {
                "robot_id": "ur5", "group_id": "arm",
                "urdf_path": "test.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
            },
            "adapters": {
                "planning": {"adapter_id": "p", "adapter_version": "0.1.0", "config": {}},
                "context": {"adapter_id": "c", "adapter_version": "0.1.0", "config": {}},
                "execution": {"adapter_id": "e", "adapter_version": "0.1.0", "config": {}},
            },
            "orchestrator": {"engine": "py_trees", "config": {}},
            "launch": {"runner": {"module": "m", "args": []}},
        }
        f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
        yaml.dump(data, f)
        f.close()
        runner = OrchestratorRunner(
            CATALOG_PATH, FIXTURE_PATH, kernel,
            deployment_path=f.name)
        from manyforge_behavior.deployment import DeploymentValidationError
        with pytest.raises(DeploymentValidationError):
            runner.load()
        import os
        os.unlink(f.name)

    def test_global_tcp_speed_on_blackboard(self, kernel):
        """After build_tree with deployment, global TCP speed is on blackboard."""
        runner = OrchestratorRunner(
            CATALOG_PATH, FIXTURE_PATH, kernel,
            deployment_path=DEPLOYMENT_FIXTURE)
        runner.load()
        runner.build_tree()
        bb = py_trees.blackboard.Client(name="test_tcp")
        bb.register_key(key="global_max_tcp_linear_speed_mps",
                        access=py_trees.common.Access.READ)
        assert bb.global_max_tcp_linear_speed_mps == 0.25

    def test_gripper_configs_on_blackboard(self, kernel, tmp_path):
        deployment_path = tmp_path / "with_gripper.deployment.yaml"
        deployment_path.write_text(
            yaml.safe_dump(
                {
                    "api_version": "manyforge.deployment.v0",
                    "schema_version": "0.1.0",
                    "name": "with_gripper",
                    "program_profile": "v0.0",
                    "robot_config": {
                        "robot_id": "ur5",
                        "group_id": "arm",
                        "urdf_path": "package://roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf",
                        "srdf_path": "package://roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf",
                        "global_max_tcp_linear_speed_mps": 0.25,
                    },
                    "grippers": {
                        "primary": {
                            "command_mode": "simulated_joint",
                            "group_id": "gripper",
                            "joint_name": "finger_joint",
                            "open_position": 0.0,
                            "closed_position": 0.7,
                            "motion_time_s": 0.8,
                            "touch_links": ["left_inner_finger_pad", "right_inner_finger_pad"],
                        }
                    },
                    "adapters": {
                        "planning": {"adapter_id": "p", "adapter_version": "0.1.0", "config": {}},
                        "context": {"adapter_id": "c", "adapter_version": "0.1.0", "config": {}},
                        "execution": {"adapter_id": "e", "adapter_version": "0.1.0", "config": {}},
                    },
                    "orchestrator": {"engine": "py_trees", "config": {}},
                    "launch": {"runner": {"module": "manyforge_behavior.runner", "args": []}},
                }
            )
        )
        runner = OrchestratorRunner(
            CATALOG_PATH,
            FIXTURE_PATH,
            kernel,
            deployment_path=str(deployment_path),
        )
        runner.load()
        runner.build_tree()
        bb = py_trees.blackboard.Client(name="test_gripper_configs")
        bb.register_key(key="gripper_configs", access=py_trees.common.Access.READ)
        assert bb.gripper_configs["primary"]["group_id"] == "gripper"
        assert bb.gripper_configs["primary"]["joint_name"] == "finger_joint"
        assert bb.gripper_configs["primary"]["touch_links"] == [
            "left_inner_finger_pad",
            "right_inner_finger_pad",
        ]

    def test_runner_without_deployment_still_works(self, kernel):
        """Existing behavior unchanged when no deployment_path."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        assert runner.deployment is None
        runner.build_tree()  # Should not raise

    def test_query_model_receives_scene_snapshot_provider(self, kernel):
        """Runner threads the configured HMI render callback into query_model nodes."""
        catalog = NodeCatalog(CATALOG_PATH)
        program = Program.from_dict(
            {
                "api_version": "manyforge.program.v0",
                "program_profile": "v0.0",
                "name": "query_model_program",
                "catalog_version": "0.1.0",
                "robot": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                },
                "blackboard": {
                    "namespaces": [
                        {
                            "id": "vlm_target",
                            "contract": "query_model_output_v1",
                            "prefix": "vlm/target",
                            "fields": [
                                {"name": "pick_left_bin", "type": "bool"},
                            ],
                        },
                    ],
                },
                "tree": {
                    "id": "query_model",
                    "name": "pick_target",
                    "params": {
                        "model_endpoint_alias": "local_vlm",
                        "prompt_template_id": "pick_target_v1",
                        "output_key_prefix": {"namespace_ref": "vlm_target"},
                        "include_scene_snapshot": True,
                    },
                },
            },
            catalog,
        )
        deployment = Deployment.from_dict(
            {
                "api_version": "manyforge.deployment.v0",
                "schema_version": "0.1.0",
                "name": "query_model_deployment",
                "program_profile": "v0.0",
                "robot_config": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "urdf_path": "package://test.urdf",
                    "global_max_tcp_linear_speed_mps": 0.25,
                },
                "adapters": {
                    "planning": {"adapter_id": "planning.test", "adapter_version": "0.1.0", "config": {}},
                    "context": {"adapter_id": "context.test", "adapter_version": "0.1.0", "config": {}},
                    "execution": {"adapter_id": "execution.test", "adapter_version": "0.1.0", "config": {}},
                },
                "orchestrator": {"engine": "py_trees", "config": {}},
                "launch": {"runner": {"module": "manyforge_behavior.runner", "args": []}},
                "model_endpoints": {
                    "local_vlm": {
                        "provider": "openai",
                        "model_id": "gemma-test",
                        "base_url": "http://localhost:8000/v1",
                    }
                },
                "prompt_templates": {
                    "pick_target_v1": {
                        "version": "1",
                        "template_text": "Pick a target",
                        "output_schema": {
                            "type": "object",
                            "required": ["pick_left_bin"],
                            "properties": {"pick_left_bin": {"type": "boolean"}},
                        },
                    }
                },
            }
        )

        def render_scene(width: int, height: int, format: str):
            return b"fake-image"

        runner = OrchestratorRunner(
            CATALOG_PATH,
            FIXTURE_PATH,
            kernel,
            program=program,
            deployment=deployment,
            scene_snapshot_provider=render_scene,
        )
        runner.load()
        tree = runner.build_tree()

        assert tree.root.name == "pick_target"
        assert getattr(tree.root, "_include_scene_snapshot") is True
        assert getattr(tree.root, "_scene_snapshot_provider") is render_scene

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_vision_guided_example_runs_on_mock_kernel(self, mock_call, kernel):
        """The shipped vision-guided example validates and executes one branch."""
        mock_call.return_value = {
            "text": '{"pick_left_bin": true, "selected_object_id": "left_part", "confidence": 0.91, "reason": "left object is clearer"}',
            "parsed": {
                "pick_left_bin": True,
                "selected_object_id": "left_part",
                "confidence": 0.91,
                "reason": "left object is clearer",
            },
            "model_version": "gemma-test",
            "usage": {"input_tokens": 12, "output_tokens": 8},
        }

        runner = OrchestratorRunner(
            CATALOG_PATH,
            VISION_PROGRAM_EXAMPLE,
            kernel,
            deployment_path=VISION_DEPLOYMENT_EXAMPLE,
            scene_snapshot_provider=lambda width, height, format: b"fake-image",
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()

        result = runner.run(max_ticks=200)

        assert result is True
        assert kernel.execute_count >= 1

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_vision_guided_pose_example_runs_on_mock_kernel(self, mock_call, kernel):
        """The shipped pose-injection example executes motion from VLM pose output."""
        mock_call.return_value = {
            "text": (
                '{"selected_object_id": "left_part", '
                '"approach_pose": {"position_m": [0.40, -0.14, 0.34], '
                '"orientation_rpy_deg": [180.0, 0.0, -90.0]}, '
                '"grasp_pose": {"position_m": [0.40, -0.14, 0.26], '
                '"orientation_rpy_deg": [180.0, 0.0, -90.0]}, '
                '"confidence": 0.92, "reason": "left part is isolated"}'
            ),
            "parsed": {
                "selected_object_id": "left_part",
                "approach_pose": {
                    "position_m": [0.40, -0.14, 0.34],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
                "grasp_pose": {
                    "position_m": [0.40, -0.14, 0.26],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
                "confidence": 0.92,
                "reason": "left part is isolated",
            },
            "model_version": "gemma-test",
            "usage": {"input_tokens": 24, "output_tokens": 16},
        }

        runner = OrchestratorRunner(
            CATALOG_PATH,
            VISION_POSE_PROGRAM_EXAMPLE,
            kernel,
            deployment_path=VISION_POSE_DEPLOYMENT_EXAMPLE,
            scene_snapshot_provider=lambda width, height, format: b"fake-image",
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()

        result = runner.run(max_ticks=200)

        assert result is True
        assert kernel.execute_count >= 4
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.move_id == "vlm_place_pose"
        assert kernel.last_execute_request.pose_goal is not None
        assert kernel.last_execute_request.pose_goal["position_m"] == [0.22, 0.0, 0.30]

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_semantic_shape_pick_example_runs_on_mock_kernel(self, mock_call, kernel):
        """The semantic shape-pick example uses deterministic planning after model selection."""
        mock_call.return_value = {
            "text": "",
            "parsed": {
                "outcome": "selected",
                "selected_object_id": "sphere_part",
                "matched_attributes": ["shape=sphere"],
                "confidence": 0.95,
                "reason": "sphere requested",
            },
            "tool_call": {
                "name": "select_scene_object",
                "arguments_text": (
                    '{"outcome": "selected", "selected_object_id": "sphere_part", '
                    '"matched_attributes": ["shape=sphere"], '
                    '"confidence": 0.95, "reason": "sphere requested"}'
                ),
                "id": "call_1",
                "raw": {},
            },
            "model_version": "gemma-test",
            "usage": {"input_tokens": 20, "output_tokens": 10},
        }

        runner = OrchestratorRunner(
            CATALOG_PATH,
            SEMANTIC_SHAPE_PROGRAM_EXAMPLE,
            kernel,
            deployment_path=SEMANTIC_SHAPE_DEPLOYMENT_EXAMPLE,
            scene_snapshot_provider=lambda width, height, format: b"fake-image",
        )
        runner.load()
        runner.build_tree()
        runner.apply_scene()

        result = runner.run(max_ticks=300)

        assert result is True
        assert kernel.execute_count >= 6
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.move_id == "planned_place_retreat"
        assert kernel.get_metrics()["plans_succeeded"] >= 2

    @patch("manyforge_behavior.nodes.model._call_model")
    def test_query_model_can_drive_motion_pose_goal_via_blackboard(self, mock_call, kernel):
        """Structured VLM output can feed a downstream move_manipulator pose goal."""
        mock_call.return_value = {
            "text": (
                '{"grasp_pose": {"position_m": [0.42, -0.05, 0.31], '
                '"orientation_rpy_deg": [180.0, 0.0, -90.0]}, "confidence": 0.94}'
            ),
            "parsed": {
                "grasp_pose": {
                    "position_m": [0.42, -0.05, 0.31],
                    "orientation_rpy_deg": [180.0, 0.0, -90.0],
                },
                "confidence": 0.94,
            },
            "model_version": "gemma-test",
            "usage": {"input_tokens": 18, "output_tokens": 12},
        }

        program = Program.from_dict(
            {
                "api_version": "manyforge.program.v0",
                "program_profile": "v0.0",
                "name": "vlm_pose_goal_program",
                "catalog_version": "0.1.0",
                "robot": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                },
                "blackboard": {
                    "namespaces": [
                        {
                            "id": "vlm_grasp",
                            "contract": "query_model_output_v1",
                            "prefix": "vlm/grasp",
                            "fields": [
                                {"name": "grasp_pose", "type": "pose"},
                                {"name": "confidence", "type": "float"},
                            ],
                        },
                    ],
                },
                "tree": {
                    "id": "sequence",
                    "name": "root",
                    "children": [
                        {
                            "id": "query_model",
                            "name": "infer_grasp_pose",
                            "params": {
                                "model_endpoint_alias": "local_vlm",
                                "prompt_template_id": "grasp_pose_v1",
                                "output_key_prefix": {"namespace_ref": "vlm_grasp"},
                                "min_confidence": 0.80,
                            },
                        },
                        {
                            "id": "move_manipulator_action",
                            "name": "execute_grasp_pose",
                            "params": {
                                "move_id": "vlm_pose_goal",
                                "motion_type": "pose_goal",
                                "pose_goal": {
                                    "field_ref": {
                                        "namespace": "vlm_grasp",
                                        "field": "grasp_pose",
                                    }
                                },
                            },
                        },
                    ],
                },
            },
            NodeCatalog(CATALOG_PATH),
        )
        deployment = Deployment.from_dict(
            {
                "api_version": "manyforge.deployment.v0",
                "schema_version": "0.1.0",
                "name": "vlm_pose_goal_deployment",
                "program_profile": "v0.0",
                "robot_config": {
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "urdf_path": "package://test.urdf",
                    "global_max_tcp_linear_speed_mps": 0.25,
                },
                "adapters": {
                    "planning": {"adapter_id": "planning.test", "adapter_version": "0.1.0", "config": {}},
                    "context": {"adapter_id": "context.test", "adapter_version": "0.1.0", "config": {}},
                    "execution": {"adapter_id": "execution.test", "adapter_version": "0.1.0", "config": {}},
                },
                "orchestrator": {"engine": "py_trees", "config": {}},
                "launch": {"runner": {"module": "manyforge_behavior.runner", "args": []}},
                "model_endpoints": {
                    "local_vlm": {
                        "provider": "openai",
                        "model_id": "gemma-test",
                        "base_url": "http://localhost:8000/v1",
                    }
                },
                "prompt_templates": {
                    "grasp_pose_v1": {
                        "version": "1",
                        "template_text": "Return a grasp pose",
                        "output_schema": {
                            "type": "object",
                            "required": ["grasp_pose", "confidence"],
                            "properties": {
                                "grasp_pose": {
                                    "type": "object",
                                    "required": ["position_m"],
                                    "properties": {
                                        "position_m": {
                                            "type": "array",
                                            "minItems": 3,
                                            "maxItems": 3,
                                            "items": {"type": "number"},
                                        },
                                        "orientation_rpy_deg": {
                                            "type": "array",
                                            "minItems": 3,
                                            "maxItems": 3,
                                            "items": {"type": "number"},
                                        },
                                        "orientation_xyzw": {
                                            "type": "array",
                                            "minItems": 4,
                                            "maxItems": 4,
                                            "items": {"type": "number"},
                                        },
                                    },
                                },
                                "confidence": {"type": "number"},
                            },
                        },
                    }
                },
            }
        )

        runner = OrchestratorRunner(
            CATALOG_PATH,
            FIXTURE_PATH,
            kernel,
            program=program,
            deployment=deployment,
        )
        runner.load()
        runner.build_tree()

        result = runner.run(max_ticks=200)

        assert result is True
        assert kernel.last_execute_request is not None
        assert kernel.last_execute_request.pose_goal is not None
        assert kernel.last_execute_request.pose_goal["position_m"] == [0.42, -0.05, 0.31]
        assert kernel.last_execute_request.pose_goal["orientation_xyzw"] == pytest.approx(
            [0.70710678, -0.70710678, -4.32978028e-17, 4.32978028e-17],
            abs=1e-6,
        )


class TestRunnerCapabilities:
    def test_runner_validates_capabilities_on_load(self, kernel):
        """Deployment with capability keys → load succeeds."""
        runner = OrchestratorRunner(
            CATALOG_PATH, FIXTURE_PATH, kernel,
            deployment_path=DEPLOYMENT_FIXTURE)
        runner.load()  # Should not raise
        assert runner.deployment is not None

    def test_runner_rejects_missing_capabilities(self, kernel):
        """Deployment missing a key → load raises error."""
        import tempfile
        import yaml
        data = {
            "api_version": "manyforge.deployment.v0",
            "schema_version": "0.1.0",
            "name": "test",
            "program_profile": "v0.0",
            "robot_config": {
                "robot_id": "ur5", "group_id": "arm",
                "urdf_path": "test.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
            },
            "adapters": {
                "planning": {
                    "adapter_id": "p",
                    "adapter_version": "0.1.0",
                    "supported_profiles": ["v0.0"],
                    "capability_keys": [
                        "planning.motion.joint_goal",
                        "planning.motion.pose_goal",
                        # Missing: planning.trajectory.time_parameterized
                    ],
                    "config": {},
                },
                "context": {
                    "adapter_id": "c",
                    "adapter_version": "0.1.0",
                    "supported_profiles": ["v0.0"],
                    "capability_keys": [
                        "context.state.current",
                        "context.validation.trajectory",
                        "context.collision.check",
                        "context.kinematics.fk",
                        "context.limits.joint",
                        "scene.shape.box",
                        "scene.shape.sphere",
                        "scene.edit.add",
                        "scene.edit.remove",
                        "scene.edit.update_pose",
                    ],
                    "config": {},
                },
                "execution": {
                    "adapter_id": "e",
                    "adapter_version": "0.1.0",
                    "supported_profiles": ["v0.0"],
                    "capability_keys": [
                        "execution.trajectory.execute",
                        "execution.feedback.phase_events",
                        "execution.stop.soft",
                        "execution.pause",
                        "execution.resume",
                        "execution.reset",
                    ],
                    "config": {},
                },
            },
            "orchestrator": {"engine": "py_trees", "config": {}},
            "launch": {"runner": {"module": "m", "args": []}},
        }
        f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
        yaml.dump(data, f)
        f.close()
        runner = OrchestratorRunner(
            CATALOG_PATH, FIXTURE_PATH, kernel,
            deployment_path=f.name)
        from manyforge_behavior.deployment import DeploymentValidationError
        with pytest.raises(DeploymentValidationError, match="Missing capability keys"):
            runner.load()
        import os
        os.unlink(f.name)
