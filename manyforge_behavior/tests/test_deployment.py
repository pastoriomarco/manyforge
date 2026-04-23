"""Tests for deployment artifact loading and validation."""

import os
import tempfile
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.deployment import (
    Deployment,
    DeploymentValidationError,
)
from manyforge_behavior.program import Program

CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
DEPLOYMENT_FIXTURE_PATH = (
    Path(__file__).parent / "fixtures" / "ur5_roboplan.deployment.yaml"
)
PROGRAM_FIXTURE_PATH = (
    Path(__file__).parent / "fixtures" / "pick_and_place.program.yaml"
)
EXAMPLE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "pick_and_place.deployment.yaml"
)
EXAMPLE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "pick_and_place.program.yaml"
)
UR10E_ROBOTIQ_EXAMPLE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "pick_and_place_ur10e_robotiq.deployment.yaml"
)
UR10E_ROBOTIQ_EXAMPLE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "pick_and_place_ur10e_robotiq.program.yaml"
)
INTERRUPT_EXAMPLE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "interrupt_pick_and_place.deployment.yaml"
)
INTERRUPT_EXAMPLE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2] / "examples" / "interrupt_pick_and_place.program.yaml"
)


@pytest.fixture
def catalog():
    return NodeCatalog(CATALOG_PATH)


def _minimal_tree() -> dict:
    """A minimal valid tree that passes catalog validation."""
    return {
        "id": "sequence",
        "name": "root",
        "children": [
            {
                "id": "reset_trajectories",
                "name": "noop",
            }
        ],
    }


def _write_deployment(data: dict) -> str:
    f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.dump(data, f)
    f.close()
    return f.name


def _minimal_program(**overrides) -> dict:
    program = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "test_program",
        "catalog_version": "0.1.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        "tree": _minimal_tree(),
    }
    program.update(overrides)
    return program


def _minimal_deployment(**overrides) -> dict:
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


