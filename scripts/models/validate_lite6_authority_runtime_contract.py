#!/usr/bin/env python3
"""Validate owner-neutral static Lite6 ROS/control/gripper artifacts.

This validator intentionally does not read or accept a deployment document.
Its retained CLI is available only through the explicit ``static-artifacts``
mode; the historical command shape is a typed unavailable boundary.
The historical version mixed reusable static artifact checks with the removed
``manyforge.deployment.v0`` ``robot_config``/``adapters`` schema and the old
Python RobotModelAuthority owner.  Those portions return through I3/I6 and the
I9/I, I10/R, and I11/T product gates; no replacement catalog, identifier, or
runtime contract is invented here.  JSON output is an unversioned diagnostic,
not durable qualification evidence.

Retained checks are limited to facts present in the supplied artifacts:

* the topic-based ros2_control plugin and six position-command joints;
* the Lite-gripper command/mimic relationship in the URDF;
* controller joint/interface and fail-closed cancel-deceleration settings; and
* the static Isaac gripper bridge joint, topic, effort, and dwell settings.

Exact historical source: ManyForge
``08222188293df8e12f90dc72e204c21f22f92ea1``, blob
``b228171c782c0b9a81c9d3683ab5edf6fbd5fd92``.  Historical lines 107-236
and 289-367 are assigned to I3/I6, I9/I, I10/R, and I11/T; the static
ros2_control and bridge assertions are retained below.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Any, Final, NoReturn, Sequence
import xml.etree.ElementTree as ET

import yaml


REPO_ROOT = Path(__file__).resolve().parents[2]
ARM_JOINTS: Final = tuple(f"joint{index}" for index in range(1, 7))
GRIPPER_COMMAND_JOINT: Final = "right_finger_joint"
GRIPPER_DERIVED_JOINT: Final = "left_finger_joint"
HISTORICAL_SOURCE_COMMIT: Final = (
    "08222188293df8e12f90dc72e204c21f22f92ea1"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "b228171c782c0b9a81c9d3683ab5edf6fbd5fd92"
)
LEGACY_DEPLOYMENT_AUTHORITY_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_lite6_deployment_authority_unavailable"
)
_MAX_ARTIFACT_BYTES: Final = 4 * 1024 * 1024
STATIC_ARTIFACT_MODE: Final = "static-artifacts"


class ContractFailure(RuntimeError):
    """A supplied static artifact violates the retained Lite6 facts."""


class LegacyDeploymentAuthorityUnavailableError(RuntimeError):
    """Raised before inspecting the removed deployment/authority contract."""


def validate_legacy_deployment_authority(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Reject the old authority validator without inspecting caller input."""

    raise LegacyDeploymentAuthorityUnavailableError(
        f"{LEGACY_DEPLOYMENT_AUTHORITY_UNAVAILABLE_REASON_CODE}: "
        "compiler-owned model products and resource addresses return at "
        "I3/I6; real-Lite6 product validation returns at I10/R"
    )


def _bounded_text(path: Path) -> str:
    try:
        with path.open("rb") as stream:
            encoded = stream.read(_MAX_ARTIFACT_BYTES + 1)
    except OSError as exc:
        raise ContractFailure(f"failed to read artifact {path}: {exc}") from exc
    if len(encoded) > _MAX_ARTIFACT_BYTES:
        raise ContractFailure(f"artifact exceeds {_MAX_ARTIFACT_BYTES} bytes: {path}")
    try:
        return encoded.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ContractFailure(f"artifact must be UTF-8: {path}") from exc


def _load_yaml(path: Path) -> dict[str, Any]:
    try:
        loaded = yaml.safe_load(_bounded_text(path))
    except yaml.YAMLError as exc:
        raise ContractFailure(f"failed to parse YAML {path}: {exc}") from exc
    if not isinstance(loaded, dict):
        raise ContractFailure(f"{path} must contain a mapping")
    return loaded


