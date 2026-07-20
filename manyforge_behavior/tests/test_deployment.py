"""Tests for deployment artifact loading and validation."""

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest
import yaml

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.capabilities import (
    get_adapter_capability_manifest,
    get_required_keys,
)
from manyforge_behavior.deployment import (
    DEFAULT_GLOBAL_MAX_TCP_LINEAR_SPEED_MPS,
    Deployment,
    DeploymentValidationError,
    RobotConfig,
)
from manyforge_behavior.program import Program
from manyforge_behavior.skill_loader import (
    SkillCatalog,
    default_skills_dir,
    load_skill,
    load_skill_catalog,
)

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
ISAAC_LIGHT_ROS2_CONTROL_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_manipulator_scene_light_ros2_control.deployment.yaml"
)
ISAAC_LIGHT_ROS2_CONTROL_COLLISION_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_manipulator_scene_light_ros2_control_collision.deployment.yaml"
)
ISAAC_LIGHT_ASSISTANT_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_manipulator_scene_light_assistant.deployment.yaml"
)
ISAAC_ROBOTIQ_GRIPPER_BRIDGE_CONFIG_PATH = (
    Path(__file__).resolve().parents[2]
    / "manyforge_ros_bringup"
    / "config"
    / "isaac_robotiq_2f_140_gripper_bridge.yaml"
)
ISAAC_LIGHT_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_manipulator_scene_light.program.yaml"
)
ISAAC_LITE6_PICK_PLACE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_lite6_lite_gripper_pick_and_place.deployment.yaml"
)
ISAAC_LITE6_PICK_PLACE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_lite6_lite_gripper_pick_and_place.program.yaml"
)
TROCAR_LITE6_FOUNDATIONPOSE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "trocar_lite6_foundationpose_pick.deployment.yaml"
)
TROCAR_LITE6_FOUNDATIONPOSE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "trocar_lite6_foundationpose_pick.program.yaml"
)
LITE6_PICK_PLACE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "lite6_lite_gripper_pick_and_place.deployment.yaml"
)
LITE6_PICK_PLACE_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "lite6_lite_gripper_pick_and_place.program.yaml"
)
LITE6_MODEL_DIR = (
    Path(__file__).resolve().parents[2]
    / "manyforge_example_models"
    / "models"
    / "lite6_lite_gripper"
)
LITE6_STATIC_MANIFEST_PATH = LITE6_MODEL_DIR / "STATIC_MANIFEST.json"
LITE6_RUNTIME_CAPTURE_DIR = LITE6_MODEL_DIR / "manymove_runtime_capture"
LITE6_RUNTIME_CAPTURE_JSON_PATH = (
    LITE6_RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime_capture.json"
)
LITE6_RUNTIME_CAPTURE_URDF_PATH = (
    LITE6_RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime.urdf"
)
LITE6_RUNTIME_CAPTURE_SRDF_PATH = (
    LITE6_RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime.srdf"
)
LITE6_ISAAC_ROS2_CONTROL_URDF_PATH = (
    Path(__file__).resolve().parents[2]
    / "manyforge_ros_bringup"
    / "urdf"
    / "isaac_lite6_topic_based.ros2_control.urdf"
)
LITE6_ISAAC_CONTROLLER_CONFIG_PATH = (
    Path(__file__).resolve().parents[2]
    / "manyforge_ros_bringup"
    / "config"
    / "isaac_lite6_topic_based_controllers.yaml"
)
LITE6_ISAAC_GRIPPER_BRIDGE_CONFIG_PATH = (
    Path(__file__).resolve().parents[2]
    / "manyforge_ros_bringup"
    / "config"
    / "isaac_lite6_gripper_bridge.yaml"
)
LITE6_RUNTIME_COMPARE_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "models"
    / "compare_lite6_runtime_snapshot.py"
)
LITE6_RUNTIME_CAPTURE_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "models"
    / "capture_manymove_lite6_descriptions.py"
)
LITE6_STATIC_MANIFEST_VALIDATOR_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "models"
    / "validate_lite6_static_manifest.py"
)
LITE6_AUTHORITY_RUNTIME_CONTRACT_VALIDATOR_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "models"
    / "validate_lite6_authority_runtime_contract.py"
)
LITE6_ISAAC_SMOKE_VALIDATOR_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "ros"
    / "validate-isaac-lite6-smoke.py"
)
LITE6_ISAAC_SMOKE_ARTIFACT_VALIDATOR_SCRIPT = (
    Path(__file__).resolve().parents[2]
    / "scripts"
    / "ros"
    / "validate-isaac-lite6-smoke-artifact.py"
)
LAUNCH_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "launch.sh"
LITE6_LIMITED_ARM_BOUNDS = {
    "joint1": ("-3.1101767270538954", "3.1101767270538954"),
    "joint2": ("-2.61799", "2.61799"),
    "joint3": ("-0.061087", "3.1101767270538954"),
    "joint4": ("-3.1101767270538954", "3.1101767270538954"),
    "joint5": ("-2.1642", "2.1642"),
    "joint6": ("-3.1101767270538954", "3.1101767270538954"),
}
ASSISTANT_MODES_EXAMPLE_DEPLOYMENT_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "assistant_modes_scene_authoring.deployment.yaml"
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


def _lite6_live_smoke_artifact_payload() -> dict:
    joints = ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
    start = {joint: 0.0 for joint in joints}
    probe = dict(start)
    probe["joint1"] = 0.03

    def arm_result(target):
        return {
            "target_positions_rad": target,
            "final_arm_state_rad": target,
            "final_isaac_state_rad": target,
            "max_abs_target_error_rad": 0.0,
        }

    return {
        "passed": True,
        "details": {
            "schema": "manyforge.isaac_lite6_live_smoke.v1",
            "started_at_utc": "2026-06-25T10:00:00+00:00",
            "ended_at_utc": "2026-06-25T10:00:03+00:00",
            "arm_action": "/isaac_arm_controller/follow_joint_trajectory",
            "gripper_action": "/lite_gripper_controller/gripper_cmd",
            "arm_joint_state_topic": "/joint_states",
            "isaac_joint_state_topic": "/isaac_joint_states",
            "gripper_command_topic": "/isaac_joint_commands_gripper",
            "gripper_transport_kind": "isaac_joint_state_effort",
            "arm_joints": joints,
            "gripper_joint": "right_finger_joint",
            "start_positions_rad": start,
            "probe_positions_rad": probe,
            "arm_delta_rad": 0.03,
            "arm_motion_duration_s": 0.8,
            "gripper_open_position": 0.0081,
            "gripper_close_position": 0.0,
            "gripper_open_effort": 5.0,
            "gripper_close_effort": -5.0,
            "gripper_dwell_s": 0.8,
            "arm_probe_result": arm_result(probe),
            "arm_restore_result": arm_result(start),
            "gripper_close_result": {
                "transport_kind": "isaac_joint_state_effort",
                "actuation_kind": "pneumatic",
                "state_authority": "sim_observed",
                "completion_policy": "dwell_elapsed",
                "target_position": 0.0,
                "raw_command_position": 0.0,
                "raw_command_effort": -5.0,
                "dwell_s": 0.8,
                "dwell_elapsed_s": 0.8,
                "isaac_state_position": 0.0,
            },
            "gripper_open_result": {
                "transport_kind": "isaac_joint_state_effort",
                "actuation_kind": "pneumatic",
                "state_authority": "sim_observed",
                "completion_policy": "dwell_elapsed",
                "target_position": 0.0081,
                "raw_command_position": 0.0081,
                "raw_command_effort": 5.0,
                "dwell_s": 0.8,
                "dwell_elapsed_s": 0.8,
                "isaac_state_position": 0.0081,
            },
        },
    }


def _write_deployment(data: dict) -> str:
    f = tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False)
    yaml.dump(data, f)
    f.close()
    return f.name


def _write_gripper_srdf(
    tmp_path: Path,
    *,
    group: str = "gripper",
    joint: str = "finger_joint",
    states: dict[str, float] | None = None,
) -> Path:
    states = states or {"open": 0.0, "closed": 0.42}
    state_xml = "\n".join(
        f'  <group_state name="{name}" group="{group}">\n'
        f'    <joint name="{joint}" value="{value}"/>\n'
        "  </group_state>"
        for name, value in states.items()
    )
    srdf_path = tmp_path / "robot.srdf"
    srdf_path.write_text(
        '<?xml version="1.0"?>\n'
        '<robot name="fixture">\n'
        f"{state_xml}\n"
        "</robot>\n"
    )
    return srdf_path


def _minimal_deployment_with_gripper_srdf(
    tmp_path: Path,
    *,
    grippers: dict,
    group: str = "gripper",
    joint: str = "finger_joint",
    states: dict[str, float] | None = None,
) -> dict:
    srdf_path = _write_gripper_srdf(
        tmp_path,
        group=group,
        joint=joint,
        states=states,
    )
    return _minimal_deployment(
        robot_config={
            "robot_id": "ur5",
            "group_id": "arm",
            "urdf_path": "package://test.urdf",
            "srdf_path": str(srdf_path),
            "global_max_tcp_linear_speed_mps": 0.5,
        },
        grippers=grippers,
    )


def _program_with_move_tcp(tcp_value=None, *, parameters=None) -> dict:
    """Program dict with one valid move_manipulator_action node, optionally
    carrying a per-move max_tcp_linear_speed_mps (literal number or
    ``${param:...}`` reference)."""
    move_params = {
        "move_id": "pick_approach",
        "motion_type": "joint",
        "joint_goal_names": ["j1", "j2", "j3", "j4", "j5", "j6"],
        "joint_goal_positions_rad": [0.2, -1.2, 1.0, -1.5, -1.3, 0.4],
        "velocity_scale": 0.5,
        "planning_timeout_s": 2.0,
        "execution_timeout_s": 10.0,
    }
    if tcp_value is not None:
        move_params["max_tcp_linear_speed_mps"] = tcp_value
    data = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "tcp_cap_test",
        "catalog_version": "0.4.0",
        "robot": {
            "robot_id": "ur5",
            "group_id": "arm",
            "base_frame": "base_link",
            "tcp_frame": "tool0",
        },
        "tree": {
            "id": "sequence",
            "name": "main",
            "children": [
                {
                    "id": "move_manipulator_action",
                    "name": "move_to_pick",
                    "params": move_params,
                }
            ],
        },
    }
    if parameters is not None:
        data["parameters"] = parameters
    return data


def _minimal_program(**overrides) -> dict:
    program = {
        "api_version": "manyforge.program.v0",
        "program_profile": "v0.0",
        "name": "test_program",
        "catalog_version": "0.4.0",
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


def _support_contact_scene(*, release_evidence_mode: str) -> list[dict]:
    return [
        {
            "object_id": "part",
            "shape": {"type": "box", "box_dimensions_m": [0.1, 0.1, 0.1]},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.0, 0.0, 0.05],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "metadata": {
                "support_contacts": [
                    {
                        "support_object_id": "fixture",
                        "release_evidence_mode": release_evidence_mode,
                    }
                ]
            },
        },
        {
            "object_id": "fixture",
            "shape": {"type": "box", "box_dimensions_m": [1.0, 1.0, 0.1]},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.0, 0.0, -0.05],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
        },
    ]


