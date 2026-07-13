"""Tests for the ManyForge launcher script."""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from pathlib import Path
from types import ModuleType
from unittest.mock import patch

import pytest
import yaml

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.deployment import Deployment, DeploymentValidationError
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.program import Program
from manyforge_behavior.runner import OrchestratorRunner

# Import launcher functions
from scripts.launch import _native_kernel_config_from_deployment, create_kernel, main


CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
DEPLOYMENT_FIXTURE_PATH = (
    Path(__file__).parent / "fixtures" / "ur5_roboplan.deployment.yaml"
)
PROGRAM_FIXTURE_PATH = (
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)
LITE6_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "lite6_lite_gripper_pick_and_place.deployment.yaml"
)


def _write_yaml(data: dict, suffix: str = ".yaml") -> str:
    """Write a dict to a temp YAML file and return the path."""
    f = tempfile.NamedTemporaryFile(mode="w", suffix=suffix, delete=False)
    yaml.dump(data, f)
    f.close()
    return f.name


def _write_one_joint_model(root: Path, *, urdf_name: str = "custom.urdf") -> tuple[Path, Path]:
    robots = root / "robots"
    robots.mkdir(parents=True, exist_ok=True)
    urdf = robots / urdf_name
    srdf = robots / urdf_name.replace(".urdf", ".srdf")
    urdf.write_text(
        """
        <robot name="custom">
          <link name="base"/>
          <link name="tool"/>
          <joint name="joint1" type="revolute">
            <parent link="base"/>
            <child link="tool"/>
            <axis xyz="0 0 1"/>
            <limit lower="-1" upper="1" effort="10" velocity="2"/>
          </joint>
        </robot>
        """,
        encoding="utf-8",
    )
    srdf.write_text(
        """
        <robot name="custom">
          <group name="arm">
            <joint name="joint1"/>
          </group>
        </robot>
        """,
        encoding="utf-8",
    )
    return urdf, srdf


def _minimal_deployment(**overrides) -> dict:
    """Create a minimal valid deployment dict."""
    deploy = {
        "api_version": "manyforge.deployment.v0",
        "schema_version": "0.1.0",
        "name": "test_deployment",
        "program_profile": "v0.0",
        "robot_config": {
            "robot_id": "ur5",
            "group_id": "arm",
            "urdf_path": "package://test.urdf",
            "global_max_tcp_linear_speed_mps": 0.5,
        },
        "adapters": {
            "planning": {
                "adapter_id": "planning.test",
                "adapter_version": "0.1.0",
                "config": {},
            },
            "context": {
                "adapter_id": "context.test",
                "adapter_version": "0.1.0",
                "config": {},
            },
            "execution": {
                "adapter_id": "execution.test",
                "adapter_version": "0.1.0",
                "config": {},
            },
        },
        "orchestrator": {
            "engine": "py_trees",
            "config": {},
        },
        "launch": {
            "runner": {
                "module": "manyforge_behavior.runner",
                "args": [],
            },
        },
    }
    deploy.update(overrides)
    return deploy


def _minimal_program(**overrides) -> dict:
    """Create a minimal valid program dict."""
    program = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "test_program",
        "catalog_version": "0.2.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        "tree": {
            "id": "sequence",
            "name": "root",
            "children": [
                {
                    "id": "reset_trajectories",
                    "name": "noop",
                }
            ],
        },
    }
    program.update(overrides)
    return program


class TestLauncherArgParser:
    """Tests for launcher argument parsing."""

    def test_launcher_argparser_requires_program(self):
        """Verify argparser requires --program argument."""
        parser = argparse.ArgumentParser(description="Launch a ManyForge program")
        parser.add_argument("--program", required=True, help="Path to program YAML")
        parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
        parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
        parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
        parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
        
        # This will fail because --program is missing
        with pytest.raises(SystemExit):
            parser.parse_args(["--deployment", "test.yaml", "--catalog", "catalog.yaml"])

    def test_launcher_argparser_requires_deployment(self):
        """Verify argparser requires --deployment argument."""
        parser = argparse.ArgumentParser(description="Launch a ManyForge program")
        parser.add_argument("--program", required=True, help="Path to program YAML")
        parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
        parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
        parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
        parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
        
        with pytest.raises(SystemExit):
            parser.parse_args(["--program", "test.yaml", "--catalog", "catalog.yaml"])

    def test_launcher_argparser_requires_catalog(self):
        """Verify argparser requires --catalog argument."""
        parser = argparse.ArgumentParser(description="Launch a ManyForge program")
        parser.add_argument("--program", required=True, help="Path to program YAML")
        parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
        parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
        parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
        parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
        
        with pytest.raises(SystemExit):
            parser.parse_args(["--program", "test.yaml", "--deployment", "deploy.yaml"])

    def test_launcher_argparser_accepts_all_args(self):
        """Verify argparser accepts all required arguments."""
        parser = argparse.ArgumentParser(description="Launch a ManyForge program")
        parser.add_argument("--program", required=True, help="Path to program YAML")
        parser.add_argument("--deployment", required=True, help="Path to deployment YAML")
        parser.add_argument("--catalog", required=True, help="Path to node catalog YAML")
        parser.add_argument("--auto-run", action="store_true", help="Run immediately without waiting")
        parser.add_argument("--max-ticks", type=int, default=1000, help="Max tree ticks")
        
        args = parser.parse_args([
            "--program", "test.yaml",
            "--deployment", "deploy.yaml",
            "--catalog", "catalog.yaml",
            "--auto-run",
            "--max-ticks", "100",
        ])
        
        assert args.program == "test.yaml"
        assert args.deployment == "deploy.yaml"
        assert args.catalog == "catalog.yaml"
        assert args.auto_run is True
        assert args.max_ticks == 100


