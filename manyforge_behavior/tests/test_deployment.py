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
ISAAC_LIGHT_PROGRAM_PATH = (
    Path(__file__).resolve().parents[2]
    / "examples"
    / "isaac_manipulator_scene_light.program.yaml"
)
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
        "catalog_version": "0.2.0",
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
        assert deploy.orchestrator.config == {"collision_monitor_period_ms": 50.0}

    def test_launch_parsed(self):
        deploy = Deployment(DEPLOYMENT_FIXTURE_PATH)
        assert deploy.launch.runner.module == "manyforge_behavior.runner"
        assert deploy.launch.runner.args == []
        assert deploy.launch.ui_backend is not None
        assert deploy.launch.ui_backend.module == "manyforge_hmi.backend"
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
                    "objects": {
                        "isaac_cheese_box": {
                            "entity": "/mac_n_cheese_centered",
                            "apply_policy": "before_planning",
                            "confidence_min": 0.8,
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
        assert set(source.objects) == {"isaac_cheese_box", "isaac_soup_can"}
        assert source.objects["isaac_cheese_box"].entity == "/mac_n_cheese_centered"
        assert source.objects["isaac_cheese_box"].apply_policy == "before_planning"
        assert source.objects["isaac_cheese_box"].confidence_min == 0.8
        assert source.objects["isaac_cheese_box"].metadata == {"source": "fixture"}

        serialized = deploy.to_dict()
        reloaded = Deployment.from_dict(serialized)
        assert reloaded.pose_sources["isaac_scene_objects"].objects[
            "isaac_soup_can"
        ].apply_policy == "observe_only"

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
            "catalog_version": "0.2.0",
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
            "catalog_version": "0.2.0",
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
            "catalog_version": "0.2.0",
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
        assert deploy.adapters["execution"].config["gripper"]["action_name"] == (
            "/robotiq_gripper_controller/gripper_cmd"
        )
        deploy.validate_against_program(prog)
        deploy.validate_capabilities()

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
            "execution.feedback.state_snapshots",
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