class TestDeploymentLoading:
    def test_load_valid_deployment(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.name == "ur5_roboplan_local"
        assert deploy.description == "Local deployment with RoboPlan planner and ros2_control JTC."
        assert deploy.api_version == "manyforge.deployment.v0"
        assert deploy.schema_version == "0.1.0"
        assert deploy.program_profile == "v0.0"

    def test_all_fields_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.name == "ur5_roboplan_local"
        assert deploy.description == "Local deployment with RoboPlan planner and ros2_control JTC."
        assert deploy.api_version == "manyforge.deployment.v0"
        assert deploy.schema_version == "0.1.0"
        assert deploy.program_profile == "v0.0"
        assert deploy.robot_config is not None
        assert len(deploy.adapters) == 3
        assert deploy.orchestrator is not None
        assert deploy.launch is not None

    def test_robot_config_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.robot_config.robot_id == "ur5"
        assert deploy.robot_config.group_id == "arm"
        assert deploy.robot_config.urdf_path == "package://roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf"
        assert deploy.robot_config.srdf_path == "package://roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf"
        assert deploy.robot_config.package_paths == []
        assert deploy.robot_config.global_max_tcp_linear_speed_mps == 0.25

    def test_robot_config_package_paths_parsed(self):
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://custom_pkg/models/robot.urdf",
                "srdf_path": "package://custom_pkg/models/robot.srdf",
                "package_paths": ["./assets", "/opt/shared_models"],
                "global_max_tcp_linear_speed_mps": 0.5,
            }
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            assert deploy.robot_config.package_paths == ["./assets", "/opt/shared_models"]
        finally:
            os.unlink(path)

    def test_grippers_parsed(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "simulated_joint",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "open_position": 0.0,
                    "closed_position": 0.7,
                    "motion_time_s": 0.8,
                    "touch_links": ["left_inner_finger_pad", "right_inner_finger_pad"],
                    "mount_frame": "tool0",
                    "attach_frame": "grasp_frame",
                    "tcp_frame": "grasp_frame",
                    "auxiliary_frames": ["gripper_frame", "insertion_frame"],
                }
            }
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            assert "primary" in deploy.grippers
            assert deploy.grippers["primary"].group_id == "gripper"
            assert deploy.grippers["primary"].joint_name == "finger_joint"
            assert deploy.grippers["primary"].closed_position == pytest.approx(0.7)
            assert deploy.grippers["primary"].touch_links == [
                "left_inner_finger_pad",
                "right_inner_finger_pad",
            ]
            assert deploy.grippers["primary"].mount_frame == "tool0"
            assert deploy.grippers["primary"].attach_frame == "grasp_frame"
            assert deploy.grippers["primary"].tcp_frame == "grasp_frame"
            assert deploy.grippers["primary"].auxiliary_frames == [
                "gripper_frame",
                "insertion_frame",
            ]
        finally:
            os.unlink(path)

    def test_adapters_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert "planning" in deploy.adapters
        assert "context" in deploy.adapters
        assert "execution" in deploy.adapters
        assert deploy.adapters["planning"].adapter_id == "planning.roboplan"
        assert deploy.adapters["planning"].adapter_version == "0.1.0"
        assert deploy.adapters["planning"].config["rng_seed"] == 12345
        assert deploy.adapters["context"].adapter_id == "planning.roboplan_context"
        assert deploy.adapters["execution"].adapter_id == "execution.ros2_control_fjt"

    def test_orchestrator_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.orchestrator.engine == "py_trees"
        assert deploy.orchestrator.config == {"collision_monitor_period_ms": 10.0}

    def test_launch_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.launch.runner.module == "manyforge_behavior.runner"
        assert deploy.launch.runner.args == []
        assert deploy.launch.ui_backend is not None
        assert deploy.launch.ui_backend.module == "manyforge_hmi.backend"
        assert deploy.launch.ui_backend.enabled is True

    def test_interrupt_sources_parsed(self):
        data = _minimal_deployment(
            interrupt_sources={
                "operator_pause": {
                    "adapter": "composer_ui",
                    "signal": "pause_request",
                    "kind": "edge",
                    "priority": 30,
                    "maskable": True,
                },
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            assert "operator_pause" in deploy.interrupt_sources
            assert deploy.interrupt_sources["operator_pause"].kind == "edge"
        finally:
            os.unlink(path)

    def test_authority_matrix_parsed(self):
        data = _minimal_deployment(
            authority_matrix={
                "motion": {
                    "owner": "manyforge",
                    "on_disconnect": "hold",
                    "description": "ManyForge owns motion commands during runtime.",
                },
                "plc_signals": {
                    "owner": "plc",
                    "on_disconnect": "latched_fault",
                },
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            assert deploy.authority_matrix["motion"].owner == "manyforge"
            assert deploy.authority_matrix["motion"].on_disconnect == "hold"
            assert deploy.authority_matrix["plc_signals"].owner == "plc"
        finally:
            os.unlink(path)


class TestDeploymentValidation:
    def test_invalid_api_version(self):
        path = _write_deployment(_minimal_deployment(api_version="wrong"))
        with pytest.raises(DeploymentValidationError, match="api_version"):
            Deployment(path)
        os.unlink(path)

    def test_invalid_authority_matrix_domain(self):
        path = _write_deployment(
            _minimal_deployment(
                authority_matrix={
                    "camera": {
                        "owner": "manyforge",
                        "on_disconnect": "hold",
                    }
                },
            )
        )
        with pytest.raises(DeploymentValidationError, match="supported domain"):
            Deployment(path)
        os.unlink(path)

    def test_invalid_authority_matrix_owner(self):
        path = _write_deployment(
            _minimal_deployment(
                authority_matrix={
                    "motion": {
                        "owner": "robot",
                        "on_disconnect": "hold",
                    }
                },
            )
        )
        with pytest.raises(DeploymentValidationError, match="owner must be one of"):
            Deployment(path)
        os.unlink(path)

    def test_missing_program_profile(self):
        path = _write_deployment(_minimal_deployment(program_profile=""))
        with pytest.raises(DeploymentValidationError, match="program_profile"):
            Deployment(path)
        os.unlink(path)

    def test_missing_robot_config(self):
        data = _minimal_deployment()
        del data["robot_config"]
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="robot_config"):
            Deployment(path)
        os.unlink(path)

    def test_missing_adapter_slot(self):
        data = _minimal_deployment()
        del data["adapters"]["planning"]
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="adapters.planning"):
            Deployment(path)
        os.unlink(path)

    def test_missing_adapter_id(self):
        data = _minimal_deployment()
        del data["adapters"]["planning"]["adapter_id"]
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="adapter_id"):
            Deployment(path)
        os.unlink(path)

    def test_negative_tcp_speed(self):
        data = _minimal_deployment()
        data["robot_config"]["global_max_tcp_linear_speed_mps"] = -1.0
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="global_max_tcp_linear_speed_mps"):
            Deployment(path)
        os.unlink(path)

    def test_zero_tcp_speed(self):
        data = _minimal_deployment()
        data["robot_config"]["global_max_tcp_linear_speed_mps"] = 0.0
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="global_max_tcp_linear_speed_mps"):
            Deployment(path)
        os.unlink(path)

    def test_optional_description(self):
        data = _minimal_deployment()
        del data["name"]
        path = _write_deployment(data)
        deploy = Deployment(path)
        assert deploy.name == ""
        os.unlink(path)

    def test_optional_srdf_path(self):
        data = _minimal_deployment()
        # srdf_path is not in minimal deployment, so it should default to ""
        path = _write_deployment(data)
        deploy = Deployment(path)
        assert deploy.robot_config.srdf_path == ""
        os.unlink(path)

    def test_invalid_robot_config_package_paths(self):
        data = _minimal_deployment()
        data["robot_config"]["package_paths"] = "not-a-list"
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="package_paths"):
            Deployment(path)
        os.unlink(path)

    def test_update_from_dict_validates_authority_matrix(self):
        deploy = Deployment.from_dict(_minimal_deployment())
        with pytest.raises(DeploymentValidationError, match="owner must be one of"):
            deploy.update_from_dict(
                {
                    "authority_matrix": {
                        "motion": {
                            "owner": "robot",
                            "on_disconnect": "hold",
                        }
                    }
                }
            )

    def test_update_from_dict_validates_interrupt_sources(self):
        deploy = Deployment.from_dict(_minimal_deployment())
        with pytest.raises(DeploymentValidationError, match="kind must be one of"):
            deploy.update_from_dict(
                {
                    "interrupt_sources": {
                        "operator_pause": {
                            "adapter": "composer_ui",
                            "signal": "pause_request",
                            "kind": "pulse",
                            "priority": 30,
                            "maskable": True,
                        }
                    }
                }
            )

    def test_update_from_dict_validates_robot_config_package_paths(self):
        deploy = Deployment.from_dict(_minimal_deployment())
        deploy.update_from_dict(
            {
                "robot_config": {
                    "srdf_path": "package://custom_pkg/models/robot.srdf",
                    "package_paths": ["./robot_models", "../shared_models"],
                }
            }
        )

        assert deploy.robot_config.srdf_path == "package://custom_pkg/models/robot.srdf"
        assert deploy.robot_config.package_paths == ["./robot_models", "../shared_models"]

    def test_update_from_dict_rejects_invalid_robot_config_package_paths(self):
        deploy = Deployment.from_dict(_minimal_deployment())
        with pytest.raises(DeploymentValidationError, match="package_paths"):
            deploy.update_from_dict(
                {
                    "robot_config": {
                        "package_paths": ["/valid", ""],
                    }
                }
            )

    def test_optional_ui_backend(self):
        data = _minimal_deployment()
        # ui_backend is not in minimal deployment, so it should be None
        path = _write_deployment(data)
        deploy = Deployment(path)
        assert deploy.launch.ui_backend is None
        os.unlink(path)

    def test_rejects_interrupt_source_with_invalid_kind(self):
        data = _minimal_deployment(
            interrupt_sources={
                "safety_stop": {
                    "adapter": "plc",
                    "signal": "safety.stop",
                    "kind": "pulse",
                    "priority": 100,
                    "maskable": False,
                },
            },
        )
        path = _write_deployment(data)
        with pytest.raises(DeploymentValidationError, match="kind must be one of"):
            Deployment(path)
        os.unlink(path)