class TestLauncherKernelCreation:
    """Tests for kernel creation logic."""

    def test_native_kernel_config_forwards_planning_budget_and_policy_snapshot(self):
        data = _minimal_deployment(
            contact_policies=[
                {
                    "policy_id": "deployment_baseline",
                    "require_full_diagnostics": True,
                }
            ]
        )
        data["orchestrator"]["config"] = {
            "validation_collision_query_timeout_ms": 275.0,
            "validation_collision_wall_timeout_ms": 3250.0,
        }
        data["adapters"]["planning"]["config"] = {
            "planner_max_planning_time_s": 9.0,
            "rng_seed": 12345,
        }
        data["adapters"]["execution"]["config"] = {
            "single_shot_ik_timeout_s": 0.04
        }
        deployment = Deployment.from_dict(data)

        config = _native_kernel_config_from_deployment(deployment)

        assert config["planner_max_planning_time_s"] == 9.0
        assert config["rng_seed"] == 12345
        assert config["single_shot_ik_timeout_s"] == 0.04
        assert config["validation_collision_query_timeout_ns"] == 275_000_000
        assert config["validation_collision_wall_timeout_ns"] == 3_250_000_000
        assert config["baseline_collision_policy_snapshot"]["policy_id"] == (
            "deployment_baseline"
        )

    def test_create_kernel_returns_mock_for_unknown_adapter(self):
        """When adapter is not roboplan, return MockKernelClient."""
        data = _minimal_deployment()
        data["adapters"]["planning"]["adapter_id"] = "planning.unknown"
        path = _write_yaml(data)
        
        try:
            deployment = Deployment(path)
            kernel = create_kernel(deployment)
            
            assert isinstance(kernel, MockKernelClient)
        finally:
            os.unlink(path)

    def test_create_kernel_returns_mock_when_roboplan_not_available(self):
        """When roboplan adapter but module not available, return MockKernelClient."""
        data = _minimal_deployment()
        data["adapters"]["planning"]["adapter_id"] = "planning.roboplan"
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
        ]
        deploy_path = _write_yaml(data)

        try:
            deployment = Deployment(deploy_path)
            # Force ImportError for manyforge_roboplan_py to test fallback path
            with patch.dict(sys.modules, {"manyforge_roboplan_py": None}):
                kernel = create_kernel(deployment)

            # Should fall back to MockKernelClient since manyforge_roboplan_py is not available
            assert isinstance(kernel, MockKernelClient)
        finally:
            os.unlink(deploy_path)

    def test_create_kernel_rejects_ros2_control_execution(self):
        """Standalone launcher does not own ROS 2 controller runtime wiring."""
        deployment = Deployment(DEPLOYMENT_FIXTURE_PATH)

        with pytest.raises(RuntimeError, match="ROS 2 controller execution"):
            create_kernel(deployment)

    def test_create_kernel_uses_explicit_bundle_paths_for_custom_model(self, tmp_path):
        _write_one_joint_model(tmp_path)
        deploy_path = tmp_path / "deployment.yaml"
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "robots/custom.urdf",
                "srdf_path": "robots/custom.srdf",
                "package_paths": ["./assets", "../shared_assets"],
                "global_max_tcp_linear_speed_mps": 0.5,
            },
            adapters={
                "planning": {
                    "adapter_id": "planning.roboplan",
                    "adapter_version": "0.1.0",
                    "capability_keys": [
                        "planning.motion.joint_goal",
                        "planning.motion.pose_goal",
                        "planning.trajectory.time_parameterized",
                    ],
                    "config": {},
                },
                "context": {
                    "adapter_id": "context.test",
                    "adapter_version": "0.1.0",
                    "config": {},
                },
                "execution": {
                    "adapter_id": "execution.test",
                    "adapter_version": "0.1.0",
                    "config": {},
                },
            },
        )
        deploy_path.write_text(yaml.safe_dump(data), encoding="utf-8")

        recorded: dict[str, object] = {}
        fake_bundle = object()

        fake_roboplan = ModuleType("manyforge_roboplan_py")

        def fake_create_native_kernel_from_paths(
            model_name,
            group_name,
            urdf_path,
            srdf_path,
            package_paths,
            config,
            logical_robot_id,
        ):
            recorded["model_name"] = model_name
            recorded["group_name"] = group_name
            recorded["urdf_path"] = urdf_path
            recorded["srdf_path"] = srdf_path
            recorded["package_paths"] = package_paths
            recorded["config"] = config
            recorded["logical_robot_id"] = logical_robot_id
            return fake_bundle

        fake_roboplan.create_native_kernel_from_paths = fake_create_native_kernel_from_paths

        fake_client_module = ModuleType("native_kernel_client")

        class FakeNativeKernelClient:
            def __init__(self, bundle, **kwargs):
                self.bundle = bundle
                self.kwargs = kwargs

        fake_client_module.NativeKernelClient = FakeNativeKernelClient

        deployment = Deployment(deploy_path)
        with patch.dict(
            sys.modules,
            {
                "manyforge_roboplan_py": fake_roboplan,
                "native_kernel_client": fake_client_module,
            },
        ), patch("scripts.launch.default_package_paths", return_value=["/usr/local/share"]):
            kernel = create_kernel(deployment)

        assert isinstance(kernel, FakeNativeKernelClient)
        assert kernel.bundle is fake_bundle
        assert recorded["model_name"] == "custom"
        assert recorded["group_name"] == "arm"
        assert recorded["logical_robot_id"] == "ur5"
        assert recorded["urdf_path"] == str(tmp_path / "robots" / "custom.urdf")
        assert recorded["srdf_path"] == str(tmp_path / "robots" / "custom.srdf")
        assert recorded["package_paths"] == [
            str((tmp_path / "assets").resolve()),
            str((tmp_path / "../shared_assets").resolve()),
        ]
        assert recorded["config"] == {}
        assert kernel.robot_model_authority_metadata["view_changed"] is False
        assert kernel.robot_model_authority_metadata["canonical_urdf_path"] == (
            str(tmp_path / "robots" / "custom.urdf")
        )

    def test_create_kernel_routes_lite6_through_private_roboplan_view(self):
        recorded: dict[str, object] = {}
        fake_bundle = object()

        fake_roboplan = ModuleType("manyforge_roboplan_py")

        def fake_create_native_kernel_from_paths(
            model_name,
            group_name,
            urdf_path,
            srdf_path,
            package_paths,
            config,
            logical_robot_id,
        ):
            recorded["model_name"] = model_name
            recorded["group_name"] = group_name
            recorded["urdf_path"] = urdf_path
            recorded["srdf_path"] = srdf_path
            recorded["package_paths"] = package_paths
            recorded["config"] = config
            recorded["logical_robot_id"] = logical_robot_id
            return fake_bundle

        fake_roboplan.create_native_kernel_from_paths = fake_create_native_kernel_from_paths

        fake_client_module = ModuleType("native_kernel_client")

        class FakeNativeKernelClient:
            def __init__(self, bundle, **kwargs):
                self.bundle = bundle
                self.kwargs = kwargs

        fake_client_module.NativeKernelClient = FakeNativeKernelClient

        deployment = Deployment(LITE6_DEPLOYMENT_PATH)
        with patch.dict(
            sys.modules,
            {
                "manyforge_roboplan_py": fake_roboplan,
                "native_kernel_client": fake_client_module,
            },
        ):
            kernel = create_kernel(deployment)

        assert isinstance(kernel, FakeNativeKernelClient)
        assert recorded["model_name"] == "lite6_lite_gripper"
        assert recorded["group_name"] == "lite6"
        assert recorded["logical_robot_id"] == "lite6"
        assert recorded["urdf_path"] != str(
            LITE6_DEPLOYMENT_PATH.parent
            / "../manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf"
        )
        assert Path(str(recorded["urdf_path"])).exists()
        assert "__rma_mimic_" in Path(str(recorded["urdf_path"])).read_text(
            encoding="utf-8"
        )
        metadata = kernel.robot_model_authority_metadata
        assert kernel.kwargs["robot_model_authority_metadata"] == metadata
        assert kernel.kwargs["robot_model_authority_temp_dir"] is not None
        assert metadata["view_changed"] is True
        assert metadata["canonical_urdf_path"].endswith(
            "lite6_lite_gripper/lite6_lite_gripper.urdf"
        )
        assert metadata["view_urdf_path"] == recorded["urdf_path"]
        assert metadata["canonical_to_view_joint_names"]["right_finger_joint"] != (
            "right_finger_joint"
        )


