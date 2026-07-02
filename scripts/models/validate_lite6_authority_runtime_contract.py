#!/usr/bin/env python3
"""Validate Lite6 runtime scaffolds against Robot Model Authority metadata.

This is a non-live RMA-7 gate. It does not contact ROS, Isaac, or Composer; it
checks that the static Isaac/ros2_control lane files agree with the canonical
Lite6 authority contract.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))
if str(REPO_ROOT / "manyforge_behavior") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "manyforge_behavior"))

from manyforge_behavior.robot_model_authority import (  # noqa: E402
    RobotModelGripperConfig,
    load_robot_model_authority,
)

ARM_GROUP = "lite6"
GRIPPER_ID = "primary"
GRIPPER_GROUP = "lite_gripper"
ARM_JOINTS = ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6")
GRIPPER_COMMAND_JOINT = "right_finger_joint"
GRIPPER_DERIVED_JOINT = "left_finger_joint"


class ContractFailure(RuntimeError):
    pass


def _load_yaml(path: Path) -> dict[str, Any]:
    try:
        loaded = yaml.safe_load(path.read_text(encoding="utf-8"))
    except Exception as exc:  # pragma: no cover - reported through CLI.
        raise ContractFailure(f"failed to read YAML {path}: {exc}") from exc
    if not isinstance(loaded, dict):
        raise ContractFailure(f"{path} must contain a mapping")
    return loaded


def _assert_equal(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise ContractFailure(f"{label}: expected {expected!r}, got {actual!r}")


def _assert_true(value: Any, label: str) -> None:
    if not value:
        raise ContractFailure(label)


def _assert_finite_number(value: Any, label: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise ContractFailure(f"{label}: expected finite number, got {value!r}") from exc
    if not math.isfinite(result):
        raise ContractFailure(f"{label}: expected finite number, got {value!r}")
    return result


def _canonical_path_from_package_uri(uri: str) -> Path:
    prefix = "package://manyforge_example_models/"
    if not uri.startswith(prefix):
        raise ContractFailure(f"expected manyforge_example_models package URI: {uri}")
    return REPO_ROOT / "manyforge_example_models" / uri.removeprefix(prefix)


def _ros2_control_joint_contract(path: Path) -> dict[str, Any]:
    root = ET.parse(path).getroot()
    ros2_control = root.find("ros2_control")
    if ros2_control is None:
        raise ContractFailure(f"{path} is missing <ros2_control>")
    plugin = ros2_control.find("./hardware/plugin")
    _assert_equal(
        plugin.text.strip() if plugin is not None and plugin.text else "",
        "topic_based_ros2_control/TopicBasedSystem",
        "ros2_control hardware plugin",
    )
    joints: dict[str, dict[str, list[str]]] = {}
    for joint in ros2_control.findall("joint"):
        name = joint.attrib.get("name", "")
        commands = [
            child.attrib.get("name", "")
            for child in joint.findall("command_interface")
        ]
        states = [
            child.attrib.get("name", "")
            for child in joint.findall("state_interface")
        ]
        joints[name] = {"command_interfaces": commands, "state_interfaces": states}
    return joints


def validate(args: argparse.Namespace) -> dict[str, Any]:
    deployment = _load_yaml(args.deployment)
    robot_config = deployment.get("robot_config")
    if not isinstance(robot_config, dict):
        raise ContractFailure("deployment.robot_config must be a mapping")
    grippers = deployment.get("grippers")
    if not isinstance(grippers, dict) or GRIPPER_ID not in grippers:
        raise ContractFailure(f"deployment.grippers.{GRIPPER_ID} is required")
    gripper_config = grippers[GRIPPER_ID]
    if not isinstance(gripper_config, dict):
        raise ContractFailure(f"deployment.grippers.{GRIPPER_ID} must be a mapping")

    urdf_path = _canonical_path_from_package_uri(str(robot_config.get("urdf_path", "")))
    srdf_path = _canonical_path_from_package_uri(str(robot_config.get("srdf_path", "")))
    authority = load_robot_model_authority(
        model_name="lite6_lite_gripper",
        urdf_path=urdf_path,
        srdf_path=srdf_path,
        package_paths=(str(REPO_ROOT),),
        grippers={
            GRIPPER_GROUP: RobotModelGripperConfig(
                group_id=GRIPPER_GROUP,
                joint_name=GRIPPER_COMMAND_JOINT,
                touch_links=tuple(gripper_config.get("touch_links") or ()),
                mount_frame=str(gripper_config.get("mount_frame") or ""),
                attach_frame=str(gripper_config.get("attach_frame") or ""),
                tcp_frame=str(gripper_config.get("tcp_frame") or ""),
                auxiliary_frames=tuple(gripper_config.get("auxiliary_frames") or ()),
            )
        },
    )

    _assert_equal(robot_config.get("robot_id"), "lite6", "deployment robot_id")
    _assert_equal(robot_config.get("group_id"), ARM_GROUP, "deployment group_id")
    _assert_equal(
        authority.group_commandable_joints(ARM_GROUP),
        ARM_JOINTS,
        "authority arm commandable joints",
    )
    _assert_equal(
        authority.group_commandable_joints(GRIPPER_GROUP),
        (GRIPPER_COMMAND_JOINT,),
        "authority gripper commandable joints",
    )
    _assert_equal(
        authority.grippers[GRIPPER_GROUP].derived_joints,
        (GRIPPER_DERIVED_JOINT,),
        "authority gripper derived joints",
    )
    _assert_equal(
        gripper_config.get("joint_name"),
        GRIPPER_COMMAND_JOINT,
        "deployment gripper command joint",
    )
    _assert_equal(
        gripper_config.get("actuation_kind"),
        "pneumatic",
        "deployment gripper actuation_kind",
    )
    _assert_equal(
        gripper_config.get("state_authority"),
        "sim_observed",
        "deployment gripper state_authority",
    )
    _assert_equal(
        gripper_config.get("completion_policy"),
        "dwell_elapsed",
        "deployment gripper completion_policy",
    )
    _assert_equal(
        gripper_config.get("open_state_ref"),
        "open",
        "deployment gripper open_state_ref",
    )
    _assert_equal(
        gripper_config.get("closed_state_ref"),
        "closed",
        "deployment gripper closed_state_ref",
    )
    transport = gripper_config.get("transport")
    if not isinstance(transport, dict):
        raise ContractFailure("deployment gripper transport must be a mapping")
    gripper_transport_kind = str(transport.get("kind") or "")
    if gripper_transport_kind == "isaac_joint_state_effort":
        _assert_equal(
            transport.get("command_topic"),
            "/isaac_joint_commands_gripper",
            "Lite6 direct gripper command_topic",
        )
        _assert_equal(
            transport.get("state_topic"),
            "/isaac_joint_states",
            "Lite6 direct gripper state_topic",
        )
        _assert_equal(
            transport.get("joint_names"),
            [GRIPPER_COMMAND_JOINT],
            "Lite6 direct gripper joint_names",
        )
        close_effort = _assert_finite_number(
            transport.get("close_effort"), "Lite6 direct close_effort"
        )
        open_effort = _assert_finite_number(
            transport.get("open_effort"), "Lite6 direct open_effort"
        )
        dwell_s = _assert_finite_number(
            transport.get("dwell_s"), "Lite6 direct dwell_s"
        )
        _assert_true(
            close_effort < 0.0,
            "Lite6 direct close_effort must close with negative effort",
        )
        _assert_true(
            open_effort > 0.0,
            "Lite6 direct open_effort must open with positive effort",
        )
        _assert_true(
            dwell_s > 0.0,
            "Lite6 direct dwell_s must be positive",
        )
        _assert_true(
            transport.get("publish_nominal_position") is True,
            "Lite6 direct transport must publish nominal position for Isaac",
        )
    elif gripper_transport_kind != "ros_gripper_command":
        raise ContractFailure(
            "deployment gripper transport.kind must be isaac_joint_state_effort "
            f"or ros_gripper_command, got {gripper_transport_kind!r}"
        )

    ros2_joints = _ros2_control_joint_contract(args.ros2_control_urdf)
    _assert_equal(tuple(ros2_joints), ARM_JOINTS, "ros2_control URDF joint list")
    for joint_name, interfaces in ros2_joints.items():
        _assert_equal(
            interfaces["command_interfaces"],
            ["position"],
            f"ros2_control {joint_name} command interfaces",
        )
        _assert_equal(
            interfaces["state_interfaces"],
            ["position", "velocity"],
            f"ros2_control {joint_name} state interfaces",
        )

    controllers = _load_yaml(args.controllers)
    arm_controller = controllers.get("isaac_arm_controller")
    if not isinstance(arm_controller, dict):
        raise ContractFailure("isaac_arm_controller config is required")
    arm_params = arm_controller.get("ros__parameters")
    if not isinstance(arm_params, dict):
        raise ContractFailure("isaac_arm_controller.ros__parameters is required")
    _assert_equal(
        tuple(arm_params.get("joints") or ()),
        ARM_JOINTS,
        "JTC controller joints",
    )
    _assert_equal(
        arm_params.get("command_interfaces"),
        ["position"],
        "JTC command interfaces",
    )
    _assert_equal(
        arm_params.get("state_interfaces"),
        ["position", "velocity"],
        "JTC state interfaces",
    )
    constraints = arm_params.get("constraints")
    if not isinstance(constraints, dict):
        raise ContractFailure("JTC constraints mapping is required")
    _assert_true(
        constraints.get("decelerate_on_cancel") is True,
        "JTC decelerate_on_cancel must be true for hard_deceleration evidence",
    )
    for joint_name in ARM_JOINTS:
        joint_constraints = constraints.get(joint_name)
        if not isinstance(joint_constraints, dict):
            raise ContractFailure(f"missing JTC constraints for {joint_name}")
        _assert_true(
            float(joint_constraints.get("max_deceleration_on_cancel", 0.0)) > 0.0,
            f"{joint_name} max_deceleration_on_cancel must be positive",
        )

    execution = (
        deployment.get("adapters", {})
        .get("execution", {})
        .get("config", {})
    )
    if not isinstance(execution, dict):
        raise ContractFailure("deployment execution config must be a mapping")
    _assert_equal(
        tuple(execution.get("required_joints") or ()),
        ARM_JOINTS,
        "deployment execution required_joints",
    )
    _assert_equal(
        tuple(execution.get("velocity_state_interface_joints") or ()),
        ARM_JOINTS,
        "deployment velocity_state_interface_joints",
    )
    decel_limits = execution.get("max_deceleration_on_cancel_rad_s2")
    if not isinstance(decel_limits, dict):
        raise ContractFailure("deployment max_deceleration_on_cancel_rad_s2 required")
    _assert_equal(
        tuple(decel_limits.keys()),
        ARM_JOINTS,
        "deployment cancel-deceleration joint keys",
    )
    _assert_true(
        execution.get("decelerate_on_cancel") is True,
        "deployment decelerate_on_cancel must be true",
    )
    if gripper_transport_kind == "ros_gripper_command":
        exec_gripper = execution.get("gripper")
        if not isinstance(exec_gripper, dict):
            raise ContractFailure("deployment execution gripper config required")
        bridge = _load_yaml(args.gripper_bridge)
        bridge_node = bridge.get("manyforge_isaac_gripper_action_bridge")
        if not isinstance(bridge_node, dict):
            raise ContractFailure("manyforge_isaac_gripper_action_bridge is required")
        bridge_params = bridge_node.get("ros__parameters")
        if not isinstance(bridge_params, dict):
            raise ContractFailure("gripper bridge ros__parameters are required")
        _assert_equal(
            bridge_params.get("commanded_joint"),
            GRIPPER_COMMAND_JOINT,
            "gripper bridge commanded_joint",
        )
        _assert_equal(
            bridge_params.get("joint_names"),
            [GRIPPER_COMMAND_JOINT],
            "gripper bridge joint_names",
        )
        _assert_true(
            bridge_params.get("require_all_state_joints") is True,
            "gripper bridge must require all state joints",
        )
        _assert_equal(
            exec_gripper.get("action_name"),
            bridge_params.get("action_name"),
            "deployment gripper action must match bridge action",
        )

    return {
        "schema": "manyforge.lite6_authority_runtime_contract.v1",
        "canonical_fingerprint": authority.canonical_fingerprint,
        "robot_id": "lite6",
        "arm_group": ARM_GROUP,
        "arm_joints": list(ARM_JOINTS),
        "gripper_group": GRIPPER_GROUP,
        "gripper_command_joint": GRIPPER_COMMAND_JOINT,
        "gripper_derived_joints": [GRIPPER_DERIVED_JOINT],
        "ros2_control_urdf": str(args.ros2_control_urdf),
        "controller_config": str(args.controllers),
        "gripper_transport_kind": gripper_transport_kind,
        "gripper_bridge_config": (
            str(args.gripper_bridge)
            if gripper_transport_kind == "ros_gripper_command"
            else None
        ),
        "deployment": str(args.deployment),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--deployment",
        type=Path,
        default=REPO_ROOT
        / "examples"
        / "isaac_lite6_lite_gripper_pick_and_place.deployment.yaml",
    )
    parser.add_argument(
        "--ros2-control-urdf",
        type=Path,
        default=(
            REPO_ROOT
            / "manyforge_ros_bringup"
            / "urdf"
            / "isaac_lite6_topic_based.ros2_control.urdf"
        ),
    )
    parser.add_argument(
        "--controllers",
        type=Path,
        default=(
            REPO_ROOT
            / "manyforge_ros_bringup"
            / "config"
            / "isaac_lite6_topic_based_controllers.yaml"
        ),
    )
    parser.add_argument(
        "--gripper-bridge",
        type=Path,
        default=(
            REPO_ROOT
            / "manyforge_ros_bringup"
            / "config"
            / "isaac_lite6_gripper_bridge.yaml"
        ),
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        report = validate(args)
    except ContractFailure as exc:
        print(f"Lite6 authority runtime contract failed: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print(
            "Lite6 authority runtime contract passed: "
            f"{report['canonical_fingerprint']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