class TestCrossArtifactValidation:
    def test_validate_against_program_success(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        prog = Program(PROGRAM_FIXTURE_PATH, catalog)
        deploy.validate_against_program(prog)

    def test_interrupt_example_program_and_deployment_validate(self, catalog):
        deploy = Deployment(INTERRUPT_EXAMPLE_DEPLOYMENT_PATH)
        prog = Program(INTERRUPT_EXAMPLE_PROGRAM_PATH, catalog)
        deploy.validate_against_program(prog)
        assert "operator_pause" in deploy.interrupt_sources
        assert "operator_pause_hold" in prog.interrupt_policies
        assert "operator_pause_hold_handler" in prog.interrupt_handlers

    def test_validate_profile_mismatch(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.1",  # different from deployment
            "name": "test",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": _minimal_tree(),
        }
        path = _write_deployment(data)
        prog = Program(path, catalog)
        with pytest.raises(DeploymentValidationError, match="program_profile"):
            deploy.validate_against_program(prog)
        os.unlink(path)

    def test_validate_interrupt_source_reference(self, catalog):
        deployment_data = _minimal_deployment(
            interrupt_sources={
                "operator_pause": {
                    "adapter": "composer_ui",
                    "signal": "pause_request",
                    "kind": "edge",
                    "priority": 30,
                    "maskable": True,
                },
            },
        )
        program_data = _minimal_program(
            interrupt_handlers={
                "pause_announce": {
                    "root": {
                        "id": "sequence",
                        "name": "pause_announce",
                        "children": [
                            {"id": "reset_trajectories", "name": "noop"},
                        ],
                    },
                },
            },
            interrupt_policies=[
                {
                    "id": "pause_hold",
                    "source": "operator_pause",
                    "applies_to": {"subtree_root": "root"},
                    "preemption_mode": "hold_cycle",
                    "handler": "pause_announce",
                    "after_handler": "resumable_hold",
                },
            ],
        )
        deploy_path = _write_deployment(deployment_data)
        program_path = _write_deployment(program_data)
        try:
            deploy = Deployment(deploy_path)
            prog = Program(program_path, catalog)
            deploy.validate_against_program(prog)
        finally:
            os.unlink(deploy_path)
            os.unlink(program_path)

    def test_rejects_program_interrupt_policy_for_unknown_deployment_source(self, catalog):
        deployment_data = _minimal_deployment()
        program_data = _minimal_program(
            interrupt_handlers={
                "pause_announce": {
                    "root": {
                        "id": "sequence",
                        "name": "pause_announce",
                        "children": [
                            {"id": "reset_trajectories", "name": "noop"},
                        ],
                    },
                },
            },
            interrupt_policies=[
                {
                    "id": "pause_hold",
                    "source": "operator_pause",
                    "applies_to": {"subtree_root": "root"},
                    "preemption_mode": "hold_cycle",
                    "handler": "pause_announce",
                    "after_handler": "resumable_hold",
                },
            ],
        )
        deploy_path = _write_deployment(deployment_data)
        program_path = _write_deployment(program_data)
        try:
            deploy = Deployment(deploy_path)
            prog = Program(program_path, catalog)
            with pytest.raises(DeploymentValidationError, match="undeclared deployment interrupt source"):
                deploy.validate_against_program(prog)
        finally:
            os.unlink(deploy_path)
            os.unlink(program_path)

    def test_validate_robot_id_mismatch(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "test",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur10",  # different from deployment
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": _minimal_tree(),
        }
        path = _write_deployment(data)
        prog = Program(path, catalog)
        with pytest.raises(DeploymentValidationError, match="robot_id"):
            deploy.validate_against_program(prog)
        os.unlink(path)

    def test_validate_group_id_mismatch(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "test",
            "catalog_version": "0.1.0",
            "robot": {
                "robot_id": "ur5",
                "group_id": "manipulator",  # different from deployment
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": _minimal_tree(),
        }
        path = _write_deployment(data)
        prog = Program(path, catalog)
        with pytest.raises(DeploymentValidationError, match="group_id"):
            deploy.validate_against_program(prog)
        os.unlink(path)

    def test_validate_against_example_v0_1_program_success(self, catalog):
        """The real pick-and-place demo deployment matches the v0.1 example program."""
        deploy = Deployment(EXAMPLE_DEPLOYMENT_PATH)
        prog = Program(EXAMPLE_PROGRAM_PATH, catalog)
        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    def test_validate_against_ur10e_robotiq_preview_program_success(self, catalog):
        """The UR10e + Robotiq preview deployment matches its paired example program."""
        deploy = Deployment(UR10E_ROBOTIQ_EXAMPLE_DEPLOYMENT_PATH)
        prog = Program(UR10E_ROBOTIQ_EXAMPLE_PROGRAM_PATH, catalog)
        assert "primary" in deploy.grippers
        assert deploy.grippers["primary"].command_mode == "simulated_joint"
        assert deploy.grippers["primary"].mount_frame == "tool0"
        assert deploy.grippers["primary"].attach_frame == "grasp_frame"
        assert deploy.grippers["primary"].tcp_frame == "grasp_frame"
        assert deploy.grippers["primary"].auxiliary_frames == ["gripper_frame"]
        deploy.validate_against_program(prog)
        deploy.validate_capabilities()


class TestAdapterCapabilityKeys:
    def test_adapter_capability_keys_parsed(self):
        """Load fixture, verify capability_keys are on each adapter."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        
        # Planning adapter
        assert deploy.adapters["planning"].supported_profiles == ["v0.0"]
        assert deploy.adapters["planning"].capability_keys == [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
        ]
        
        # Context adapter
        assert deploy.adapters["context"].supported_profiles == ["v0.0"]
        assert deploy.adapters["context"].capability_keys == [
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
        
        # Execution adapter
        assert deploy.adapters["execution"].supported_profiles == ["v0.0"]
        assert deploy.adapters["execution"].capability_keys == [
            "execution.trajectory.execute",
            "execution.feedback.phase_events",
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]


class TestValidateCapabilities:
    def test_validate_capabilities_success(self):
        """Full coverage → no error."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        deploy.validate_capabilities()  # Should not raise

    def test_validate_capabilities_missing_key(self):
        """Remove one key from fixture → DeploymentValidationError."""
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
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]
        path = _write_deployment(data)
        deploy = Deployment(path)
        with pytest.raises(DeploymentValidationError, match="Missing capability keys"):
            deploy.validate_capabilities()
        os.unlink(path)

    def test_validate_capabilities_wrong_profile(self):
        """Adapter lists wrong profile → DeploymentValidationError."""
        data = _minimal_deployment()
        data["adapters"]["planning"]["supported_profiles"] = ["v99.0"]  # Wrong profile
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
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
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]
        path = _write_deployment(data)
        deploy = Deployment(path)
        with pytest.raises(DeploymentValidationError, match="supported_profiles"):
            deploy.validate_capabilities()
        os.unlink(path)

    def test_validate_capabilities_backwards_compat(self):
        """No capability_keys on any adapter → passes silently."""
        data = _minimal_deployment()
        # Don't add capability_keys to any adapter
        path = _write_deployment(data)
        deploy = Deployment(path)
        deploy.validate_capabilities()  # Should not raise (backwards compat)
        os.unlink(path)

    def test_validate_capabilities_partial_coverage(self):
        """Some adapters have keys, missing total coverage → error."""
        data = _minimal_deployment()
        # Only planning adapter has capability_keys, others don't
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
        ]
        # context and execution have no capability_keys
        path = _write_deployment(data)
        deploy = Deployment(path)
        with pytest.raises(DeploymentValidationError, match="Missing capability keys"):
            deploy.validate_capabilities()
        os.unlink(path)

    def test_validate_capabilities_checks_program_node_requirements(self, catalog):
        data = _minimal_deployment()
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
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
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]
        deploy_path = _write_deployment(data)
        program_path = _write_deployment(
            _minimal_program(
                tree={
                    "id": "sequence",
                    "name": "root",
                    "children": [
                        {
                            "id": "command_gripper",
                            "name": "close_gripper",
                            "params": {
                                "gripper_id": "primary",
                                "command": "close",
                            },
                        }
                    ],
                }
            )
        )
        try:
            deploy = Deployment(deploy_path)
            program = Program(program_path, catalog)
            with pytest.raises(
                DeploymentValidationError,
                match="Missing capability keys for program nodes",
            ):
                deploy.validate_capabilities(program)
        finally:
            os.unlink(deploy_path)
            os.unlink(program_path)