class TestLauncherValidation:
    """Tests for launcher validation logic."""

    def test_launcher_validates_deployment(self):
        """Valid deployment + program → no error."""
        deployment = Deployment(DEPLOYMENT_FIXTURE_PATH)
        catalog = NodeCatalog(CATALOG_PATH)
        program = Program(PROGRAM_FIXTURE_PATH, catalog)
        
        # Cross-validate should not raise
        deployment.validate_against_program(program)
        deployment.validate_capabilities()

    def test_launcher_rejects_invalid_deployment(self):
        """Bad deployment → error."""
        # Create deployment with wrong robot_id
        data = _minimal_deployment()
        data["robot_config"]["robot_id"] = "ur10"  # Different from program
        deploy_path = _write_yaml(data)
        
        try:
            deployment = Deployment(deploy_path)
            catalog = NodeCatalog(CATALOG_PATH)
            program = Program(PROGRAM_FIXTURE_PATH, catalog)
            
            # Should raise DeploymentValidationError
            with pytest.raises(DeploymentValidationError, match="robot_id"):
                deployment.validate_against_program(program)
        finally:
            os.unlink(deploy_path)

    def test_launcher_rejects_profile_mismatch(self):
        """Deployment with wrong profile → error."""
        data = _minimal_deployment(program_profile="v0.1")
        deploy_path = _write_yaml(data)
        
        try:
            deployment = Deployment(deploy_path)
            catalog = NodeCatalog(CATALOG_PATH)
            program = Program(PROGRAM_FIXTURE_PATH, catalog)
            
            with pytest.raises(DeploymentValidationError, match="program_profile"):
                deployment.validate_against_program(program)
        finally:
            os.unlink(deploy_path)

    def test_launcher_rejects_missing_capabilities(self):
        """Deployment with missing capabilities → error."""
        data = _minimal_deployment()
        # Add capability_keys but miss one required key
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            # Missing: planning.trajectory.time_parameterized
        ]
        data["adapters"]["context"]["capability_keys"] = [
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
        ]
        data["adapters"]["execution"]["capability_keys"] = [
            "execution.trajectory.execute",
            "execution.feedback.phase_events",
            "execution.feedback.state_snapshots",
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]
        deploy_path = _write_yaml(data)
        
        try:
            deployment = Deployment(deploy_path)
            
            with pytest.raises(DeploymentValidationError, match="Missing capability keys"):
                deployment.validate_capabilities()
        finally:
            os.unlink(deploy_path)