def _minimal_deployment(**overrides) -> dict:
    deploy = {
        "api_version": "manyforge.deployment.v0",
        "schema_version": "0.1.0",
        "name": "test_deployment",
        "program_profile": "v0.0",
        "grasp_risk_policy": {
            "open_loop_grasp": {
                "allowed": True,
                "scope": "simulation",
                "rationale": "Test deployments use open-loop attach assumptions.",
            }
        },
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


def _assistant_mode_block(
    *,
    skills: list[str] | None = None,
    tools: list[str] | None = None,
    nodes: list[str] | None = None,
    session_kind: str = "not_applicable",
    approval_mode: str = "human_approved",
    review: str = "required",
) -> dict:
    return {
        "enabled": True,
        "description": "Mode under test.",
        "authority": {
            "session_kind": session_kind,
            "approval_mode": approval_mode,
            "review": review,
        },
        "catalog": {
            "skills": skills or [],
            "tools": tools or [],
            "nodes": nodes or [],
        },
    }


def _tool_catalog(*tool_ids: str, effect: str = "read_only") -> list[dict]:
    return [
        {
            "id": tool_id,
            "effect": effect,
            "description": f"{tool_id} fixture tool",
        }
        for tool_id in tool_ids
    ]


def _skill_catalog(tmp_path: Path, *, requires_tools: list[str] | None = None) -> SkillCatalog:
    skill_path = tmp_path / "assist_skill.skill.yaml"
    requires_tools_yaml = ""
    if requires_tools is not None:
        requires_tools_yaml = (
            "requires_tools:\n"
            + "".join(f"  - {tool_id}\n" for tool_id in requires_tools)
        )
    skill_path.write_text(
        "api_version: manyforge.skill.v0\n"
        "skill_id: assist_skill\n"
        f"{requires_tools_yaml}"
        "body: {id: reset_trajectories, name: noop}\n",
    )
    skill = load_skill(skill_path)
    return SkillCatalog(skills=[skill], _by_id={skill.id: skill})


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

    def test_robot_config_default_global_cap_is_safe_positive(self):
        """R4: a RobotConfig constructed in code is born with a positive,
        conservative global TCP cap (not 0.0), so programmatic/scaffold
        construction is mandatory-compliant instead of failing the > 0 rule."""
        rc = RobotConfig(robot_id="r", group_id="g", urdf_path="package://x.urdf")
        assert (
            rc.global_max_tcp_linear_speed_mps
            == DEFAULT_GLOBAL_MAX_TCP_LINEAR_SPEED_MPS
        )
        assert rc.global_max_tcp_linear_speed_mps > 0

    def test_deployment_missing_global_cap_rejected(self):
        """R4: the global cap is mandatory — an omitted value fails loud at load
        (no silent fallback to the in-code default)."""
        data = _minimal_deployment()
        del data["robot_config"]["global_max_tcp_linear_speed_mps"]
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError, match="must be > 0"):
                Deployment(path)
        finally:
            os.unlink(path)

    def test_robot_control_joint_limits_roundtrip(self):
        """spec/430 §3.2 — robot_control_joint_limits parse + serialize round-trip.

        Regression: serializer used to drop the entire block so
        DeploymentPanel "Save" silently lost per-joint overrides.
        """
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://x/y.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
                "robot_control_joint_limits": {
                    "shoulder_pan_joint": {
                        "max_velocity_rad_s": 0.5,
                        "max_acceleration_rad_s2": 2.0,
                    },
                    # Velocity-only — acceleration absent should
                    # survive the round trip as None.
                    "elbow_joint": {"max_velocity_rad_s": 0.3},
                    # Acceleration-only entry.
                    "wrist_1_joint": {"max_acceleration_rad_s2": 1.5},
                },
            }
        )
        path = _write_deployment(data)
        try:
            deploy_a = Deployment(path)
            assert (
                deploy_a.robot_config.robot_control_joint_limits[
                    "shoulder_pan_joint"
                ].max_velocity_rad_s
                == 0.5
            )
            assert (
                deploy_a.robot_config.robot_control_joint_limits[
                    "elbow_joint"
                ].max_acceleration_rad_s2
                is None
            )
            assert (
                deploy_a.robot_config.robot_control_joint_limits[
                    "wrist_1_joint"
                ].max_velocity_rad_s
                is None
            )
            # Serialize → reload → compare.
            serialized = deploy_a.to_dict()
            path_b = _write_deployment(serialized)
            try:
                deploy_b = Deployment(path_b)
                limits_a = deploy_a.robot_config.robot_control_joint_limits
                limits_b = deploy_b.robot_config.robot_control_joint_limits
                assert set(limits_a.keys()) == set(limits_b.keys())
                for joint, a in limits_a.items():
                    b = limits_b[joint]
                    assert a.max_velocity_rad_s == b.max_velocity_rad_s
                    assert (
                        a.max_acceleration_rad_s2 == b.max_acceleration_rad_s2
                    )
            finally:
                os.unlink(path_b)
        finally:
            os.unlink(path)

    def test_robot_control_joint_limits_empty_block_drops_from_serialize(self):
        """A deployment with no per-joint overrides MUST NOT emit an
        empty `robot_control_joint_limits` block — keeps the YAML minimal
        and matches existing deployments that don't use the feature.
        """
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.robot_config.robot_control_joint_limits == {}
        serialized = deploy.to_dict()
        assert "robot_control_joint_limits" not in serialized["robot_config"]

    def test_robot_control_joint_limits_against_drivable_joints(self):
        """spec/430 §3.2 — keys not in the drivable joint set MUST be rejected."""
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://x/y.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
                "robot_control_joint_limits": {
                    "shoulder_pan_joint": {"max_velocity_rad_s": 0.5},
                    "typo_not_a_joint": {"max_velocity_rad_s": 0.3},
                },
            }
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            # Valid joint set: validate OK
            deploy.robot_config.robot_control_joint_limits.pop("typo_not_a_joint")
            deploy.validate_robot_control_joint_limits_against(
                ["shoulder_pan_joint", "elbow_joint"]
            )
            # Reintroduce typo: validate rejects
            from manyforge_behavior.deployment import RobotControlJointLimit
            deploy.robot_config.robot_control_joint_limits["typo_not_a_joint"] = (
                RobotControlJointLimit(max_velocity_rad_s=0.3)
            )
            with pytest.raises(DeploymentValidationError) as exc:
                deploy.validate_robot_control_joint_limits_against(
                    ["shoulder_pan_joint", "elbow_joint"]
                )
            assert "typo_not_a_joint" in str(exc.value)
        finally:
            os.unlink(path)

    def test_legacy_streaming_joint_limits_key_is_rejected(self):
        """Clean break: the removed `streaming_joint_limits` key MUST fail loud
        at load time, never silently drop the operator's per-joint caps."""
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://x/y.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
                "streaming_joint_limits": {
                    "shoulder_pan_joint": {"max_velocity_rad_s": 0.5},
                },
            }
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            message = str(exc.value)
            assert "streaming_joint_limits" in message
            assert "robot_control_joint_limits" in message
        finally:
            os.unlink(path)

    def test_robot_control_joint_limits_jerk_roundtrip(self):
        """Phase 4-P5.1K — max_jerk_rad_s3 parses and survives the serialize
        round-trip alongside velocity/acceleration, and a jerk-only entry keeps
        velocity/acceleration as None (empty fields stay optional)."""
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://x/y.urdf",
                "global_max_tcp_linear_speed_mps": 0.25,
                "robot_control_joint_limits": {
                    "shoulder_pan_joint": {
                        "max_velocity_rad_s": 0.5,
                        "max_acceleration_rad_s2": 2.0,
                        "max_jerk_rad_s3": 10.0,
                    },
                    # Jerk-only entry: velocity/acceleration must remain None.
                    "elbow_joint": {"max_jerk_rad_s3": 7.5},
                },
            }
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            limits = deploy.robot_config.robot_control_joint_limits
            assert limits["shoulder_pan_joint"].max_jerk_rad_s3 == 10.0
            assert limits["elbow_joint"].max_jerk_rad_s3 == 7.5
            assert limits["elbow_joint"].max_velocity_rad_s is None
            assert limits["elbow_joint"].max_acceleration_rad_s2 is None
            # Serialize emits jerk and a reload preserves it.
            serialized = deploy.to_dict()
            assert (
                serialized["robot_config"]["robot_control_joint_limits"][
                    "shoulder_pan_joint"
                ]["max_jerk_rad_s3"]
                == 10.0
            )
            path_b = _write_deployment(serialized)
            try:
                deploy_b = Deployment(path_b)
                assert (
                    deploy_b.robot_config.robot_control_joint_limits[
                        "elbow_joint"
                    ].max_jerk_rad_s3
                    == 7.5
                )
            finally:
                os.unlink(path_b)
        finally:
            os.unlink(path)

    def test_robot_control_joint_limits_reject_non_finite_or_nonpositive(self):
        """Phase 4-P5.1K fail-closed: every per-joint dynamics cap (velocity,
        acceleration, jerk) must be a finite, strictly-positive number. NaN, inf,
        zero, and negative values are rejected at load; non-numeric values too."""
        for dim in (
            "max_velocity_rad_s",
            "max_acceleration_rad_s2",
            "max_jerk_rad_s3",
        ):
            for bad in (0.0, -1.0, float("inf"), float("nan")):
                data = _minimal_deployment(
                    robot_config={
                        "robot_id": "ur5",
                        "group_id": "arm",
                        "urdf_path": "package://x/y.urdf",
                        "global_max_tcp_linear_speed_mps": 0.25,
                        "robot_control_joint_limits": {
                            "shoulder_pan_joint": {dim: bad},
                        },
                    }
                )
                path = _write_deployment(data)
                try:
                    with pytest.raises(DeploymentValidationError) as exc:
                        Deployment(path)
                    msg = str(exc.value)
                    assert dim in msg
                    assert "finite number > 0" in msg
                finally:
                    os.unlink(path)
            # Non-numeric value → distinct "must be a number" rejection.
            data = _minimal_deployment(
                robot_config={
                    "robot_id": "ur5",
                    "group_id": "arm",
                    "urdf_path": "package://x/y.urdf",
                    "global_max_tcp_linear_speed_mps": 0.25,
                    "robot_control_joint_limits": {
                        "shoulder_pan_joint": {dim: "fast"},
                    },
                }
            )
            path = _write_deployment(data)
            try:
                with pytest.raises(DeploymentValidationError) as exc:
                    Deployment(path)
                assert "must be a number" in str(exc.value)
            finally:
                os.unlink(path)

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
            assert deploy.grippers["primary"].semantic_configured is False
            assert deploy.grippers["primary"].actuation_kind == "position_commanded"
            assert deploy.grippers["primary"].state_authority == "native_backend"
            assert deploy.grippers["primary"].completion_policy == "position_reached"
            assert deploy.grippers["primary"].grasp_confirmation_policy == "none_open_loop"
            assert deploy.grippers["primary"].transport == {"kind": "native_kinematic"}
            assert deploy.grippers["primary"].migration_notes
        finally:
            os.unlink(path)

    def test_gripper_srdf_presets_are_parsed(self, tmp_path):
        srdf_path = tmp_path / "robot.srdf"
        srdf_path.write_text(
            '<?xml version="1.0"?>\n'
            '<robot name="fixture">\n'
            '  <group_state name="ready" group="gripper">\n'
            '    <joint name="finger_joint" value="0.15"/>\n'
            "  </group_state>\n"
            '  <group_state name="pinch" group="gripper">\n'
            '    <joint name="finger_joint" value="0.42"/>\n'
            "  </group_state>\n"
            "</robot>\n"
        )
        data = _minimal_deployment(
            robot_config={
                "robot_id": "ur5",
                "group_id": "arm",
                "urdf_path": "package://test.urdf",
                "srdf_path": str(srdf_path),
                "global_max_tcp_linear_speed_mps": 0.5,
            },
            grippers={
                "primary": {
                    "command_mode": "simulated_joint",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "motion_time_s": 0.8,
                }
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            assert deploy.grippers["primary"].srdf_presets == {
                "ready": 0.15,
                "pinch": 0.42,
            }
        finally:
            os.unlink(path)

    def test_deployment_gripper_presets_are_rejected(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "simulated_joint",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "closed_position": 0.7,
                    "motion_time_s": 0.8,
                }
            },
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(
                DeploymentValidationError,
                match="closed_position is no longer supported",
            ):
                Deployment(path)
        finally:
            os.unlink(path)

    def test_semantic_native_gripper_roundtrips_and_overlay_preserves_fields(
        self,
        tmp_path,
    ):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "native_backend",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {"kind": "native_kinematic"},
                }
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            gripper = deploy.grippers["primary"]
            assert gripper.command_mode == "simulated_joint"
            assert gripper.semantic_configured is True
            assert gripper.actuation_kind == "pneumatic"
            assert gripper.state_authority == "native_backend"
            assert gripper.completion_policy == "dwell_elapsed"
            assert gripper.grasp_confirmation_policy == "none_open_loop"
            assert gripper.open_state_ref == "open"
            assert gripper.closed_state_ref == "closed"
            assert gripper.transport == {"kind": "native_kinematic"}

            roundtripped = Deployment.from_dict(deploy.to_dict())
            assert roundtripped.grippers["primary"].transport == {
                "kind": "native_kinematic"
            }
            assert roundtripped.grippers["primary"].semantic_configured is True

            deploy.update_from_dict({"grippers": {"primary": {"motion_time_s": 1.2}}})
            updated = deploy.grippers["primary"]
            assert updated.motion_time_s == 1.2
            assert updated.transport == {"kind": "native_kinematic"}
            assert updated.actuation_kind == "pneumatic"
            assert updated.open_state_ref == "open"
            assert updated.closed_state_ref == "closed"
        finally:
            os.unlink(path)

    def test_semantic_ros_gripper_command_parsed(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "command_mode": "ros_gripper_command",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "position_commanded",
                    "state_authority": "sim_observed",
                    "completion_policy": "position_reached",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {
                        "kind": "ros_gripper_command",
                        "action_name": "/gripper/gripper_cmd",
                        "result_timeout_s": 5.0,
                        "position_tolerance": 0.004,
                    },
                }
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            gripper = deploy.grippers["primary"]
            assert gripper.command_mode == "ros_gripper_command"
            assert gripper.transport["kind"] == "ros_gripper_command"
            assert gripper.transport["action_name"] == "/gripper/gripper_cmd"
            assert gripper.transport["result_timeout_s"] == 5.0
        finally:
            os.unlink(path)

    def test_semantic_isaac_joint_state_effort_gripper_parsed(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            group="lite_gripper",
            joint="right_finger_joint",
            states={"open": 0.0081, "closed": 0.0},
            grippers={
                "primary": {
                    "command_mode": "ros_gripper_command",
                    "group_id": "lite_gripper",
                    "joint_name": "right_finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "sim_observed",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {
                        "kind": "isaac_joint_state_effort",
                        "command_topic": "/isaac_joint_commands_gripper",
                        "state_topic": "/isaac_joint_states",
                        "joint_names": ["right_finger_joint"],
                        "close_effort": -5.0,
                        "open_effort": 5.0,
                        "publish_nominal_position": True,
                        "dwell_s": 0.8,
                    },
                }
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            gripper = deploy.grippers["primary"]
            assert gripper.command_mode == "ros_gripper_command"
            assert gripper.actuation_kind == "pneumatic"
            assert gripper.completion_policy == "dwell_elapsed"
            assert gripper.transport["kind"] == "isaac_joint_state_effort"
            assert gripper.transport["close_effort"] == -5.0
            assert gripper.transport["open_effort"] == 5.0
        finally:
            os.unlink(path)

    def test_semantic_plc_gripper_request_schema_parsed_for_deferred_real_lane(
        self,
        tmp_path,
    ):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "commanded_estimate",
                    "completion_policy": "request_ack_then_dwell",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {
                        "kind": "plc_gripper_request",
                        "command_endpoint": "lite_gripper/request",
                        "ready_signal": "lite_gripper/ready",
                        "fault_signal": "lite_gripper/fault",
                        "request_timeout_s": 0.25,
                        "dwell_s": 0.8,
                    },
                }
            },
        )
        path = _write_deployment(data)
        try:
            deploy = Deployment(path)
            gripper = deploy.grippers["primary"]
            assert gripper.command_mode == "plc_gripper_request"
            assert gripper.state_authority == "commanded_estimate"
            assert gripper.transport["kind"] == "plc_gripper_request"
            assert deploy.grasp_risk_policy.open_loop_grasp.allowed is True
            Deployment.from_dict(deploy.to_dict())
        finally:
            os.unlink(path)

    def test_none_open_loop_grasp_requires_deployment_risk_gate(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "native_backend",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {"kind": "native_kinematic"},
                }
            },
        )
        data.pop("grasp_risk_policy")
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "grasp_confirmation_policy=none_open_loop requires" in str(
                exc.value
            )
            assert "primary" in str(exc.value)
        finally:
            os.unlink(path)

    def test_legacy_none_open_loop_migration_requires_deployment_risk_gate(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "simulated_joint",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "motion_time_s": 0.8,
                }
            },
        )
        data.pop("grasp_risk_policy")
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "grasp_confirmation_policy=none_open_loop requires" in str(
                exc.value
            )
        finally:
            os.unlink(path)

    def test_open_loop_grasp_gate_requires_scope_and_rationale(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "native_backend",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {"kind": "native_kinematic"},
                }
            },
        )
        data["grasp_risk_policy"] = {
            "open_loop_grasp": {
                "allowed": True,
                "scope": "debug",
                "rationale": "invalid scope",
            }
        }
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "grasp_risk_policy.open_loop_grasp.scope must be one of" in str(
                exc.value
            )
        finally:
            os.unlink(path)

    def test_open_loop_grasp_gate_rejects_conflicting_aliases(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "native_backend",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {"kind": "native_kinematic"},
                }
            },
        )
        data["grasp_risk_policy"]["openLoopGrasp"] = {
            "allowed": False,
        }
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "open_loop_grasp and openLoopGrasp with different values" in str(
                exc.value
            )
        finally:
            os.unlink(path)

    def test_gripper_unknown_fields_are_rejected(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "simulated_joint",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "motion_time_s": 0.8,
                    "close_position_hint": 0.0,
                }
            },
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "grippers.primary contains unknown fields" in str(exc.value)
            assert "close_position_hint" in str(exc.value)
        finally:
            os.unlink(path)

    def test_legacy_ros_gripper_command_requires_explicit_semantics(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "ros_gripper_command",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "motion_time_s": 0.8,
                }
            },
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "command_mode=ros_gripper_command is ambiguous" in str(exc.value)
        finally:
            os.unlink(path)

    def test_legacy_plc_gripper_request_requires_explicit_semantics(self):
        data = _minimal_deployment(
            grippers={
                "primary": {
                    "command_mode": "plc_gripper_request",
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "motion_time_s": 0.8,
                }
            },
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "command_mode=plc_gripper_request requires explicit semantic" in str(
                exc.value
            )
        finally:
            os.unlink(path)

    def test_semantic_gripper_missing_srdf_state_ref_rejected(self, tmp_path):
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            states={"open": 0.0},
            grippers={
                "primary": {
                    "group_id": "gripper",
                    "joint_name": "finger_joint",
                    "actuation_kind": "pneumatic",
                    "state_authority": "native_backend",
                    "completion_policy": "dwell_elapsed",
                    "grasp_confirmation_policy": "none_open_loop",
                    "open_state_ref": "open",
                    "closed_state_ref": "closed",
                    "motion_time_s": 0.8,
                    "transport": {"kind": "native_kinematic"},
                }
            },
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert "grippers.primary.closed_state_ref references SRDF group_state" in str(
                exc.value
            )
        finally:
            os.unlink(path)

    @pytest.mark.parametrize(
        ("gripper_patch", "expected"),
        [
            (
                {"state_authority": "sim_observed"},
                "transport.kind=native_kinematic requires state_authority=native_backend",
            ),
            (
                {
                    "transport": {
                        "kind": "isaac_joint_state_effort",
                        "command_topic": "/cmd",
                        "close_effort": -5.0,
                        "open_effort": 5.0,
                        "dwell_s": 0.8,
                    },
                    "state_authority": "real_observed",
                },
                "transport.kind=isaac_joint_state_effort requires state_authority=sim_observed or commanded_estimate",
            ),
            (
                {
                    "transport": {
                        "kind": "ros_gripper_command",
                        "action_name": "/gripper/gripper_cmd",
                        "result_timeout_s": 5.0,
                    },
                    "actuation_kind": "pneumatic",
                    "state_authority": "sim_observed",
                    "completion_policy": "position_reached",
                },
                "transport.kind=ros_gripper_command requires actuation_kind=position_commanded",
            ),
            (
                {
                    "transport": {
                        "kind": "ros_gripper_command",
                        "action_name": "/gripper/gripper_cmd",
                        "result_timeout_s": 5.0,
                    },
                    "actuation_kind": "position_commanded",
                    "state_authority": "sim_observed",
                    "completion_policy": "driver_result",
                },
                "completion_policy=driver_result requires transport.driver_result_contract",
            ),
            (
                {
                    "transport": {
                        "kind": "ros_gripper_command",
                        "action_name": "/gripper/gripper_cmd",
                        "result_timeout_s": 5.0,
                    },
                    "actuation_kind": "position_commanded",
                    "state_authority": "sim_observed",
                    "completion_policy": "dwell_elapsed",
                },
                "completion_policy=dwell_elapsed is valid only for pneumatic native_kinematic or isaac_joint_state_effort",
            ),
            (
                {
                    "completion_policy": "request_ack_then_dwell",
                },
                "completion_policy=request_ack_then_dwell requires transport.kind=plc_gripper_request",
            ),
            (
                {
                    "completion_policy": "open_loop_immediate",
                },
                "completion_policy=open_loop_immediate is not allowed without an explicit commissioning profile gate",
            ),
        ],
    )
    def test_semantic_gripper_invalid_cross_products_fail_closed(
        self,
        tmp_path,
        gripper_patch,
        expected,
    ):
        gripper = {
            "group_id": "gripper",
            "joint_name": "finger_joint",
            "actuation_kind": "pneumatic",
            "state_authority": "native_backend",
            "completion_policy": "dwell_elapsed",
            "grasp_confirmation_policy": "none_open_loop",
            "open_state_ref": "open",
            "closed_state_ref": "closed",
            "motion_time_s": 0.8,
            "transport": {"kind": "native_kinematic"},
        }
        gripper.update(gripper_patch)
        data = _minimal_deployment_with_gripper_srdf(
            tmp_path,
            grippers={"primary": gripper},
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(DeploymentValidationError) as exc:
                Deployment(path)
            assert expected in str(exc.value)
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
        assert deploy.orchestrator.config == {
            "collision_monitor_period_ms": 50.0,
            "validation_collision_workers": 4,
            "runtime_collision_lookahead_cycle_budget_ms": 200.0,
            "runtime_collision_lookahead_s": 2.0,
            "runtime_collision_lookahead_max_samples": 32,
            "runtime_collision_lookahead_workers": 2,
            "runtime_collision_immediate_query_budget_ms": 50.0,
            "runtime_collision_lookahead_query_budget_ms": 75.0,
        }

    def test_validation_collision_workers_materializes_production_default(self):
        deploy = Deployment.from_dict(_minimal_deployment())

        assert deploy.orchestrator.config["validation_collision_workers"] == 4
        assert (
            deploy.to_dict()["orchestrator"]["config"][
                "validation_collision_workers"
            ]
            == 4
        )

    def test_validation_collision_workers_roundtrips(self):
        data = _minimal_deployment(
            orchestrator={
                "engine": "py_trees",
                "config": {"validation_collision_workers": 3},
            }
        )
        deploy = Deployment.from_dict(data)
        assert deploy.orchestrator.config["validation_collision_workers"] == 3
        assert (
            deploy.to_dict()["orchestrator"]["config"][
                "validation_collision_workers"
            ]
            == 3
        )

    def test_runtime_collision_lookahead_materializes_production_defaults(self):
        deploy = Deployment.from_dict(_minimal_deployment())

        assert deploy.orchestrator.config == {
            "validation_collision_workers": 4,
            "runtime_collision_lookahead_cycle_budget_ms": 200.0,
            "runtime_collision_lookahead_s": 2.0,
            "runtime_collision_lookahead_max_samples": 32,
            "runtime_collision_lookahead_workers": 2,
            "runtime_collision_immediate_query_budget_ms": 50.0,
            "runtime_collision_lookahead_query_budget_ms": 75.0,
        }
        assert deploy.to_dict()["orchestrator"]["config"] == (
            deploy.orchestrator.config
        )

    def test_runtime_collision_lookahead_roundtrips_explicit_values(self):
        config = {
            "runtime_collision_lookahead_cycle_budget_ms": 75,
            "runtime_collision_lookahead_s": 1.5,
            "runtime_collision_lookahead_max_samples": 48,
            "runtime_collision_lookahead_workers": 4,
            "runtime_collision_immediate_query_budget_ms": 25,
            "runtime_collision_lookahead_query_budget_ms": 60,
        }
        deploy = Deployment.from_dict(
            _minimal_deployment(
                orchestrator={"engine": "py_trees", "config": config}
            )
        )

        assert deploy.orchestrator.config[
            "runtime_collision_lookahead_cycle_budget_ms"
        ] == 75.0
        assert deploy.orchestrator.config["runtime_collision_lookahead_s"] == 1.5
        assert (
            deploy.orchestrator.config["runtime_collision_lookahead_max_samples"]
            == 48
        )
        assert deploy.orchestrator.config["runtime_collision_lookahead_workers"] == 4
        assert (
            deploy.orchestrator.config[
                "runtime_collision_immediate_query_budget_ms"
            ]
            == 25.0
        )
        assert (
            deploy.orchestrator.config[
                "runtime_collision_lookahead_query_budget_ms"
            ]
            == 60.0
        )

    @pytest.mark.parametrize(
        ("key", "value"),
        [
            ("runtime_collision_lookahead_cycle_budget_ms", True),
            ("runtime_collision_lookahead_cycle_budget_ms", 0),
            ("runtime_collision_lookahead_cycle_budget_ms", 49),
            ("runtime_collision_lookahead_cycle_budget_ms", "100"),
            ("runtime_collision_lookahead_cycle_budget_ms", float("inf")),
            ("runtime_collision_lookahead_s", -1),
            ("runtime_collision_lookahead_s", float("nan")),
            ("runtime_collision_lookahead_max_samples", False),
            ("runtime_collision_lookahead_max_samples", 0),
            ("runtime_collision_lookahead_max_samples", 1),
            ("runtime_collision_lookahead_max_samples", 4097),
            ("runtime_collision_lookahead_workers", True),
            ("runtime_collision_lookahead_workers", 0),
            ("runtime_collision_lookahead_workers", 65),
            ("runtime_collision_immediate_query_budget_ms", True),
            ("runtime_collision_immediate_query_budget_ms", 0),
            ("runtime_collision_immediate_query_budget_ms", 201),
            ("runtime_collision_lookahead_query_budget_ms", True),
            ("runtime_collision_lookahead_query_budget_ms", 0),
            ("runtime_collision_lookahead_query_budget_ms", 201),
            ("runtime_collision_lookahead_query_budget_ms", float("inf")),
        ],
    )
    def test_runtime_collision_lookahead_rejects_invalid_values(self, key, value):
        data = _minimal_deployment(
            orchestrator={"engine": "py_trees", "config": {key: value}}
        )

        with pytest.raises(DeploymentValidationError, match=key):
            Deployment.from_dict(data)

    @pytest.mark.parametrize(
        "removed_key",
        [
            "runtime_collision_lookahead_period_ms",
            "runtime_collision_lookahead_max_waypoints",
            "runtime_collision_query_timeout_ms",
        ],
    )
    def test_runtime_collision_lookahead_rejects_removed_keys(self, removed_key):
        data = _minimal_deployment(
            orchestrator={"engine": "py_trees", "config": {removed_key: 1}}
        )
        with pytest.raises(
            DeploymentValidationError, match="removed RT-8C.3 keys"
        ):
            Deployment.from_dict(data)

    @pytest.mark.parametrize("value", [True, 0, -1, 65, 2.5, "4"])
    def test_validation_collision_workers_rejects_invalid_values(self, value):
        data = _minimal_deployment(
            orchestrator={
                "engine": "py_trees",
                "config": {"validation_collision_workers": value},
            }
        )
        with pytest.raises(
            DeploymentValidationError,
            match="validation_collision_workers",
        ):
            Deployment.from_dict(data)

    def test_launch_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.launch.runner.module == "manyforge_behavior.runner"
        assert deploy.launch.runner.args == []
        assert deploy.launch.ui_backend is not None
        assert deploy.launch.ui_backend.module == "manyforge_composer"
        assert deploy.launch.ui_backend.enabled is True

    def test_pose_sources_parse_and_roundtrip(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "service_prefix": "auto",
                    "max_update_hz": 4,
                    "stale_after_s": 1.25,
                    "frame_id": "universe",
                    "description": "Isaac entity poses for known scene objects.",
                    "measured_tcp": {
                        "entity": "/World/tool0",
                        "tcp_frame_id": "tool0",
                        "frame_id": "universe",
                        "entity_to_tcp": {
                            "position_xyz": [0.0, 0.0, 0.2],
                            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        },
                    },
                    "measured_robot": {
                        "joint_state_topic": "/joint_states",
                    },
                    "objects": {
                        "isaac_cheese_box": {
                            "entity": "/mac_n_cheese_centered",
                            "apply_policy": "before_planning",
                            "scene_admission": {
                                "role": "observed_target",
                                "lease_max_age_s": 2.0,
                                "target_drift_tolerance_m": 0.02,
                                "target_angular_tolerance_rad": 0.174533,
                            },
                            "confidence_min": 0.8,
                            "measured_state_ghost": {
                                "enabled": True,
                                "robot_tcp": True,
                                "object": True,
                                "opacity": 0.5,
                            },
                            "measured_attachment_baseline": {
                                "capture": "on_attach_or_first_paired_sample",
                                "require_same_time_robot_tcp": True,
                            },
                            "attached_drift_monitor": {
                                "enabled": True,
                                "translation_threshold_m": 0.02,
                                "rotation_threshold_rad": 0.2,
                                "debounce_s": 0.1,
                                "stale_behavior": "fault",
                                "action": "interrupt",
                                "signal_id": "attached_object_drift/isaac_cheese_box",
                                "confirmation_signal_id": (
                                    "attached_object_grasp_confirmed/isaac_cheese_box"
                                ),
                                "sample_time_tolerance_s": 0.05,
                            },
                            "metadata": {"source": "fixture"},
                        },
                        "isaac_soup_can": {
                            "entity": "/World/soup_can",
                            "apply_policy": "observe_only",
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)

        source = deploy.pose_sources["isaac_scene_objects"]
        assert source.kind == "simulation_interfaces"
        assert source.max_update_hz == 4.0
        assert source.stale_after_s == 1.25
        assert source.frame_id == "universe"
        assert source.measured_tcp is not None
        assert source.measured_tcp.entity == "/World/tool0"
        assert source.measured_tcp.tcp_frame_id == "tool0"
        assert source.measured_tcp.entity_to_tcp_position_xyz == (0.0, 0.0, 0.2)
        assert source.measured_robot is not None
        assert source.measured_robot.joint_state_topic == "/joint_states"
        assert set(source.objects) == {"isaac_cheese_box", "isaac_soup_can"}
        assert source.objects["isaac_cheese_box"].entity == "/mac_n_cheese_centered"
        assert source.objects["isaac_cheese_box"].apply_policy == "before_planning"
        assert source.objects["isaac_cheese_box"].scene_admission is not None
        assert (
            source.objects["isaac_cheese_box"].scene_admission.role
            == "observed_target"
        )
        assert (
            source.objects[
                "isaac_cheese_box"
            ].scene_admission.target_drift_tolerance_m
            == 0.02
        )
        assert (
            source.objects[
                "isaac_cheese_box"
            ].scene_admission.target_angular_tolerance_rad
            == 0.174533
        )
        assert source.objects["isaac_cheese_box"].confidence_min == 0.8
        assert source.objects["isaac_cheese_box"].measured_state_ghost is not None
        assert source.objects["isaac_cheese_box"].measured_state_ghost.enabled is True
        assert source.objects["isaac_cheese_box"].measured_attachment_baseline is not None
        assert (
            source.objects[
                "isaac_cheese_box"
            ].measured_attachment_baseline.require_same_time_robot_tcp
            is True
        )
        assert source.objects["isaac_cheese_box"].attached_drift_monitor is not None
        assert (
            source.objects["isaac_cheese_box"].attached_drift_monitor.translation_threshold_m
            == 0.02
        )
        assert (
            source.objects[
                "isaac_cheese_box"
            ].attached_drift_monitor.confirmation_signal_id
            == "attached_object_grasp_confirmed/isaac_cheese_box"
        )
        assert source.objects["isaac_cheese_box"].metadata == {"source": "fixture"}

        serialized = deploy.to_dict()
        reloaded = Deployment.from_dict(serialized)
        assert (
            reloaded.pose_sources[
                "isaac_scene_objects"
            ].measured_tcp.entity_to_tcp_position_xyz
            == (0.0, 0.0, 0.2)
        )
        assert reloaded.pose_sources["isaac_scene_objects"].objects[
            "isaac_soup_can"
        ].apply_policy == "observe_only"
        assert (
            reloaded.pose_sources["isaac_scene_objects"]
            .objects["isaac_cheese_box"]
            .scene_admission
            .role
            == "observed_target"
        )
        assert (
            reloaded.pose_sources["isaac_scene_objects"]
            .objects["isaac_cheese_box"]
            .attached_drift_monitor
            .signal_id
            == "attached_object_drift/isaac_cheese_box"
        )
        assert (
            reloaded.pose_sources["isaac_scene_objects"]
            .objects["isaac_cheese_box"]
            .attached_drift_monitor
            .confirmation_signal_id
            == "attached_object_grasp_confirmed/isaac_cheese_box"
        )
        assert (
            reloaded.pose_sources["isaac_scene_objects"].measured_tcp.entity
            == "/World/tool0"
        )
        assert (
            reloaded.pose_sources[
                "isaac_scene_objects"
            ].measured_robot.joint_state_topic
            == "/joint_states"
        )

    def test_pose_source_measured_tcp_rejects_non_unit_fixed_rotation(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "measured_tcp": {
                        "entity": "/World/tool0",
                        "tcp_frame_id": "tool0",
                        "entity_to_tcp": {
                            "orientation_xyzw": [0.0, 0.0, 0.0, 2.0],
                        },
                    },
                    "objects": {},
                }
            ]
        )

        with pytest.raises(
            DeploymentValidationError,
            match="orientation_xyzw must be a unit quaternion",
        ):
            Deployment.from_dict(data)

    @pytest.mark.parametrize("invalid_component", [True, "0.0"])
    def test_pose_source_measured_tcp_rejects_non_numeric_fixed_transform(
        self, invalid_component
    ):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "measured_tcp": {
                        "entity": "/World/tool0",
                        "tcp_frame_id": "tool0",
                        "entity_to_tcp": {
                            "position_xyz": [invalid_component, 0.0, 0.2],
                        },
                    },
                    "objects": {},
                }
            ]
        )

        with pytest.raises(
            DeploymentValidationError,
            match="position_xyz must contain only numbers",
        ):
            Deployment.from_dict(data)

    def test_pose_source_observe_only_defaults_to_visualization_admission(self):
        deploy = Deployment.from_dict(
            _minimal_deployment(
                pose_sources=[
                    {
                        "id": "tracked_objects",
                        "kind": "pose_stamped_topic",
                        "topic": "/tracked_pose",
                        "objects": {
                            "box": {
                                "apply_policy": "observe_only",
                            }
                        },
                    }
                ]
            )
        )

        admission = deploy.pose_sources["tracked_objects"].objects["box"].scene_admission
        assert admission is not None
        assert admission.role == "visualization_only"
        serialized = deploy.to_dict()
        assert (
            serialized["pose_sources"][0]["objects"]["box"]["scene_admission"][
                "role"
            ]
            == "visualization_only"
        )

    def test_pose_source_execution_policy_defaults_to_snapshot_obstacle_admission(self):
        deploy = Deployment.from_dict(
            _minimal_deployment(
                pose_sources=[
                    {
                        "id": "tracked_objects",
                        "kind": "pose_stamped_topic",
                        "topic": "/tracked_pose",
                        "objects": {
                            "box": {
                                "apply_policy": "continuous",
                            }
                        },
                    }
                ]
            )
        )

        admission = deploy.pose_sources["tracked_objects"].objects["box"].scene_admission
        assert admission is not None
        assert admission.role == "snapshot_obstacle"
        serialized = deploy.to_dict()
        assert (
            serialized["pose_sources"][0]["objects"]["box"]["scene_admission"][
                "role"
            ]
            == "snapshot_obstacle"
        )

    def test_pose_source_rejects_execution_affecting_visualization_admission(self):
        with pytest.raises(
            DeploymentValidationError,
            match="role=visualization_only is only valid with apply_policy=observe_only",
        ):
            Deployment.from_dict(
                _minimal_deployment(
                    pose_sources=[
                        {
                            "id": "tracked_objects",
                            "kind": "pose_stamped_topic",
                            "topic": "/tracked_pose",
                            "objects": {
                                "box": {
                                    "apply_policy": "before_planning",
                                    "scene_admission": {
                                        "role": "visualization_only",
                                    },
                                }
                            },
                        }
                    ]
                )
            )

    def test_pose_source_requires_drift_tolerances_for_observed_target_admission(self):
        with pytest.raises(
            DeploymentValidationError,
            match="target_drift_tolerance_m is required",
        ):
            Deployment.from_dict(
                _minimal_deployment(
                    pose_sources=[
                        {
                            "id": "tracked_objects",
                            "kind": "pose_stamped_topic",
                            "topic": "/tracked_pose",
                            "objects": {
                                "box": {
                                    "apply_policy": "before_planning",
                                    "scene_admission": {
                                        "role": "observed_target",
                                        "target_angular_tolerance_rad": 0.1,
                                    },
                                }
                            },
                        }
                    ]
                )
            )

    def test_pose_source_rejects_drift_tolerances_on_snapshot_obstacle(self):
        with pytest.raises(
            DeploymentValidationError,
            match="target drift tolerances are only valid for observed_target",
        ):
            Deployment.from_dict(
                _minimal_deployment(
                    pose_sources=[
                        {
                            "id": "tracked_objects",
                            "kind": "pose_stamped_topic",
                            "topic": "/tracked_pose",
                            "objects": {
                                "box": {
                                    "apply_policy": "continuous",
                                    "scene_admission": {
                                        "role": "snapshot_obstacle",
                                        "target_drift_tolerance_m": 0.01,
                                    },
                                }
                            },
                        }
                    ]
                )
            )

    def test_pose_source_rejects_enabled_drift_monitor_without_thresholds(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "objects": {
                        "box": {
                            "entity": "/World/box",
                            "apply_policy": "continuous_unattached",
                            "attached_drift_monitor": {
                                "enabled": True,
                                "debounce_s": 0.1,
                            },
                        },
                    },
                },
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="enabled monitors require at least one threshold",
        ):
            Deployment.from_dict(data)

    def test_pose_source_rejects_empty_measured_robot_topic(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "measured_robot": {"joint_state_topic": "  "},
                    "objects": {
                        "box": {
                            "entity": "/World/box",
                            "apply_policy": "observe_only",
                        },
                    },
                },
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_robot.joint_state_topic must be non-empty",
        ):
            Deployment.from_dict(data)

    def test_pose_source_rejects_non_boolean_measured_ghost_fields(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "isaac_scene_objects",
                    "kind": "simulation_interfaces",
                    "objects": {
                        "box": {
                            "entity": "/World/box",
                            "measured_state_ghost": {
                                "enabled": "false",
                            },
                        },
                    },
                },
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_state_ghost.enabled must be a boolean",
        ):
            Deployment.from_dict(data)

    def test_pose_stamped_topic_source_parse_and_roundtrip(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_box",
                    "kind": "pose_stamped_topic",
                    "topic": "/tracked_box_pose",
                    "max_update_hz": 12,
                    "frame_id": "world",
                    "objects": {
                        "box": {
                            "apply_policy": "before_planning",
                            "frame_id": "world",
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)
        source = deploy.pose_sources["tracked_box"]
        assert source.kind == "pose_stamped_topic"
        assert source.topic == "/tracked_box_pose"
        assert source.max_update_hz == 12.0
        assert source.objects["box"].apply_policy == "before_planning"
        assert source.objects["box"].entity == ""

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.pose_sources["tracked_box"].topic == "/tracked_box_pose"
        assert reloaded.pose_sources["tracked_box"].objects["box"].frame_id == "world"

    def test_pose_source_rejects_duplicate_object_authority(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "source_a",
                    "kind": "simulation_interfaces",
                    "objects": {
                        "box": {
                            "entity": "/World/box_a",
                            "apply_policy": "observe_only",
                        }
                    },
                },
                {
                    "id": "source_b",
                    "kind": "simulation_interfaces",
                    "objects": {
                        "box": {
                            "entity": "/World/box_b",
                            "apply_policy": "observe_only",
                        }
                    },
                },
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="multiple write authorities for object 'box'",
        ):
            Deployment.from_dict(data)

    def test_pose_stamped_topic_source_rejects_multiple_objects(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_box",
                    "kind": "pose_stamped_topic",
                    "topic": "/tracked_box_pose",
                    "objects": {
                        "box_a": {"apply_policy": "observe_only"},
                        "box_b": {"apply_policy": "observe_only"},
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="exactly one object for pose_stamped_topic",
        ):
            Deployment.from_dict(data)

    def test_pose_stamped_topic_source_rejects_unsupported_confidence(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_box",
                    "kind": "pose_stamped_topic",
                    "topic": "/tracked_box_pose",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "confidence_min": 0.5,
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="confidence_min is not supported for pose_stamped_topic",
        ):
            Deployment.from_dict(data)

    def test_pose_stamped_topic_source_rejects_measured_tcp(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_box",
                    "kind": "pose_stamped_topic",
                    "topic": "/tracked_box_pose",
                    "measured_tcp": {
                        "tcp_frame_id": "tool0",
                    },
                    "objects": {
                        "box": {"apply_policy": "observe_only"},
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_tcp is not supported for pose_stamped_topic",
        ):
            Deployment.from_dict(data)

    def test_pose_array_topic_source_parse_and_roundtrip(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_objects",
                    "kind": "pose_array_topic",
                    "topic": "/tracked_object_poses",
                    "frame_id": "world",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"pose_index": 0},
                        },
                        "can": {
                            "apply_policy": "before_planning",
                            "metadata": {"pose_index": 1},
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)
        source = deploy.pose_sources["tracked_objects"]
        assert source.kind == "pose_array_topic"
        assert source.topic == "/tracked_object_poses"
        assert source.objects["box"].metadata["pose_index"] == 0
        assert source.objects["can"].metadata["pose_index"] == 1

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.pose_sources["tracked_objects"].objects["can"].metadata[
            "pose_index"
        ] == 1

    def test_pose_array_topic_source_rejects_missing_pose_index(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_objects",
                    "kind": "pose_array_topic",
                    "topic": "/tracked_object_poses",
                    "objects": {
                        "box": {"apply_policy": "observe_only"},
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="metadata.pose_index must be a non-negative integer",
        ):
            Deployment.from_dict(data)

    def test_pose_array_topic_source_rejects_duplicate_pose_index(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_objects",
                    "kind": "pose_array_topic",
                    "topic": "/tracked_object_poses",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"pose_index": 0},
                        },
                        "can": {
                            "apply_policy": "observe_only",
                            "metadata": {"pose_index": 0},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="duplicates object 'box'",
        ):
            Deployment.from_dict(data)

    def test_pose_array_topic_source_rejects_unsupported_confidence(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_objects",
                    "kind": "pose_array_topic",
                    "topic": "/tracked_object_poses",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "confidence_min": 0.5,
                            "metadata": {"pose_index": 0},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="confidence_min is not supported for pose_array_topic",
        ):
            Deployment.from_dict(data)

    def test_pose_array_topic_source_rejects_measured_tcp(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_objects",
                    "kind": "pose_array_topic",
                    "topic": "/tracked_object_poses",
                    "measured_tcp": {
                        "tcp_frame_id": "tool0",
                    },
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"pose_index": 0},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_tcp is not supported for pose_array_topic",
        ):
            Deployment.from_dict(data)

    def test_tf_pose_source_parse_and_roundtrip(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_tf_objects",
                    "kind": "tf",
                    "frame_id": "world",
                    "max_update_hz": 20,
                    "objects": {
                        "box": {
                            "entity": "box_frame",
                            "apply_policy": "observe_only",
                        },
                        "can": {
                            "entity": "can_frame",
                            "frame_id": "map",
                            "apply_policy": "before_planning",
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)
        source = deploy.pose_sources["tracked_tf_objects"]
        assert source.kind == "tf"
        assert source.frame_id == "world"
        assert source.objects["box"].entity == "box_frame"
        assert source.objects["can"].frame_id == "map"

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.pose_sources["tracked_tf_objects"].objects["box"].entity == (
            "box_frame"
        )

    def test_tf_pose_source_rejects_missing_target_frame(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_tf_objects",
                    "kind": "tf",
                    "objects": {
                        "box": {
                            "entity": "box_frame",
                            "apply_policy": "observe_only",
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="frame_id.*is required for tf",
        ):
            Deployment.from_dict(data)

    def test_tf_pose_source_rejects_unsupported_confidence(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_tf_objects",
                    "kind": "tf",
                    "frame_id": "world",
                    "objects": {
                        "box": {
                            "entity": "box_frame",
                            "apply_policy": "observe_only",
                            "confidence_min": 0.5,
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="confidence_min is not supported for tf",
        ):
            Deployment.from_dict(data)

    def test_tf_pose_source_rejects_measured_tcp(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "tracked_tf_objects",
                    "kind": "tf",
                    "frame_id": "world",
                    "measured_tcp": {
                        "tcp_frame_id": "tool0",
                    },
                    "objects": {
                        "box": {
                            "entity": "box_frame",
                            "apply_policy": "observe_only",
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_tcp is not supported for tf",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_parse_and_roundtrip(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "frame_id": "world",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "confidence_min": 0.6,
                            "metadata": {"class_id": "box"},
                        },
                        "can": {
                            "apply_policy": "before_planning",
                            "confidence_min": 0.8,
                            "metadata": {
                                "detection_id": "can-01",
                                "max_position_variance_m2": 0.0025,
                            },
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)
        source = deploy.pose_sources["detections"]
        assert source.kind == "detection3d_array_topic"
        assert source.objects["box"].metadata["class_id"] == "box"
        assert source.objects["can"].metadata["detection_id"] == "can-01"
        assert source.objects["can"].confidence_min == 0.8

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.pose_sources["detections"].objects["can"].metadata[
            "max_position_variance_m2"
        ] == 0.0025

    def test_detection3d_array_topic_source_accepts_selection_association(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "foundationpose",
                    "kind": "detection3d_array_topic",
                    "topic": "/output",
                    "frame_id": "link_base",
                    "objects": {
                        "graspable": {
                            "apply_policy": "before_planning",
                            "confidence_min": 0.0,
                            "metadata": {
                                "selection": "first",
                                "max_position_variance_m2": 0.0025,
                            },
                        },
                    },
                }
            ],
        )
        deploy = Deployment.from_dict(data)
        source = deploy.pose_sources["foundationpose"]
        assert source.frame_id == "link_base"
        assert source.objects["graspable"].metadata["selection"] == "first"

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert (
            reloaded.pose_sources["foundationpose"]
            .objects["graspable"]
            .metadata["selection"]
            == "first"
        )

    def test_detection3d_array_topic_source_rejects_missing_association(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {"apply_policy": "observe_only"},
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="exactly one of detection_id, class_id, or selection",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_rejects_ambiguous_association(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {
                                "detection_id": "box-01",
                                "class_id": "box",
                            },
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="exactly one of detection_id, class_id, or selection",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_rejects_invalid_selection(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"selection": "closest"},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="metadata.selection must be 'first' or 'best_score'",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_rejects_duplicate_association(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"class_id": "grocery_box"},
                        },
                        "box_duplicate": {
                            "apply_policy": "observe_only",
                            "metadata": {"class_id": "grocery_box"},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="class_id.*duplicates object 'box'",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_rejects_invalid_optional_variance(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {
                                "class_id": "grocery_box",
                                "max_position_variance_m2": 0.0,
                            },
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="max_position_variance_m2 must be a positive finite number",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_requires_quality_gates_for_mutation(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "objects": {
                        "box": {
                            "apply_policy": "before_planning",
                            "metadata": {"class_id": "box"},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="confidence_min is required",
        ):
            Deployment.from_dict(data)

        data["pose_sources"][0]["objects"]["box"]["confidence_min"] = 0.8
        with pytest.raises(
            DeploymentValidationError,
            match="max_position_variance_m2 is required",
        ):
            Deployment.from_dict(data)

    def test_detection3d_array_topic_source_rejects_measured_tcp(self):
        data = _minimal_deployment(
            pose_sources=[
                {
                    "id": "detections",
                    "kind": "detection3d_array_topic",
                    "topic": "/detections",
                    "measured_tcp": {
                        "tcp_frame_id": "tool0",
                    },
                    "objects": {
                        "box": {
                            "apply_policy": "observe_only",
                            "metadata": {"class_id": "box"},
                        },
                    },
                }
            ],
        )
        with pytest.raises(
            DeploymentValidationError,
            match="measured_tcp is not supported for detection3d_array_topic",
        ):
            Deployment.from_dict(data)

    @pytest.mark.parametrize(
        ("pose_sources", "match"),
        [
            (
                [
                    {
                        "id": "bad_kind",
                        "kind": "raw_magic",
                        "objects": {
                            "box": {
                                "entity": "/World/box",
                                "apply_policy": "observe_only",
                            }
                        },
                    }
                ],
                "kind must be one of",
            ),
            (
                [
                    {
                        "id": "bad_policy",
                        "kind": "simulation_interfaces",
                        "objects": {
                            "box": {
                                "entity": "/World/box",
                                "apply_policy": "during_motion",
                            }
                        },
                    }
                ],
                "apply_policy must be one of",
            ),
            (
                [
                    {
                        "id": "bad_rate",
                        "kind": "simulation_interfaces",
                        "max_update_hz": 0,
                        "objects": {
                            "box": {
                                "entity": "/World/box",
                                "apply_policy": "observe_only",
                            }
                        },
                    }
                ],
                "max_update_hz must be a number > 0",
            ),
            (
                [
                    {
                        "id": "bad_confidence",
                        "kind": "simulation_interfaces",
                        "objects": {
                            "box": {
                                "entity": "/World/box",
                                "apply_policy": "observe_only",
                                "confidence_min": 1.5,
                            }
                        },
                    }
                ],
                "confidence_min must be a number between 0.0 and 1.0",
            ),
            (
                [
                    {
                        "id": "missing_entity",
                        "kind": "simulation_interfaces",
                        "objects": {
                            "box": {
                                "apply_policy": "observe_only",
                            }
                        },
                    }
                ],
                "entity is required for simulation_interfaces",
            ),
            (
                [
                    {
                        "id": "topic_source",
                        "kind": "pose_stamped_topic",
                        "objects": {
                            "box": {
                                "apply_policy": "observe_only",
                            }
                        },
                    }
                ],
                "topic is required",
            ),
        ],
    )
    def test_pose_source_validation_rejects_invalid_contract(
        self,
        pose_sources,
        match,
    ):
        with pytest.raises(DeploymentValidationError, match=match):
            Deployment.from_dict(_minimal_deployment(pose_sources=pose_sources))

    def test_simulation_control_parse_roundtrip_and_capability_keys(self):
        data = _minimal_deployment(
            simulation_control={
                "kind": "ros2_simulation_interfaces",
                "service_prefix": "auto",
                "service_timeout_s": 8.5,
                "capabilities": {
                    "reset": True,
                    "pause": False,
                    "play": True,
                    "step": False,
                    "set_entity_state": True,
                },
                "reset": {
                    "default_scope": "state",
                    "close_command_gate": True,
                    "wait_ready_s": 4.5,
                    "resume_play_after_reset": False,
                },
            },
        )
        deploy = Deployment.from_dict(data)

        control = deploy.simulation_control
        assert control is not None
        assert control.kind == "ros2_simulation_interfaces"
        assert control.service_prefix == "auto"
        assert control.service_timeout_s == 8.5
        assert control.capabilities.reset is True
        assert control.capabilities.set_entity_state is True
        assert control.reset.default_scope == "state"
        assert control.reset.close_command_gate is True
        assert control.reset.wait_ready_s == 4.5
        assert control.reset.resume_play_after_reset is False
        assert "simulator.control.reset" in deploy.capability_keys()
        assert "simulator.control.play" in deploy.capability_keys()
        assert "simulator.entity_state.set" in deploy.capability_keys()

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.simulation_control is not None
        assert reloaded.simulation_control.service_timeout_s == 8.5
        assert reloaded.simulation_control.capabilities.reset is True
        assert reloaded.simulation_control.reset.wait_ready_s == 4.5
        assert reloaded.simulation_control.reset.resume_play_after_reset is False

    @pytest.mark.parametrize(
        ("simulation_control", "match"),
        [
            (
                {
                    "kind": "raw_isaac_python",
                    "capabilities": {"reset": True},
                },
                "simulation_control.kind must be one of",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "service": "/isaac",
                    "capabilities": {"reset": True},
                },
                "simulation_control contains unknown fields",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "service_timeout_s": 0,
                    "capabilities": {"reset": True},
                },
                "simulation_control.service_timeout_s must be a number > 0",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "capabilities": {"reset": "true"},
                },
                "simulation_control.capabilities.reset must be a boolean",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "capabilities": {"pause": True},
                    "reset": {"default_scope": "state"},
                },
                "reset may be configured only",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "capabilities": {},
                },
                "must declare at least one supported operation",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "capabilities": {"reset": True},
                    "reset": {"default_scope": "time"},
                },
                "simulation_control.reset.default_scope must be one of",
            ),
            (
                {
                    "kind": "ros2_simulation_interfaces",
                    "capabilities": {"reset": True},
                    "reset": {"resume_play_after_reset": "yes"},
                },
                "simulation_control.reset.resume_play_after_reset must be a boolean",
            ),
        ],
    )
    def test_simulation_control_validation_rejects_invalid_contract(
        self,
        simulation_control,
        match,
    ):
        with pytest.raises(DeploymentValidationError, match=match):
            Deployment.from_dict(
                _minimal_deployment(simulation_control=simulation_control)
            )

    def test_validate_capabilities_includes_simulation_control_keys(self):
        data = _minimal_deployment(
            simulation_control={
                "kind": "ros2_simulation_interfaces",
                "capabilities": {"reset": True},
            },
        )
        data["adapters"]["planning"]["capability_keys"] = [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.profile.first_feasible",
            "planning.profile.balanced",
            "planning.profile.quality_bounded",
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
            "execution.feedback.state_snapshots",
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]

        class ProgramRequiresReset:
            def required_capabilities(self):
                return ["simulator.control.reset"]

        Deployment.from_dict(data).validate_capabilities(ProgramRequiresReset())

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

    def test_model_endpoint_capabilities_and_assistant_parsed(self):
        deploy = Deployment.from_dict(
            _minimal_deployment(
                model_endpoints={
                    "assistant_model": {
                        "provider": "openai",
                        "model_id": "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge",
                        "base_url": "http://127.0.0.1:8000/v1",
                        "default_timeout_s": 12.5,
                        "max_tokens": 512,
                        "capabilities": {
                            "supports_response_tools": True,
                            "supports_json_object": True,
                            "supports_image_input": True,
                            "supported_image_transports": [
                                "inline_data_url",
                                "http_url",
                            ],
                            "tool_call_family": "openai_chat_tools",
                            "latency_budget_ms": 2500,
                        },
                    },
                },
                assistant={
                    "provider": "nemoclaw",
                    "endpoint_alias": "assistant_model",
                    "enabled": True,
                },
            )
        )

        endpoint = deploy.model_endpoints["assistant_model"]
        assert endpoint.default_timeout_s == pytest.approx(12.5)
        assert endpoint.max_tokens == 512
        assert endpoint.capabilities.supports_response_tools is True
        assert endpoint.capabilities.supported_image_transports == [
            "inline_data_url",
            "http_url",
        ]
        assert endpoint.capabilities.tool_call_family == "openai_chat_tools"
        assert endpoint.capabilities.latency_budget_ms == 2500
        assert deploy.assistant is not None
        assert deploy.assistant.provider == "nemoclaw"
        assert deploy.assistant.endpoint_alias == "assistant_model"

        serialized = deploy.to_dict()
        assert serialized["model_endpoints"]["assistant_model"]["capabilities"][
            "supports_response_tools"
        ] is True
        assert serialized["assistant"]["endpoint_alias"] == "assistant_model"

    def test_model_endpoint_profile_registry_resolves_relative_profile_ref(self, tmp_path):
        registry_path = tmp_path / "model_endpoint_profiles.yaml"
        registry_path.write_text(
            yaml.safe_dump(
                {
                    "api_version": "manyforge.model_endpoint_profiles.v0",
                    "schema_version": "0.1.0",
                    "profiles": {
                        "thor_manyforge_qwen36": {
                            "provider": "openai",
                            "model_id": "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge",
                            "base_url": "http://127.0.0.1:8000/v1",
                            "max_tokens": 512,
                            "capabilities": {
                                "supports_response_tools": True,
                                "supports_json_object": True,
                                "supports_image_input": True,
                                "supported_image_transports": [
                                    "inline_data_url",
                                    "http_url",
                                ],
                                "tool_call_family": "openai_chat_tools",
                            },
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        deployment_path = tmp_path / "demo.deployment.yaml"
        deployment_path.write_text(
            yaml.safe_dump(
                _minimal_deployment(
                    model_endpoint_profile_registry="model_endpoint_profiles.yaml",
                    model_endpoints={
                        "selection_model": {
                            "profile_ref": "thor_manyforge_qwen36",
                            "max_tokens": 384,
                            "capabilities": {
                                "supports_image_input": False,
                                "supported_image_transports": [],
                            },
                        }
                    },
                )
            ),
            encoding="utf-8",
        )

        deploy = Deployment(deployment_path)

        endpoint = deploy.model_endpoints["selection_model"]
        assert endpoint.profile_ref == "thor_manyforge_qwen36"
        assert endpoint.provider == "openai"
        assert endpoint.model_id == "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
        assert endpoint.base_url == "http://127.0.0.1:8000/v1"
        assert endpoint.max_tokens == 384
        assert endpoint.capabilities.supports_response_tools is True
        assert endpoint.capabilities.supports_image_input is False
        assert endpoint.capabilities.supported_image_transports == []

        serialized = deploy.to_dict()
        assert serialized["model_endpoint_profile_registry"] == (
            "model_endpoint_profiles.yaml"
        )
        assert serialized["model_endpoints"]["selection_model"]["profile_ref"] == (
            "thor_manyforge_qwen36"
        )
        assert serialized["model_endpoints"]["selection_model"]["model_id"] == (
            "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
        )


class TestDeploymentValidation:
    def test_invalid_api_version(self):
        path = _write_deployment(_minimal_deployment(api_version="wrong"))
        with pytest.raises(DeploymentValidationError, match="api_version"):
            Deployment(path)
        os.unlink(path)

    def test_rejects_removed_cooperative_interrupt_fields(self):
        data = _minimal_deployment(interrupt_sources=[])
        with pytest.raises(
            DeploymentValidationError,
            match=r"Cooperative interrupt fields.*interrupt_sources",
        ):
            Deployment.from_dict(data)

    def test_update_from_dict_rejects_removed_cooperative_interrupt_fields(self):
        deploy = Deployment.from_dict(_minimal_deployment())
        with pytest.raises(
            DeploymentValidationError,
            match=r"Cooperative interrupt fields.*interrupt_sources",
        ):
            deploy.update_from_dict({"interrupt_sources": []})

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

    def test_invalid_schema_version(self):
        path = _write_deployment(_minimal_deployment(schema_version="not-semver"))
        with pytest.raises(DeploymentValidationError, match="schema_version must match X.Y.Z"):
            Deployment(path)
        os.unlink(path)

    def test_unknown_program_profile(self):
        path = _write_deployment(_minimal_deployment(program_profile="v9.9"))
        with pytest.raises(DeploymentValidationError, match="Unknown program profile"):
            Deployment(path)
        os.unlink(path)

    def test_missing_name(self):
        path = _write_deployment(_minimal_deployment(name=""))
        with pytest.raises(DeploymentValidationError, match="name"):
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

    def test_rejects_adapter_capability_keys_when_not_list(self):
        data = _minimal_deployment()
        data["adapters"]["planning"]["capability_keys"] = "planning.motion.joint_goal"
        path = _write_deployment(data)
        with pytest.raises(
            DeploymentValidationError,
            match=r"adapters\.planning\.capability_keys must be a list",
        ):
            Deployment(path)
        os.unlink(path)

    def test_rejects_unknown_adapter_capability_keys(self):
        data = _minimal_deployment()
        data["adapters"]["planning"]["capability_keys"] = ["unknown.capability"]
        path = _write_deployment(data)
        with pytest.raises(
            DeploymentValidationError,
            match=r"adapters\.planning\.capability_keys contains unknown capability keys",
        ):
            Deployment(path)
        os.unlink(path)

    def test_rejects_unknown_model_image_transport(self):
        data = _minimal_deployment(
            model_endpoints={
                "vlm": {
                    "provider": "openai",
                    "model_id": "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge",
                    "capabilities": {
                        "supported_image_transports": ["ftp_url"],
                    },
                },
            },
        )
        with pytest.raises(
            DeploymentValidationError,
            match="supported_image_transports.*must be one of",
        ):
            Deployment.from_dict(data)

    def test_rejects_unknown_model_endpoint_profile_ref(self, tmp_path):
        registry_path = tmp_path / "model_endpoint_profiles.yaml"
        registry_path.write_text(
            yaml.safe_dump(
                {
                    "api_version": "manyforge.model_endpoint_profiles.v0",
                    "schema_version": "0.1.0",
                    "profiles": {},
                }
            ),
            encoding="utf-8",
        )
        deployment_path = tmp_path / "demo.deployment.yaml"
        deployment_path.write_text(
            yaml.safe_dump(
                _minimal_deployment(
                    model_endpoint_profile_registry="model_endpoint_profiles.yaml",
                    model_endpoints={
                        "vision_model": {
                            "profile_ref": "missing_profile",
                        }
                    },
                )
            ),
            encoding="utf-8",
        )
        with pytest.raises(
            DeploymentValidationError,
            match="references unknown model endpoint profile",
        ):
            Deployment(deployment_path)

    def test_rejects_live_assistant_without_endpoint_alias(self):
        data = _minimal_deployment(assistant={"provider": "nemoclaw"})
        with pytest.raises(
            DeploymentValidationError,
            match="assistant.endpoint_alias is required",
        ):
            Deployment.from_dict(data)

    def test_rejects_live_assistant_endpoint_without_base_url(self):
        data = _minimal_deployment(
            model_endpoints={
                "assistant_model": {
                    "provider": "openai",
                    "model_id": "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge",
                },
            },
            assistant={
                "provider": "openclaw",
                "endpoint_alias": "assistant_model",
            },
        )
        with pytest.raises(
            DeploymentValidationError,
            match="must reference a model endpoint with base_url",
        ):
            Deployment.from_dict(data)

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
        path = _write_deployment(data)
        deploy = Deployment(path)
        assert deploy.description == ""
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

class TestAssistantModeValidation:
    def test_parses_assistant_modes_and_tool_catalog(self):
        no_args_schema = {
            "type": "object",
            "properties": {},
            "additionalProperties": False,
        }
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        tools=["program_read", "tree.insert_node"],
                    ),
                },
                tool_catalog=[
                    {
                        "id": "program_read",
                        "effect": "read_only",
                        "description": "Read the loaded program.",
                        "input_schema": no_args_schema,
                    },
                    {
                        "id": "tree.insert_node",
                        "effect": "proposal",
                        "description": "Propose a tree insertion.",
                    },
                ],
            )
        )

        assert "composer-assistant" in deployment.assistant_modes
        assert deployment.assistant_modes["composer-assistant"].catalog.tools == [
            "program_read",
            "tree.insert_node",
        ]
        assert deployment.tool_catalog["program_read"].effect == "read_only"
        assert deployment.tool_catalog["program_read"].extra["input_schema"] == no_args_schema
        assert deployment.resolve_assistant_mode_catalog("composer-assistant")["tools"][0][
            "input_schema"
        ] == no_args_schema
        assert deployment.to_dict()["assistant_modes"]["composer-assistant"]["catalog"]["tools"] == [
            "program_read",
            "tree.insert_node",
        ]
        assert deployment.to_dict()["tool_catalog"][0]["input_schema"] == no_args_schema

    def test_rejects_assistant_modes_without_tool_catalog(self):
        with pytest.raises(DeploymentValidationError, match="tool_catalog is required"):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes={
                        "composer-assistant": _assistant_mode_block(
                            tools=["program_read"],
                        ),
                    },
                )
            )

    def test_rejects_invalid_assistant_tool_input_schema(self):
        with pytest.raises(DeploymentValidationError, match="input_schema.type"):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes={
                        "composer-assistant": _assistant_mode_block(
                            tools=["program_read"],
                        ),
                    },
                    tool_catalog=[
                        {
                            "id": "program_read",
                            "effect": "read_only",
                            "input_schema": {"type": "array"},
                        },
                    ],
                )
            )

    def test_rejects_assistant_modes_when_not_mapping(self):
        with pytest.raises(DeploymentValidationError, match="assistant_modes must be a mapping"):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes=[],
                )
            )

    def test_rejects_assistant_mode_unknown_tool(self):
        with pytest.raises(DeploymentValidationError, match="unknown tool 'missing.tool'"):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes={
                        "composer-assistant": _assistant_mode_block(
                            tools=["missing.tool"],
                        ),
                    },
                    tool_catalog=_tool_catalog("program_read"),
                )
            )

    def test_rejects_query_mode_with_non_read_only_tool(self):
        with pytest.raises(DeploymentValidationError, match="query.*read_only"):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes={
                        "query": _assistant_mode_block(tools=["tree.insert_node"]),
                    },
                    tool_catalog=_tool_catalog("tree.insert_node", effect="proposal"),
                )
            )

    def test_allows_composer_draft_mutating_tools_only_in_composer_assistant(self):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        tools=["scene_draft_add_object"],
                    ),
                },
                tool_catalog=_tool_catalog(
                    "scene_draft_add_object",
                    effect="composer_draft_mutating",
                ),
            )
        )

        assert (
            deployment.tool_catalog["scene_draft_add_object"].effect
            == "composer_draft_mutating"
        )

    def test_rejects_composer_draft_mutating_tools_outside_composer_assistant(self):
        with pytest.raises(
            DeploymentValidationError,
            match="composer draft mutating tool",
        ):
            Deployment.from_dict(
                _minimal_deployment(
                    assistant_modes={
                        "error-recovery": _assistant_mode_block(
                            session_kind="delegated_recovery",
                            approval_mode="policy_granted",
                            review="bounded_by_recovery_profile",
                            tools=["scene_draft_add_object"],
                        ),
                    },
                    tool_catalog=_tool_catalog(
                        "scene_draft_add_object",
                        effect="composer_draft_mutating",
                    ),
                )
            )

    def test_validates_mode_skill_requires_tools(self, catalog, tmp_path):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        skills=["assist_skill"],
                        tools=["program_read"],
                    ),
                },
                tool_catalog=_tool_catalog("program_read"),
            )
        )
        skills = _skill_catalog(tmp_path, requires_tools=["program_read"])

        deployment.validate_assistant_modes(catalog, skills)

    def test_rejects_mode_skill_missing_required_tools(self, catalog, tmp_path):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        skills=["assist_skill"],
                        tools=["program_read"],
                    ),
                },
                tool_catalog=_tool_catalog("program_read", "tree.insert_node"),
            )
        )
        skills = _skill_catalog(tmp_path, requires_tools=["tree.insert_node"])

        with pytest.raises(DeploymentValidationError, match="without required tools"):
            deployment.validate_assistant_modes(catalog, skills)

    def test_rejects_unknown_mode_skill(self, catalog, tmp_path):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        skills=["missing_skill"],
                        tools=["program_read"],
                    ),
                },
                tool_catalog=_tool_catalog("program_read"),
            )
        )
        skills = _skill_catalog(tmp_path, requires_tools=[])

        with pytest.raises(DeploymentValidationError, match="unknown skills"):
            deployment.validate_assistant_modes(catalog, skills)

    def test_rejects_unknown_mode_node(self, catalog, tmp_path):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        tools=["program_read"],
                        nodes=["missing_node"],
                    ),
                },
                tool_catalog=_tool_catalog("program_read"),
            )
        )
        skills = _skill_catalog(tmp_path, requires_tools=[])

        with pytest.raises(DeploymentValidationError, match="unknown nodes"):
            deployment.validate_assistant_modes(catalog, skills)

    def test_rejects_instrument_namespace_collision(self, catalog, tmp_path):
        deployment = Deployment.from_dict(
            _minimal_deployment(
                assistant_modes={
                    "composer-assistant": _assistant_mode_block(
                        tools=["reset_trajectories"],
                    ),
                },
                tool_catalog=_tool_catalog("reset_trajectories"),
            )
        )
        skills = _skill_catalog(tmp_path, requires_tools=[])

        with pytest.raises(DeploymentValidationError, match="globally disjoint"):
            deployment.validate_assistant_modes(catalog, skills)