def _load_xml(path: Path) -> ET.Element:
    try:
        return ET.fromstring(_bounded_text(path))
    except ET.ParseError as exc:
        raise ContractFailure(f"failed to parse XML {path}: {exc}") from exc


def _mapping(value: object, *, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ContractFailure(f"{label} must be a mapping")
    return value


def _assert_equal(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise ContractFailure(f"{label}: expected {expected!r}, got {actual!r}")


def _assert_true(value: object, label: str) -> None:
    if value is not True:
        raise ContractFailure(label)


def _finite_number(value: object, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float, str)):
        raise ContractFailure(f"{label}: expected a finite number")
    try:
        result = float(value)
    except ValueError as exc:
        raise ContractFailure(f"{label}: expected a finite number") from exc
    if not math.isfinite(result):
        raise ContractFailure(f"{label}: expected a finite number")
    return result


def _required_text(value: object, *, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value) > 512
        or "\x00" in value
        or "\n" in value
    ):
        raise ContractFailure(f"{label}: expected a bounded non-empty string")
    return value


def _ros2_control_joint_contract(
    root: ET.Element,
    *,
    path: Path,
) -> dict[str, dict[str, list[str]]]:
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
        if not name or name in joints:
            raise ContractFailure(
                "ros2_control joint names must be unique and non-empty"
            )
        joints[name] = {
            "command_interfaces": [
                child.attrib.get("name", "")
                for child in joint.findall("command_interface")
            ],
            "state_interfaces": [
                child.attrib.get("name", "")
                for child in joint.findall("state_interface")
            ],
        }
    return joints


def _validate_gripper_urdf(root: ET.Element) -> None:
    joints = {
        joint.attrib.get("name", ""): joint
        for joint in root.findall("joint")
        if joint.attrib.get("name")
    }
    if GRIPPER_COMMAND_JOINT not in joints or GRIPPER_DERIVED_JOINT not in joints:
        raise ContractFailure("Lite-gripper command and derived joints are required")
    mimic = joints[GRIPPER_DERIVED_JOINT].find("mimic")
    if mimic is None:
        raise ContractFailure("Lite-gripper derived joint must contain <mimic>")
    _assert_equal(
        mimic.attrib.get("joint"),
        GRIPPER_COMMAND_JOINT,
        "Lite-gripper mimic source",
    )
    _assert_equal(
        _finite_number(mimic.attrib.get("multiplier"), label="mimic multiplier"),
        1.0,
        "Lite-gripper mimic multiplier",
    )
    _assert_equal(
        _finite_number(mimic.attrib.get("offset"), label="mimic offset"),
        0.0,
        "Lite-gripper mimic offset",
    )