class TestLauncherIntegration:
    """Integration tests for launcher components."""

    def test_full_launch_sequence(self):
        """Test the full launch sequence without auto-run."""
        # This test verifies that all components can be loaded and validated
        # together without actually running the program.
        
        # Load deployment
        deployment = Deployment(DEPLOYMENT_FIXTURE_PATH)
        
        # Load catalog
        catalog = NodeCatalog(CATALOG_PATH)
        
        # Load program
        program = Program(PROGRAM_FIXTURE_PATH, catalog)
        
        # Cross-validate
        deployment.validate_against_program(program)
        deployment.validate_capabilities()
        
        # Use an injected mock kernel here. The fixture intentionally selects
        # ROS 2 controller execution, which is owned by Composer's ROS-capable
        # runtime profile rather than this standalone launcher.
        kernel = MockKernelClient()

        # Create runner
        runner = OrchestratorRunner(
            catalog_path=str(CATALOG_PATH),
            program_path=str(PROGRAM_FIXTURE_PATH),
            kernel=kernel,
            deployment_path=str(DEPLOYMENT_FIXTURE_PATH),
        )
        
        # Load and build
        runner.load()
        runner.build_tree()
        
        # Apply scene
        runner.apply_scene()
        
        # Verify scene was applied
        assert len(kernel.scene_edits) == 1
        assert kernel.scene_edits[0].operation == "add"
        assert kernel.scene_edits[0].object_id == "table"

    def test_runner_with_deployment(self):
        """Runner loads deployment and validates against program."""
        kernel = MockKernelClient()
        runner = OrchestratorRunner(
            str(CATALOG_PATH),
            str(PROGRAM_FIXTURE_PATH),
            kernel,
            deployment_path=str(DEPLOYMENT_FIXTURE_PATH),
        )
        runner.load()
        
        assert runner.deployment is not None
        assert runner.deployment.robot_config.global_max_tcp_linear_speed_mps == 0.25