class TestCrossArtifactValidation:
    def test_validate_against_program_success(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        prog = Program(PROGRAM_FIXTURE_PATH, catalog)
        deploy.validate_against_program(prog)

    def test_validate_profile_mismatch(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.1",  # different from deployment
            "name": "test",
            "catalog_version": "0.4.0",
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

    def test_validate_robot_id_mismatch(self, catalog):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        data = {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "test",
            "catalog_version": "0.4.0",
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
            "catalog_version": "0.4.0",
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

    def test_validate_per_move_tcp_below_global_allowed(self, catalog):
        """R3: a per-move cap below the global cap loads fine."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)  # global cap 0.25
        prog = Program.from_dict(_program_with_move_tcp(0.1), catalog)
        deploy.validate_against_program(prog)  # no raise

    def test_validate_per_move_tcp_equal_global_allowed(self, catalog):
        """R3: a per-move cap exactly equal to the global cap is allowed."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        prog = Program.from_dict(_program_with_move_tcp(0.25), catalog)
        deploy.validate_against_program(prog)  # no raise

    def test_validate_per_move_tcp_above_global_rejected(self, catalog):
        """R3 (fail-closed): a per-move cap above the global cap is rejected at
        validation rather than silently clamped at runtime."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        prog = Program.from_dict(_program_with_move_tcp(0.4), catalog)
        with pytest.raises(
            DeploymentValidationError, match="exceeds the deployment global cap"
        ):
            deploy.validate_against_program(prog)

    def test_validate_per_move_tcp_param_ref_above_global_rejected(self, catalog):
        """R3: a ${param:...} per-move cap is resolved against program defaults
        and rejected when the default exceeds the global cap."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        params = [
            {
                "name": "approach_tcp_cap",
                "type": "float",
                "default": 0.4,
                "constraints": {"min_exclusive": 0.0},
                "description": "per-move tcp cap",
            }
        ]
        prog = Program.from_dict(
            _program_with_move_tcp(
                "${param:approach_tcp_cap}", parameters=params
            ),
            catalog,
        )
        with pytest.raises(
            DeploymentValidationError, match="exceeds the deployment global cap"
        ):
            deploy.validate_against_program(prog)

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
        assert deploy.grippers["primary"].srdf_presets == {
            "open": 0.0,
            "closed": 0.66,
        }
        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    def test_validate_against_isaac_light_ros2_control_program_success(self, catalog):
        """The Isaac light-scene ROS execution deployment matches the saved program."""
        deploy = Deployment(ISAAC_LIGHT_ROS2_CONTROL_DEPLOYMENT_PATH)
        prog = Program(ISAAC_LIGHT_PROGRAM_PATH, catalog)
        assert (
            deploy.adapters["execution"].adapter_id
            == "execution.ros2_control_follow_joint_trajectory"
        )
        assert deploy.adapters["execution"].config["arm_action"] == (
            "/isaac_arm_controller/follow_joint_trajectory"
        )
        assert deploy.grippers["primary"].command_mode == "ros_gripper_command"
        assert deploy.grippers["primary"].actuation_kind == "position_commanded"
        assert deploy.grippers["primary"].state_authority == "sim_observed"
        assert deploy.grippers["primary"].completion_policy == "position_reached"
        assert deploy.grippers["primary"].transport["kind"] == "ros_gripper_command"
        assert deploy.grippers["primary"].transport["result_timeout_s"] == 15.0
        assert deploy.adapters["execution"].config["gripper"]["action_name"] == (
            "/robotiq_gripper_controller/gripper_cmd"
        )
        assert (
            deploy.adapters["execution"].config["gripper"]["result_timeout_s"]
            == deploy.grippers["primary"].transport["result_timeout_s"]
        )
        assert (
            deploy.orchestrator.config["runtime_dynamic_body_max_age_cap_ms"]
            == 750.0
        )
        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    @pytest.mark.parametrize(
        "deployment_path",
        [
            ISAAC_LIGHT_ROS2_CONTROL_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ROS2_CONTROL_COLLISION_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ASSISTANT_DEPLOYMENT_PATH,
        ],
    )
    def test_isaac_light_gripper_deadline_ownership_is_ordered(
        self, deployment_path
    ):
        """The C++ owner must cancel before the simulator bridge emergency floor."""
        deploy = Deployment(deployment_path)
        gripper = deploy.grippers["primary"]
        bridge_config = yaml.safe_load(
            ISAAC_ROBOTIQ_GRIPPER_BRIDGE_CONFIG_PATH.read_text(encoding="utf-8")
        )
        bridge_timeout_s = float(
            bridge_config["manyforge_isaac_gripper_action_bridge"]["ros__parameters"][
                "goal_timeout_s"
            ]
        )
        bridge_parameters = bridge_config["manyforge_isaac_gripper_action_bridge"][
            "ros__parameters"
        ]
        backend_deadline_s = (
            gripper.motion_time_s + gripper.transport["result_timeout_s"]
        )
        adapter_timeout_s = deploy.adapters["execution"].config["gripper"][
            "result_timeout_s"
        ]

        assert adapter_timeout_s == gripper.transport["result_timeout_s"]
        assert backend_deadline_s == pytest.approx(15.8)
        assert bridge_timeout_s == pytest.approx(20.0)

        assert bridge_timeout_s > backend_deadline_s
        assert bridge_parameters["commanded_joint"] == gripper.joint_name
        assert bridge_parameters["command_joint_names"] == [gripper.joint_name]
        assert bridge_parameters["command_multipliers"] == [1.0]
        assert bridge_parameters["command_offsets"] == [0.0]
        assert bridge_parameters["completion_joint_names"] == [
            "finger_joint",
            "left_inner_finger_joint",
            "left_inner_knuckle_joint",
            "right_inner_knuckle_joint",
            "right_outer_knuckle_joint",
            "right_inner_finger_joint",
        ]
        assert bridge_parameters["completion_multipliers"] == [
            1.0,
            1.0,
            -1.0,
            -1.0,
            -1.0,
            1.0,
        ]
        assert bridge_parameters["completion_offsets"] == [0.0] * 6
        assert bridge_parameters["completion_settle_time_s"] == pytest.approx(0.1)
        assert bridge_parameters["completion_min_samples"] == 3
        assert bridge_parameters["require_all_state_joints"] is True
        assert gripper.transport["position_tolerance"] == pytest.approx(
            bridge_parameters["completion_tolerance_rad"]
        )

    @pytest.mark.parametrize(
        "deployment_path",
        [
            ISAAC_LIGHT_ROS2_CONTROL_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ROS2_CONTROL_COLLISION_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ASSISTANT_DEPLOYMENT_PATH,
        ],
    )
    def test_isaac_light_measured_tcp_tracks_composed_ur10e_hierarchy(
        self, deployment_path
    ):
        deploy = Deployment(deployment_path)

        measured_tcp = deploy.pose_sources["isaac_scene_objects"].measured_tcp
        assert measured_tcp.entity == (
            "/World/ur10e_robotiq2f_140_ROS/ur10e_robotiq2f_140/"
            "ee_link/robotiq_arg2f_base_link"
        )
        assert measured_tcp.tcp_frame_id == "grasp_frame"
        assert measured_tcp.entity_to_tcp_position_xyz == (0.0, 0.0, 0.2)
        assert measured_tcp.entity_to_tcp_orientation_xyzw == (0.0, 0.0, 0.0, 1.0)

    @pytest.mark.parametrize(
        "deployment_path",
        [
            ISAAC_LIGHT_ROS2_CONTROL_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ROS2_CONTROL_COLLISION_DEPLOYMENT_PATH,
            ISAAC_LIGHT_ASSISTANT_DEPLOYMENT_PATH,
        ],
    )
    def test_isaac_light_measured_robot_uses_complete_gripper_state_stream(
        self, deployment_path
    ):
        deploy = Deployment(deployment_path)
        measured_robot = deploy.pose_sources["isaac_scene_objects"].measured_robot
        bridge_config = yaml.safe_load(
            ISAAC_ROBOTIQ_GRIPPER_BRIDGE_CONFIG_PATH.read_text(encoding="utf-8")
        )
        bridge_state_topic = bridge_config[
            "manyforge_isaac_gripper_action_bridge"
        ]["ros__parameters"]["state_topic"]

        assert measured_robot is not None
        assert bridge_state_topic == "/isaac_joint_states"
        assert measured_robot.joint_state_topic == bridge_state_topic

    def test_validate_against_isaac_lite6_ros2_control_program_success(self, catalog):
        """The Isaac Lite6 ROS execution deployment matches its pick-and-place program."""
        deploy = Deployment(ISAAC_LITE6_PICK_PLACE_DEPLOYMENT_PATH)
        prog = Program(ISAAC_LITE6_PICK_PLACE_PROGRAM_PATH, catalog)

        assert deploy.robot_config is not None
        assert deploy.robot_config.robot_id == "lite6"
        assert deploy.robot_config.group_id == "lite6"
        assert (
            deploy.adapters["execution"].adapter_id
            == "execution.ros2_control_follow_joint_trajectory"
        )
        execution_config = deploy.adapters["execution"].config
        assert execution_config["required_joints"] == [
            "joint1",
            "joint2",
            "joint3",
            "joint4",
            "joint5",
            "joint6",
        ]
        assert execution_config["arm_action"] == (
            "/isaac_arm_controller/follow_joint_trajectory"
        )
        assert execution_config["decelerate_on_cancel"] is True
        assert "gripper" not in execution_config
        assert deploy.grippers["primary"].command_mode == "ros_gripper_command"
        assert deploy.grippers["primary"].group_id == "lite_gripper"
        assert deploy.grippers["primary"].joint_name == "right_finger_joint"
        assert deploy.grippers["primary"].actuation_kind == "pneumatic"
        assert deploy.grippers["primary"].state_authority == "sim_observed"
        assert deploy.grippers["primary"].completion_policy == "dwell_elapsed"
        assert deploy.grippers["primary"].transport == {
            "kind": "isaac_joint_state_effort",
            "command_topic": "/isaac_joint_commands_gripper",
            "state_topic": "/isaac_joint_states",
            "joint_names": ["right_finger_joint"],
            "close_effort": -5.0,
            "open_effort": 5.0,
            "publish_nominal_position": True,
            "dwell_s": 0.8,
        }
        assert deploy.grippers["primary"].srdf_presets == {
            "open": 0.0081,
            "close": 0.0,
            "closed": 0.0,
        }

        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    def test_validate_against_trocar_lite6_foundationpose_program_success(
        self,
        catalog,
    ):
        """The Lite6 trocar FoundationPose scenario pins the live pose-source gates."""
        deploy = Deployment(TROCAR_LITE6_FOUNDATIONPOSE_DEPLOYMENT_PATH)
        prog = Program(TROCAR_LITE6_FOUNDATIONPOSE_PROGRAM_PATH, catalog)

        source = deploy.pose_sources["foundationpose_trocar"]
        assert deploy.grippers["primary"].actuation_kind == "pneumatic"
        assert deploy.grippers["primary"].completion_policy == "dwell_elapsed"
        assert deploy.grippers["primary"].transport["kind"] == "isaac_joint_state_effort"
        assert source.kind == "detection3d_array_topic"
        assert source.topic == "/output"
        assert source.frame_id == "link_base"
        assert source.stale_after_s == 5.0
        graspable = source.objects["graspable"]
        assert graspable.apply_policy == "before_planning"
        assert graspable.confidence_min == 0.0
        assert graspable.metadata["selection"] == "first"
        assert graspable.metadata["max_position_variance_m2"] == 0.0025
        plan_node = next(
            child
            for child in prog.tree.children
            if child.id == "plan_pick_sequence"
        )
        assert plan_node.params["force_vertical_pick"] is True
        assert plan_node.params["enable_min_tcp_z"] is True
        assert plan_node.params["min_tcp_z_m"] == 0.016
        assert plan_node.params["pick_offset_reference"] == {
            "type": "robot_link",
            "id": "link_tcp",
        }
        assert plan_node.params["approach_offset_reference"] == {
            "type": "robot_link",
            "id": "link_tcp",
        }
        assert plan_node.params["approach_move_node_name"] == (
            "move_to_planned_pick_approach"
        )
        assert plan_node.params["pick_move_node_name"] == "move_to_planned_pick"
        assert plan_node.params["retreat_move_node_name"] == "retreat_after_pick"
        pick_plan_namespace = prog.blackboard_namespaces["pick_plan"]
        assert any(field.name == "retreat_pose" for field in pick_plan_namespace.fields)
        graspable_resource = next(
            resource for resource in prog.scene if resource.object_id == "graspable"
        )
        assert graspable_resource.metadata["support_contacts"] == [
            {
                "support_object_id": "ground",
                "release_evidence_mode": "signed_penetration",
                "enforce_max_start_penetration": True,
                "max_start_penetration_m": 0.03,
                "release_tolerance_m": 0.001,
                "min_release_progress_m": 0.0,
                "require_separation_by_end": True,
                "require_strictly_decreasing_penetration": True,
                "allow_recontact": False,
            }
        ]

        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    def test_lite6_isaac_ros2_control_scaffold_matches_direct_transport_contract(self):
        """The Lite6 Isaac scaffold pins direct effort plus bridge compatibility."""
        urdf_text = LITE6_ISAAC_ROS2_CONTROL_URDF_PATH.read_text(encoding="utf-8")
        urdf = ET.fromstring(urdf_text.format(
            joint_commands_topic="/isaac_joint_commands",
            joint_states_topic="/isaac_joint_states",
            trigger_joint_command_threshold="-1.0",
            sum_wrapped_joint_states="false",
            joint1_initial="0.0",
            joint2_initial="0.0",
            joint3_initial="1.57",
            joint4_initial="0.0",
            joint5_initial="0.0",
            joint6_initial="0.0",
        ))
        ros2_control = urdf.find("./ros2_control")
        assert ros2_control is not None
        assert [
            joint.attrib["name"] for joint in ros2_control.findall("./joint")
        ] == ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]
        assert urdf.find("./link[@name='base_link']") is None
        assert urdf.find("./joint[@name='base_link_to_link_base']") is None
        assert urdf.find("./link[@name='link_base']") is not None
        assert urdf.find("./link[@name='link_tcp']") is not None
        assert urdf.find("./link[@name='lite_gripper_left_finger']") is not None
        assert urdf.find("./link[@name='lite_gripper_right_finger']") is not None
        left_finger = urdf.find("./joint[@name='left_finger_joint']")
        assert left_finger is not None
        mimic = left_finger.find("mimic")
        assert mimic is not None
        assert mimic.attrib.get("joint") == "right_finger_joint"
        assert urdf.find("./gazebo") is not None
        assert urdf.find("./transmission") is not None

        controller_config = yaml.safe_load(
            LITE6_ISAAC_CONTROLLER_CONFIG_PATH.read_text(encoding="utf-8")
        )
        controller = controller_config["isaac_arm_controller"]["ros__parameters"]
        assert controller["joints"] == [
            "joint1",
            "joint2",
            "joint3",
            "joint4",
            "joint5",
            "joint6",
        ]
        assert controller["command_interfaces"] == ["position"]
        assert controller["state_interfaces"] == ["position", "velocity"]
        assert controller["constraints"]["decelerate_on_cancel"] is True

        bridge_config = yaml.safe_load(
            LITE6_ISAAC_GRIPPER_BRIDGE_CONFIG_PATH.read_text(encoding="utf-8")
        )["manyforge_isaac_gripper_action_bridge"]["ros__parameters"]
        assert bridge_config["action_name"] == "/lite_gripper_controller/gripper_cmd"
        assert bridge_config["command_topic"] == "/isaac_joint_commands_gripper"
        assert bridge_config["commanded_joint"] == "right_finger_joint"
        assert bridge_config["command_joint_names"] == ["right_finger_joint"]
        assert bridge_config["completion_joint_names"] == ["right_finger_joint"]

    def test_lite6_authority_runtime_contract_validator_accepts_static_scaffold(self):
        """The non-live RMA-7 validator pins the Lite6 Isaac static contract."""
        spec = importlib.util.spec_from_file_location(
            "validate_lite6_authority_runtime_contract",
            LITE6_AUTHORITY_RUNTIME_CONTRACT_VALIDATOR_SCRIPT,
        )
        assert spec is not None
        assert spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        args = type(
            "Args",
            (),
            {
                "deployment": ISAAC_LITE6_PICK_PLACE_DEPLOYMENT_PATH,
                "ros2_control_urdf": LITE6_ISAAC_ROS2_CONTROL_URDF_PATH,
                "controllers": LITE6_ISAAC_CONTROLLER_CONFIG_PATH,
                "gripper_bridge": LITE6_ISAAC_GRIPPER_BRIDGE_CONFIG_PATH,
            },
        )()
        report = module.validate(args)

        assert report["schema"] == "manyforge.lite6_authority_runtime_contract.v1"
        assert report["robot_id"] == "lite6"
        assert report["arm_group"] == "lite6"
        assert report["arm_joints"] == [
            "joint1",
            "joint2",
            "joint3",
            "joint4",
            "joint5",
            "joint6",
        ]
        assert report["gripper_group"] == "lite_gripper"
        assert report["gripper_command_joint"] == "right_finger_joint"
        assert report["gripper_derived_joints"] == ["left_finger_joint"]
        assert report["gripper_transport_kind"] == "isaac_joint_state_effort"
        assert report["gripper_bridge_config"] is None
        assert len(report["canonical_fingerprint"]) == 64

    def test_lite6_isaac_live_smoke_gate_is_profile_aware(self):
        """The Lite6 launcher smoke must validate Lite6 joints, not UR joints."""
        validator_text = LITE6_ISAAC_SMOKE_VALIDATOR_SCRIPT.read_text(
            encoding="utf-8"
        )
        launch_text = LAUNCH_SCRIPT.read_text(encoding="utf-8")

        for joint in ["joint1", "joint2", "joint3", "joint4", "joint5", "joint6"]:
            assert joint in validator_text
        assert "/lite_gripper_controller/gripper_cmd" in validator_text
        assert "/isaac_joint_commands_gripper" in validator_text
        assert "right_finger_joint" in validator_text

        assert 'SCENARIO}" == "isaac-lite6-pick-place"' in launch_text
        assert "validate-isaac-lite6-smoke.py" in launch_text
        assert "--gripper-transport-kind" in launch_text
        assert "isaac_joint_state_effort" in launch_text
        assert "validate-isaac-lite6-smoke-artifact.py" in launch_text
        assert '${SCENARIO}-lite6-live-smoke.json' in launch_text
        assert '${SCENARIO}-foundationpose-detection3d.json' in launch_text
        assert "| tee" in launch_text
        assert "smoke_isaac_arm_action.py" in launch_text
        assert "--output" in validator_text
        assert "manyforge.isaac_lite6_live_smoke.v1" in validator_text
        assert "arm_restore_result" in validator_text
        assert "raw_command_position" in validator_text
        assert "raw_command_effort" in validator_text

    def test_lite6_rma0_active_artifacts_do_not_use_rejected_surfaces(self):
        """RMA-0 keeps Lite6 active artifacts on canonical model semantics."""
        repo_root = Path(__file__).resolve().parents[2]
        active_paths = [
            LITE6_PICK_PLACE_DEPLOYMENT_PATH,
            ISAAC_LITE6_PICK_PLACE_DEPLOYMENT_PATH,
            LITE6_MODEL_DIR / "README.md",
            LITE6_MODEL_DIR / "lite6_lite_gripper.urdf",
            LITE6_MODEL_DIR / "lite6_lite_gripper.srdf",
            LITE6_ISAAC_ROS2_CONTROL_URDF_PATH,
            repo_root / "scripts" / "models" / "export_lite6_lite_gripper_model.py",
            LITE6_AUTHORITY_RUNTIME_CONTRACT_VALIDATOR_SCRIPT,
            repo_root / "scripts" / "qualify-lite6-bringup.sh",
        ]
        forbidden_terms = [
            "coupled_joints",
            "coupledJoints",
            "GripperCoupled",
            "roboplan_compat",
            "strip_mimic_tags_for_roboplan_compat",
            "write_roboplan_compat",
        ]
        for path in active_paths:
            text = path.read_text(encoding="utf-8")
            for term in forbidden_terms:
                assert term not in text, f"{term} present in {path}"
        assert not list(LITE6_MODEL_DIR.glob("*roboplan_compat*"))

        qualifier = (repo_root / "scripts" / "qualify-lite6-bringup.sh").read_text(
            encoding="utf-8"
        )
        assert "test_native_e2e.py -k \"lite6\"" not in qualifier
        assert "validate_lite6_authority_runtime_contract.py" in qualifier
        assert "MANYFORGE_ROBOPLAN_PY_DIR" in qualifier
        assert "lite6_canonical_gripper_command" in qualifier
        assert "lite6_arm_pick_validation" in qualifier
        assert "lite6_manyforge_collision" in qualifier
        assert "pytest evidence summary did not match" in qualifier
        assert "Lite6 non-live model-authority qualification passed" in qualifier

    def test_lite6_live_smoke_artifact_validator_accepts_complete_evidence(
        self, tmp_path
    ):
        """The recorded live-smoke artifact has a deterministic acceptance gate."""
        artifact = tmp_path / "isaac-lite6-pick-place-live-smoke.json"
        artifact.write_text(
            json.dumps(_lite6_live_smoke_artifact_payload()),
            encoding="utf-8",
        )

        completed = subprocess.run(
            [
                sys.executable,
                str(LITE6_ISAAC_SMOKE_ARTIFACT_VALIDATOR_SCRIPT),
                str(artifact),
                "--json",
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        report = json.loads(completed.stdout)
        assert report["passed"] is True

    def test_lite6_live_smoke_artifact_validator_rejects_incomplete_evidence(
        self, tmp_path
    ):
        """The recorded live-smoke artifact gate fails closed on weak evidence."""
        cases = {
            "failed_run": lambda payload: payload.update({"passed": False}),
            "backwards_timestamps": lambda payload: payload["details"].update(
                {
                    "started_at_utc": "2026-06-25T10:00:03+00:00",
                    "ended_at_utc": "2026-06-25T10:00:00+00:00",
                }
            ),
            "missing_lite6_joint": lambda payload: payload["details"][
                "arm_joints"
            ].pop(),
            "missing_raw_gripper_command": lambda payload: payload["details"][
                "gripper_open_result"
            ].pop("raw_command_position"),
            "missing_direct_observed_state": lambda payload: payload["details"][
                "gripper_open_result"
            ].pop("isaac_state_position"),
            "wrong_direct_completion_policy": lambda payload: payload["details"][
                "gripper_close_result"
            ].update({"completion_policy": "position_reached"}),
            "short_direct_dwell": lambda payload: payload["details"][
                "gripper_close_result"
            ].update({"dwell_elapsed_s": 0.1}),
            "excessive_arm_target_error": lambda payload: payload["details"][
                "arm_restore_result"
            ].update(
                {
                    "final_arm_state_rad": {
                        **payload["details"]["arm_restore_result"][
                            "final_arm_state_rad"
                        ],
                        "joint1": 0.5,
                    },
                    "max_abs_target_error_rad": 0.5,
                }
            ),
            "forged_arm_target_error": lambda payload: payload["details"][
                "arm_restore_result"
            ]["final_arm_state_rad"].update({"joint1": 0.5}),
        }
        for name, mutate in cases.items():
            payload = json.loads(json.dumps(_lite6_live_smoke_artifact_payload()))
            mutate(payload)
            artifact = tmp_path / f"{name}.json"
            artifact.write_text(json.dumps(payload), encoding="utf-8")

            completed = subprocess.run(
                [
                    sys.executable,
                    str(LITE6_ISAAC_SMOKE_ARTIFACT_VALIDATOR_SCRIPT),
                    str(artifact),
                    "--json",
                ],
                text=True,
                capture_output=True,
            )

            assert completed.returncode != 0, name
            report = json.loads(completed.stdout)
            assert report["passed"] is False
            assert report["error"]

    def test_lite6_lite_gripper_pick_and_place_deployment_validates(self, catalog):
        """The self-contained Lite6 native pick-and-place deployment matches its program."""
        deploy = Deployment(LITE6_PICK_PLACE_DEPLOYMENT_PATH)
        prog = Program(LITE6_PICK_PLACE_PROGRAM_PATH, catalog)

        assert deploy.robot_config is not None
        assert deploy.robot_config.robot_id == "lite6"
        assert deploy.robot_config.group_id == "lite6"
        assert deploy.robot_config.urdf_path.endswith("lite6_lite_gripper.urdf")
        assert deploy.robot_config.srdf_path.endswith("lite6_lite_gripper.srdf")
        assert "primary" in deploy.grippers
        assert deploy.grippers["primary"].command_mode == "simulated_joint"
        assert deploy.grippers["primary"].group_id == "lite_gripper"
        assert deploy.grippers["primary"].joint_name == "right_finger_joint"
        assert deploy.grippers["primary"].mount_frame == "link_eef"
        assert deploy.grippers["primary"].attach_frame == "link_tcp"
        assert deploy.grippers["primary"].tcp_frame == "link_tcp"
        assert deploy.grippers["primary"].srdf_presets == {
            "open": 0.0081,
            "close": 0.0,
            "closed": 0.0,
        }

        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

    def test_lite6_lite_gripper_model_bundle_is_self_contained(self):
        """The vendored Lite6 model must not depend on xarm_ros2 at runtime."""
        urdf_path = LITE6_MODEL_DIR / "lite6_lite_gripper.urdf"
        srdf_path = LITE6_MODEL_DIR / "lite6_lite_gripper.srdf"
        assert urdf_path.exists()
        assert srdf_path.exists()
        assert LITE6_STATIC_MANIFEST_PATH.exists()

        urdf_text = urdf_path.read_text(encoding="utf-8")
        assert "package://xarm_description" not in urdf_text
        assert "<gazebo" in urdf_text
        assert "<ros2_control" in urdf_text
        assert "<transmission" in urdf_text
        assert "<mimic" in urdf_text

        root = ET.parse(urdf_path).getroot()
        mesh_refs = [
            mesh.attrib["filename"]
            for mesh in root.iter("mesh")
            if mesh.attrib.get("filename")
        ]
        assert mesh_refs
        prefix = "package://manyforge_example_models/models/lite6_lite_gripper/"
        for ref in mesh_refs:
            assert ref.startswith(prefix)
            resolved = LITE6_MODEL_DIR / ref.removeprefix(prefix)
            assert resolved.exists(), ref
        for joint_name, (lower, upper) in LITE6_LIMITED_ARM_BOUNDS.items():
            joint = root.find(f"./joint[@name='{joint_name}']")
            assert joint is not None
            limit = joint.find("limit")
            assert limit is not None
            assert limit.attrib.get("lower") == lower
            assert limit.attrib.get("upper") == upper
        assert root.find("./link[@name='base_link']") is None
        assert root.find("./joint[@name='base_link_to_link_base']") is None
        left_finger = root.find("./joint[@name='left_finger_joint']")
        assert left_finger is not None
        mimic = left_finger.find("mimic")
        assert mimic is not None
        assert mimic.attrib.get("joint") == "right_finger_joint"

        srdf = ET.parse(srdf_path).getroot()
        lite6_joints = {
            joint.attrib.get("name")
            for joint in srdf.findall("./group[@name='lite6']/joint")
        }
        assert {"world_joint", "joint1", "joint2", "joint3", "joint4", "joint5", "joint6", "joint_eef", "gripper_fix", "joint_tcp"} <= lite6_joints
        assert srdf.find("./group[@name='lite_gripper']/joint[@name='right_finger_joint']") is not None
        assert srdf.find("./group[@name='lite_gripper']/joint[@name='left_finger_joint']") is not None
        state_names = {
            state.attrib.get("name")
            for state in srdf.findall("./group_state[@group='lite_gripper']")
        }
        assert {"open", "close", "closed"} <= state_names
        closed = srdf.find("./group_state[@group='lite_gripper'][@name='closed']")
        assert closed is not None
        assert {
            joint.attrib.get("name"): joint.attrib.get("value")
            for joint in closed.findall("./joint")
        } == {"left_finger_joint": "0", "right_finger_joint": "0"}
        assert srdf.findall("./disable_collisions")

        manifest = json.loads(LITE6_STATIC_MANIFEST_PATH.read_text(encoding="utf-8"))
        assert manifest["schema"] == "manyforge.lite6_static_model_manifest.v1"
        assert manifest["source"]["repository"] == (
            "https://github.com/pastoriomarco/xarm_ros2"
        )
        assert manifest["source"]["branch"] == "jazzy_no_gazebo"
        assert (
            manifest["source"]["commit"]
            == "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
        )
        expected_paths = {
            "README.md",
            "licenses/xarm_ros2.LICENSE",
            "lite6_lite_gripper.srdf",
            "lite6_lite_gripper.urdf",
        } | {
            ref.removeprefix(prefix)
            for ref in mesh_refs
        }
        manifest_entries = manifest["files"]
        manifest_paths = {entry["path"] for entry in manifest_entries}
        assert manifest_paths == expected_paths
        for entry in manifest_entries:
            path = LITE6_MODEL_DIR / entry["path"]
            assert path.is_file(), entry["path"]
            data = path.read_bytes()
            assert entry["size_bytes"] == len(data), entry["path"]
            assert entry["sha256"] == hashlib.sha256(data).hexdigest(), entry["path"]

    def test_lite6_static_manifest_validator_accepts_and_rejects_drift(
        self, tmp_path
    ):
        """The standalone manifest gate rejects static asset drift."""
        accepted = subprocess.run(
            [
                sys.executable,
                str(LITE6_STATIC_MANIFEST_VALIDATOR_SCRIPT),
                "--model-dir",
                str(LITE6_MODEL_DIR),
                "--json",
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        accepted_report = json.loads(accepted.stdout)
        assert accepted_report["passed"] is True
        assert accepted_report["file_count"] == 22

        copied_model = tmp_path / "lite6_lite_gripper"
        shutil.copytree(
            LITE6_MODEL_DIR,
            copied_model,
            ignore=shutil.ignore_patterns("manymove_runtime_capture"),
        )
        with (copied_model / "README.md").open("a", encoding="utf-8") as handle:
            handle.write("\nChecksum drift fixture.\n")

        rejected = subprocess.run(
            [
                sys.executable,
                str(LITE6_STATIC_MANIFEST_VALIDATOR_SCRIPT),
                "--model-dir",
                str(copied_model),
                "--json",
            ],
            text=True,
            capture_output=True,
        )
        assert rejected.returncode != 0
        rejected_report = json.loads(rejected.stdout)
        assert rejected_report["passed"] is False
        assert "README.md" in rejected_report["error"]

    def test_lite6_runtime_snapshot_comparison_accepts_documented_deltas(self, tmp_path):
        """The ManyMove runtime comparison gate allows only documented deltas."""
        completed = subprocess.run(
            [
                sys.executable,
                str(LITE6_RUNTIME_COMPARE_SCRIPT),
                "--runtime-urdf",
                str(LITE6_RUNTIME_CAPTURE_URDF_PATH),
                "--runtime-srdf",
                str(LITE6_RUNTIME_CAPTURE_SRDF_PATH),
                "--snapshot-dir",
                str(LITE6_MODEL_DIR),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        report = yaml.safe_load(completed.stdout)
        assert report["passed"] is True
        assert report["failures"] == []

    def test_lite6_runtime_capture_metadata_is_enforced(self, tmp_path):
        """The ManyMove runtime comparison gate pins branch and capture source."""
        completed = subprocess.run(
            [
                sys.executable,
                str(LITE6_RUNTIME_COMPARE_SCRIPT),
                "--runtime-urdf",
                str(LITE6_RUNTIME_CAPTURE_URDF_PATH),
                "--runtime-srdf",
                str(LITE6_RUNTIME_CAPTURE_SRDF_PATH),
                "--capture-json",
                str(LITE6_RUNTIME_CAPTURE_JSON_PATH),
                "--snapshot-dir",
                str(LITE6_MODEL_DIR),
            ],
            check=True,
            text=True,
            capture_output=True,
        )
        report = yaml.safe_load(completed.stdout)
        assert report["passed"] is True
        assert report["facts"]["runtime_capture_xarm_branch"] == "jazzy_no_gazebo"

        bad_capture = tmp_path / "bad_capture.json"
        payload = json.loads(LITE6_RUNTIME_CAPTURE_JSON_PATH.read_text(encoding="utf-8"))
        payload["xarm_ros2"]["branch"] = "jazzy"
        bad_capture.write_text(json.dumps(payload), encoding="utf-8")
        rejected = subprocess.run(
            [
                sys.executable,
                str(LITE6_RUNTIME_COMPARE_SCRIPT),
                "--runtime-urdf",
                str(LITE6_RUNTIME_CAPTURE_URDF_PATH),
                "--runtime-srdf",
                str(LITE6_RUNTIME_CAPTURE_SRDF_PATH),
                "--capture-json",
                str(bad_capture),
                "--snapshot-dir",
                str(LITE6_MODEL_DIR),
            ],
            text=True,
            capture_output=True,
        )
        assert rejected.returncode != 0
        rejected_report = yaml.safe_load(rejected.stdout)
        assert rejected_report["passed"] is False
        assert any("branch" in failure for failure in rejected_report["failures"])

    def test_lite6_runtime_capture_resolves_xarm_commit_from_evidence(self, tmp_path):
        """The capture helper must derive xarm provenance, not silently stale it."""
        spec = importlib.util.spec_from_file_location(
            "capture_manymove_lite6_descriptions",
            LITE6_RUNTIME_CAPTURE_SCRIPT,
        )
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        commit_file = tmp_path / ".xarm_ros2_commit"
        commit_file.write_text(
            "012731afb9ef1d4289ec45112e8d083ab59a5c1e\n",
            encoding="utf-8",
        )
        assert module.resolve_xarm_commit(None, commit_file, tmp_path / "missing") == (
            "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
        )
        assert module.resolve_xarm_commit(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            commit_file,
            tmp_path / "missing",
        ) == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        missing_file = tmp_path / "missing_commit"
        with pytest.raises(SystemExit):
            module.resolve_xarm_commit(None, missing_file, tmp_path / "missing")

    def test_assistant_modes_example_deployment_validates(self, catalog):
        """The assistant-mode example validates against Robotiq program and catalogs."""
        deploy = Deployment(ASSISTANT_MODES_EXAMPLE_DEPLOYMENT_PATH)
        prog = Program(UR10E_ROBOTIQ_EXAMPLE_PROGRAM_PATH, catalog)
        skills = load_skill_catalog(default_skills_dir(), catalog=catalog, strict=True)

        deploy.validate_against_program(prog)
        deploy.validate_capabilities()
        deploy.validate_assistant_modes(catalog, skills)
        assert "primary" in deploy.grippers
        assert deploy.grippers["primary"].tcp_frame == "grasp_frame"
        assert sorted(deploy.assistant_modes) == ["composer-assistant", "query"]
        resolved_catalog = deploy.resolve_assistant_mode_catalog("composer-assistant")
        assert resolved_catalog["skills"] == []
        # Tool order matches the deployment YAML's
        # assistant_modes.composer-assistant.catalog.tools list order.
        # `program_read` leads because the deployment puts the live-state
        # read first; `tree_draft_change_node_kind` (formerly
        # `tree_draft_swap_node`, renamed in 2026-05 to free the word
        # "swap" for sibling reordering prompts) was added when the
        # swap-class rules went in.
        assert [tool["id"] for tool in resolved_catalog["tools"]] == [
            "program_read",
            "catalog_read",
            "skills_read",
            "deployment_capabilities_read",
            "scene_inspect",
            "inspect_isaac_scene",
            "propose_scene_objects",
            "propose_scene_object_nodes",
            "scene_draft_add_object",
            "scene_draft_update_object",
            "scene_draft_upsert_objects",
            "scene_draft_remove_objects",
            "tree_draft_insert_node",
            "tree_draft_update_node_params",
            "tree_draft_delete_node",
            "tree_draft_move_node",
            "tree_draft_replace_subtree",
            "tree_draft_change_node_kind",
            "tree_draft_wrap_node",
            "program_draft_upsert_parameters",
            "program_draft_remove_parameters",
            "blackboard_draft_upsert_namespaces",
            "blackboard_draft_remove_namespaces",
            "blackboard_draft_upsert_keys",
            "blackboard_draft_remove_keys",
        ]
        assert sorted(resolved_catalog["nodes"]) == sorted(
            [
                "attach_object_to_link",
                "detach_object_from_link",
                "add_collision_object",
                "remove_collision_object",
                "upsert_collision_object",
                "update_collision_object_pose",
                "sequence",
                "fallback",
                "parallel",
                "repeat",
                "retry",
                "inverter",
                "move_manipulator_action",
                "command_gripper",
                "timer",
                "wait_for_signal_bool",
                "set_key_bool_value",
            ]
        )


class TestAdapterCapabilityKeys:
    def test_adapter_capability_keys_parsed(self):
        """Load fixture, verify capability_keys are on each adapter."""
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        
        # Planning adapter
        assert deploy.adapters["planning"].supported_profiles == ["v0.0"]
        assert deploy.adapters["planning"].capability_keys == [
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.motion.cartesian_path",
            "planning.profile.first_feasible",
            "planning.profile.balanced",
            "planning.profile.quality_bounded",
            "planning.trajectory.time_parameterized",
        ]
        
        # Context adapter
        assert deploy.adapters["context"].supported_profiles == ["v0.0"]
        assert deploy.adapters["context"].capability_keys == [
            "context.state.current",
            "context.validation.trajectory",
            "context.collision.check",
            "context.collision.check.detailed",
            "execution.contact.geometric",
            "execution.contact.geometric_signed_penetration",
            "context.kinematics.fk",
            "context.limits.joint",
            "context.pose.resolve_reference",
            "context.scene.object_query",
            "scene.shape.box",
            "scene.shape.sphere",
            "scene.shape.cylinder",
            "scene.shape.mesh",
            "scene.edit.add",
            "scene.edit.upsert",
            "scene.edit.remove",
            "scene.edit.update_pose",
            "scene.edit.attach_detach",
            "observability.trace.scene",
        ]
        
        # Execution adapter
        assert deploy.adapters["execution"].supported_profiles == ["v0.0"]
        assert deploy.adapters["execution"].capability_keys == [
            "execution.trajectory.execute",
            "execution.feedback.phase_events",
            "execution.feedback.state_snapshots",
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]

    def test_known_adapter_capability_manifest_must_match(self):
        """Known adapter declarations must match code-owned manifests exactly."""
        data = yaml.safe_load(DEPLOYMENT_FIXTURE_PATH.read_text())
        data["adapters"]["context"]["capability_keys"].remove(
            "context.collision.check.detailed"
        )
        path = _write_deployment(data)
        try:
            with pytest.raises(
                DeploymentValidationError,
                match="published capability manifest",
            ):
                Deployment(path)
        finally:
            os.unlink(path)

    def test_all_roboplan_context_declarations_match_published_manifest(self):
        repo_root = Path(__file__).resolve().parents[2]
        deployment_paths = sorted(
            [*(repo_root / "examples").glob("*.deployment.yaml")]
            + [
                *(repo_root / "manyforge_behavior" / "tests" / "fixtures").glob(
                    "*.deployment.yaml"
                )
            ]
            + [
                *(repo_root / "verification" / "deployments").glob(
                    "*.deployment.yaml"
                )
            ]
        )
        manifest = get_adapter_capability_manifest("planning.roboplan_context")
        assert manifest is not None
        matched_paths: list[Path] = []

        for path in deployment_paths:
            raw = yaml.safe_load(path.read_text(encoding="utf-8"))
            for adapter in (raw.get("adapters") or {}).values():
                if adapter.get("adapter_id") != "planning.roboplan_context":
                    continue
                matched_paths.append(path)
                declared_keys = adapter.get("capability_keys") or []
                assert len(declared_keys) == len(set(declared_keys)), path
                assert len(declared_keys) == len(manifest.capability_keys), path
                assert set(declared_keys) == set(manifest.capability_keys), path

        assert len(matched_paths) == 14

    def test_contact_policies_publish_baseline_collision_policy_snapshot(self):
        data = _minimal_deployment(
            contact_policies=[
                {
                    "policy_id": "deployment_baseline",
                    "require_full_diagnostics": False,
                    "notes": "fixture policy",
                }
            ]
        )
        deploy = Deployment.from_dict(data)

        snapshot = deploy.baseline_collision_policy_snapshot()
        assert snapshot is not None
        assert snapshot["policy_id"] == "deployment_baseline"
        assert snapshot["revision_id"] == 1
        assert len(snapshot["content_hash"]) == 64
        assert snapshot["require_full_diagnostics"] is False

        reloaded = Deployment.from_dict(deploy.to_dict())
        assert reloaded.baseline_collision_policy_snapshot() == snapshot

    def test_contact_grasp_policy_exports_typed_collision_snapshot_entry(self):
        data = _minimal_deployment(
            contact_policies=[
                {
                    "policy_id": "contact_grasp/part",
                    "mode": "contact_grasp",
                    "sensing": "geometry_only",
                    "selected_object_id": "part",
                    "allowed_robot_links": ["left_finger", "right_finger"],
                    "max_penetration_m": 0.002,
                    "terminal_window_start_fraction": 0.75,
                }
            ]
        )
        deploy = Deployment.from_dict(data)

        snapshot = deploy.baseline_collision_policy_snapshot()

        assert snapshot is not None
        assert snapshot["policy_id"] == "contact_grasp/part"
        assert snapshot["contact_grasps"] == [
            {
                "policy_id": "contact_grasp/part",
                "selected_object_id": "part",
                "allowed_robot_links": ["left_finger", "right_finger"],
                "max_penetration_m": 0.002,
                "terminal_window_start_fraction": 0.75,
            }
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
            "execution.feedback.state_snapshots",
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
            "planning.profile.first_feasible",
            "planning.profile.balanced",
            "planning.profile.quality_bounded",
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
            "execution.feedback.state_snapshots",
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

    def test_validate_capabilities_requires_declared_keys(self):
        """No capability_keys on any adapter fails closed."""
        data = _minimal_deployment()
        path = _write_deployment(data)
        deploy = Deployment(path)
        with pytest.raises(DeploymentValidationError, match="Missing capability keys"):
            deploy.validate_capabilities()
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
            "planning.profile.first_feasible",
            "planning.profile.balanced",
            "planning.profile.quality_bounded",
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
            "execution.feedback.state_snapshots",
            "execution.stop.soft",
            "execution.pause",
            "execution.resume",
            "execution.reset",
        ]
        deploy_path = _write_deployment(data)
        program_path = _write_deployment(
            _minimal_program(
                program_profile="v0.1",
                tree={
                    "id": "sequence",
                    "name": "root",
                    "children": [
                        {
                            "id": "command_gripper",
                            "name": "close_gripper",
                            "params": {
                                "gripper_id": "primary",
                                "position": 0.7,
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

    @pytest.mark.parametrize(
        ("contact_capabilities", "missing_capability"),
        [
            (
                {"execution.contact.geometric"},
                "execution.contact.geometric_signed_penetration",
            ),
            (
                {"execution.contact.geometric_signed_penetration"},
                "execution.contact.geometric",
            ),
        ],
    )
    def test_contact_grasp_requires_both_geometric_capability_tiers(
        self,
        contact_capabilities,
        missing_capability,
    ):
        data = _minimal_deployment(
            contact_policies=[
                {
                    "policy_id": "contact_grasp/part",
                    "mode": "contact_grasp",
                    "sensing": "geometry_only",
                    "selected_object_id": "part",
                    "allowed_robot_links": ["left_finger", "right_finger"],
                    "max_penetration_m": 0.002,
                }
            ]
        )
        data["adapters"]["execution"]["capability_keys"] = sorted(
            get_required_keys("v0.0") | contact_capabilities
        )
        deployment = Deployment.from_dict(data)

        with pytest.raises(DeploymentValidationError, match=missing_capability):
            deployment.validate_capabilities()

    def test_contact_grasp_admits_with_both_geometric_capability_tiers(self):
        data = _minimal_deployment(
            contact_policies=[
                {
                    "policy_id": "contact_grasp/part",
                    "mode": "contact_grasp",
                    "sensing": "geometry_only",
                    "selected_object_id": "part",
                    "allowed_robot_links": ["left_finger", "right_finger"],
                    "max_penetration_m": 0.002,
                }
            ]
        )
        data["adapters"]["execution"]["capability_keys"] = sorted(
            get_required_keys("v0.0")
            | {
                "execution.contact.geometric",
                "execution.contact.geometric_signed_penetration",
            }
        )
        deployment = Deployment.from_dict(data)

        deployment.validate_capabilities()

    def test_program_binary_contact_admits_with_geometric_tier_only(self, catalog):
        program = Program.from_dict(
            _minimal_program(
                scene=_support_contact_scene(
                    release_evidence_mode="binary_contact"
                )
            ),
            catalog,
        )
        data = _minimal_deployment()
        data["adapters"]["execution"]["capability_keys"] = sorted(
            get_required_keys("v0.0") | {"execution.contact.geometric"}
        )
        deployment = Deployment.from_dict(data)

        deployment.validate_capabilities(program)

    def test_program_signed_contact_rejects_geometric_tier_only(self, catalog):
        program = Program.from_dict(
            _minimal_program(
                scene=_support_contact_scene(
                    release_evidence_mode="signed_penetration"
                )
            ),
            catalog,
        )
        data = _minimal_deployment()
        data["adapters"]["execution"]["capability_keys"] = sorted(
            get_required_keys("v0.0") | {"execution.contact.geometric"}
        )
        deployment = Deployment.from_dict(data)

        with pytest.raises(
            DeploymentValidationError,
            match="execution.contact.geometric_signed_penetration",
        ):
            deployment.validate_capabilities(program)

    def test_legacy_baseline_contact_policy_does_not_claim_contact_grasp_tier(self):
        data = _minimal_deployment(
            contact_policies=[{"policy_id": "deployment_baseline"}]
        )
        data["adapters"]["execution"]["capability_keys"] = sorted(
            get_required_keys("v0.0")
        )
        deployment = Deployment.from_dict(data)

        deployment.validate_capabilities()
