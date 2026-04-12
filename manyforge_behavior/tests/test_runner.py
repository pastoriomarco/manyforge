"""Tests for the orchestrator runner."""

from pathlib import Path

import pytest
import py_trees
import yaml

from manyforge_behavior.kernel_client import ExecutionState, MockKernelClient
from manyforge_behavior.runner import OrchestratorRunner

CATALOG_PATH = str(Path(__file__).parent.parent / "resources" / "node_catalog.yaml")
FIXTURE_PATH = str(
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)
DEPLOYMENT_FIXTURE = str(
    Path(__file__).parent / "fixtures" / "test.deployment.yaml"
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

    def test_runner_without_deployment_still_works(self, kernel):
        """Existing behavior unchanged when no deployment_path."""
        runner = OrchestratorRunner(CATALOG_PATH, FIXTURE_PATH, kernel)
        runner.load()
        assert runner.deployment is None
        runner.build_tree()  # Should not raise


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