def validate_static_artifacts(
    *,
    ros2_control_urdf: Path,
    controllers: Path,
    gripper_bridge: Path,
) -> dict[str, object]:
    """Validate static files without deployment, ROS, process, or network access."""

    urdf_root = _load_xml(ros2_control_urdf)
    ros2_joints = _ros2_control_joint_contract(
        urdf_root,
        path=ros2_control_urdf,
    )
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
    _validate_gripper_urdf(urdf_root)

    controller_document = _load_yaml(controllers)
    arm_controller = _mapping(
        controller_document.get(
            "isaac_arm_controller",
            controller_document.get("/**/isaac_arm_controller"),
        ),
        label="isaac_arm_controller",
    )
    arm_params = _mapping(
        arm_controller.get("ros__parameters"),
        label="isaac_arm_controller.ros__parameters",
    )
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
    constraints = _mapping(
        arm_params.get("constraints"),
        label="JTC constraints",
    )
    _assert_true(
        constraints.get("decelerate_on_cancel"),
        "JTC decelerate_on_cancel must be true",
    )
    for joint_name in ARM_JOINTS:
        joint_constraints = _mapping(
            constraints.get(joint_name),
            label=f"JTC constraints.{joint_name}",
        )
        deceleration = _finite_number(
            joint_constraints.get("max_deceleration_on_cancel"),
            label=f"{joint_name} max_deceleration_on_cancel",
        )
        if deceleration <= 0.0:
            raise ContractFailure(
                f"{joint_name} max_deceleration_on_cancel must be positive"
            )

    bridge_document = _load_yaml(gripper_bridge)
    bridge_node = _mapping(
        bridge_document.get("manyforge_isaac_gripper_action_bridge"),
        label="manyforge_isaac_gripper_action_bridge",
    )
    bridge_params = _mapping(
        bridge_node.get("ros__parameters"),
        label="gripper bridge ros__parameters",
    )
    _required_text(bridge_params.get("action_name"), label="gripper action_name")
    _assert_equal(
        bridge_params.get("command_topic"),
        "/isaac_joint_commands_gripper",
        "gripper command_topic",
    )
    _assert_equal(
        bridge_params.get("state_topic"),
        "/isaac_joint_states",
        "gripper state_topic",
    )
    _assert_equal(
        bridge_params.get("commanded_joint"),
        GRIPPER_COMMAND_JOINT,
        "gripper commanded_joint",
    )
    _assert_equal(
        bridge_params.get("command_joint_names"),
        [GRIPPER_COMMAND_JOINT],
        "gripper command_joint_names",
    )
    _assert_equal(
        bridge_params.get("completion_joint_names"),
        [GRIPPER_COMMAND_JOINT],
        "gripper completion_joint_names",
    )
    _assert_equal(
        bridge_params.get("actuation_kind"),
        "pneumatic",
        "gripper actuation_kind",
    )
    _assert_equal(
        bridge_params.get("completion_policy"),
        "dwell_elapsed",
        "gripper completion_policy",
    )
    close_effort = _finite_number(
        bridge_params.get("close_effort"),
        label="gripper close_effort",
    )
    open_effort = _finite_number(
        bridge_params.get("open_effort"),
        label="gripper open_effort",
    )
    dwell_s = _finite_number(
        bridge_params.get("dwell_s"),
        label="gripper dwell_s",
    )
    if close_effort >= 0.0 or open_effort <= 0.0 or dwell_s <= 0.0:
        raise ContractFailure(
            "gripper efforts must open positive/close negative and dwell "
            "must be positive"
        )
    _assert_true(
        bridge_params.get("publish_nominal_position"),
        "gripper bridge must publish nominal position",
    )
    _assert_true(
        bridge_params.get("require_all_state_joints"),
        "gripper bridge must require all state joints",
    )

    return {
        "arm_joints": list(ARM_JOINTS),
        "gripper_command_joint": GRIPPER_COMMAND_JOINT,
        "gripper_derived_joint": GRIPPER_DERIVED_JOINT,
        "ros2_control_urdf": str(ros2_control_urdf),
        "controller_config": str(controllers),
        "gripper_bridge_config": str(gripper_bridge),
    }


def main(argv: Sequence[str] | None = None) -> int:
    raw_argv = tuple(sys.argv[1:] if argv is None else argv)
    if not raw_argv or raw_argv[0] != STATIC_ARTIFACT_MODE:
        try:
            validate_legacy_deployment_authority()
        except LegacyDeploymentAuthorityUnavailableError as exc:
            print(str(exc), file=sys.stderr)
            return 78
        raise AssertionError("legacy authority gate unexpectedly returned")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ros2-control-urdf",
        type=Path,
        required=True,
        help="Host-generated Lite6 robot description to inspect (no stored fallback).",
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
    args = parser.parse_args(raw_argv[1:])

    try:
        report = validate_static_artifacts(
            ros2_control_urdf=args.ros2_control_urdf,
            controllers=args.controllers,
            gripper_bridge=args.gripper_bridge,
        )
    except ContractFailure as exc:
        print(f"Lite6 static artifact validation failed: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print("Lite6 static ROS/control/gripper artifacts passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
