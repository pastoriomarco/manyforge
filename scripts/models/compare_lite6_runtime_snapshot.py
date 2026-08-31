#!/usr/bin/env python3
"""Compare ManyMove's runtime Lite6 descriptions with ManyForge's snapshot.

This is a qualification gate, not a generator. It proves that the vendored
ManyForge canonical Lite6 model preserves the arm/gripper contract from a
captured ManyMove runtime ``robot_description`` / ``robot_description_semantic``.
Only vendoring-level deltas are allowed:

* mesh URI rewrite to ``package://manyforge_example_models/...``;
* model dynamics normalized from the pinned MoveIt ``joint_limits.yaml``;
* the additional ``closed`` SRDF group-state alias for ManyForge UI language.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

import yaml


ARM_JOINTS = ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6")
FINGER_JOINTS = ("right_finger_joint", "left_finger_joint")
SNAPSHOT_PREFIX = "package://manyforge_example_models/models/lite6_lite_gripper/"
XARM_PREFIX = "package://xarm_description/"
CAPTURE_SCHEMA = "manyforge.lite6_runtime_capture.v1"
EXPECTED_MANYMOVE_IMAGE = "manymove-xarm:jazzy"
EXPECTED_XARM_REPO = "https://github.com/pastoriomarco/xarm_ros2"
EXPECTED_XARM_BRANCH = "jazzy_no_gazebo"
EXPECTED_XARM_COMMIT = "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
EXPECTED_RUNTIME_OWNER_NODE = "/move_group"
MOVEIT_JOINT_LIMITS_RELATIVE_PATH = Path(
    "upstream_source/xarm_moveit_config/config/lite6/joint_limits.yaml"
)
EXPECTED_MOVEIT_JOINT_LIMITS_SHA256 = (
    "73368cad488a238ba6c4bc72e679a2ae7e39cef3c7aecb83e50d6037255d927c"
)
EXPECTED_LAUNCH_COMMAND = [
    "ros2",
    "launch",
    "manymove_bringup",
    "lite_foundationpose_movegroup_fake_cpp_trees.launch.py",
    "no_gui_ctrl:=true",
]

EXPECTED_DISABLE_COLLISIONS = {
    ("lite_gripper_left_finger", "lite_gripper_base_link", "Adjacent"),
    ("lite_gripper_right_finger", "lite_gripper_base_link", "Adjacent"),
    ("lite_gripper_left_finger", "lite_gripper_right_finger", "Never"),
    ("lite_gripper_left_finger", "link4", "Never"),
    ("lite_gripper_left_finger", "link5", "Never"),
    ("lite_gripper_left_finger", "link6", "Never"),
    ("lite_gripper_right_finger", "link4", "Never"),
    ("lite_gripper_right_finger", "link5", "Never"),
    ("lite_gripper_right_finger", "link6", "Never"),
    ("link6", "lite_gripper_base_link", "Adjacent"),
    ("link5", "lite_gripper_base_link", "Never"),
    ("link_eef", "lite_gripper_left_finger", "Never"),
    ("link_eef", "lite_gripper_right_finger", "Never"),
    ("link_eef", "lite_gripper_base_link", "Adjacent"),
    ("link1", "link2", "Adjacent"),
    ("link1", "link3", "Never"),
    ("link1", "link_base", "Adjacent"),
    ("link2", "link3", "Adjacent"),
    ("link2", "link4", "Never"),
    ("link2", "link_base", "Never"),
    ("link3", "link4", "Adjacent"),
    ("link3", "link5", "Never"),
    ("link3", "link6", "Never"),
    ("link4", "link5", "Adjacent"),
    ("link4", "link6", "Never"),
    ("link5", "link6", "Adjacent"),
    ("link3", "link_eef", "Never"),
    ("link5", "link_eef", "Never"),
    ("link6", "link_eef", "Adjacent"),
}


@dataclass
class Report:
    failures: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    facts: dict[str, object] = field(default_factory=dict)

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.failures.append(message)

    def warn_if(self, condition: bool, message: str) -> None:
        if condition:
            self.warnings.append(message)


def parse_xml(path: Path) -> ET.Element:
    try:
        return ET.parse(path).getroot()
    except ET.ParseError as exc:
        raise SystemExit(f"failed to parse XML {path}: {exc}") from exc


def child_attr(element: ET.Element, tag: str, attr: str) -> str | None:
    child = element.find(tag)
    if child is None:
        return None
    return child.attrib.get(attr)


def joint(root: ET.Element, name: str) -> ET.Element | None:
    return root.find(f"./joint[@name='{name}']")


def link_names(root: ET.Element) -> set[str]:
    return {link.attrib["name"] for link in root.findall("./link") if "name" in link.attrib}


def mesh_refs(root: ET.Element) -> list[str]:
    return [
        mesh.attrib["filename"]
        for mesh in root.iter("mesh")
        if mesh.attrib.get("filename")
    ]


def joint_signature(root: ET.Element, name: str) -> dict[str, object] | None:
    item = joint(root, name)
    if item is None:
        return None
    origin = item.find("origin")
    axis = item.find("axis")
    limit = item.find("limit")
    return {
        "type": item.attrib.get("type"),
        "parent": child_attr(item, "parent", "link"),
        "child": child_attr(item, "child", "link"),
        "origin": dict(origin.attrib) if origin is not None else {},
        "axis": dict(axis.attrib) if axis is not None else {},
        "limit": {
            key: limit.attrib.get(key)
            for key in ("lower", "upper")
            if limit is not None and key in limit.attrib
        },
    }


def validate_moveit_joint_dynamics(
    snapshot: ET.Element, snapshot_dir: Path, report: Report
) -> None:
    source = snapshot_dir / MOVEIT_JOINT_LIMITS_RELATIVE_PATH
    report.require(source.is_file(), "owned Lite6 MoveIt joint-limit source is missing")
    if not source.is_file():
        return
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    report.require(
        digest == EXPECTED_MOVEIT_JOINT_LIMITS_SHA256,
        "owned Lite6 MoveIt joint-limit source does not match the pinned xArm commit",
    )
    try:
        payload = yaml.safe_load(data)
    except yaml.YAMLError as exc:
        report.failures.append(f"owned Lite6 MoveIt joint limits are invalid YAML: {exc}")
        return
    limits = payload.get("joint_limits") if isinstance(payload, dict) else None
    report.require(
        isinstance(limits, dict) and set(limits) == set(ARM_JOINTS),
        "owned Lite6 MoveIt joint limits must cover exactly joint1..joint6",
    )
    if not isinstance(limits, dict) or set(limits) != set(ARM_JOINTS):
        return

    normalized: dict[str, tuple[float, float]] = {}
    for name in ARM_JOINTS:
        row = limits[name]
        valid = (
            isinstance(row, dict)
            and row.get("has_velocity_limits") is True
            and row.get("has_acceleration_limits") is True
            and isinstance(row.get("max_velocity"), (int, float))
            and not isinstance(row.get("max_velocity"), bool)
            and isinstance(row.get("max_acceleration"), (int, float))
            and not isinstance(row.get("max_acceleration"), bool)
            and float(row["max_velocity"]) > 0.0
            and float(row["max_acceleration"]) > 0.0
        )
        report.require(valid, f"owned MoveIt dynamics for {name} are incomplete")
        if not valid:
            continue
        normalized[name] = (
            float(row["max_velocity"]),
            float(row["max_acceleration"]),
        )
        item = joint(snapshot, name)
        limit = item.find("limit") if item is not None else None
        try:
            velocity = float(limit.attrib["velocity"]) if limit is not None else None
            acceleration = (
                float(limit.attrib["acceleration"]) if limit is not None else None
            )
        except (KeyError, ValueError):
            velocity = None
            acceleration = None
        report.require(
            velocity == normalized[name][0],
            f"snapshot arm joint {name} does not use its MoveIt velocity limit",
        )
        report.require(
            acceleration == normalized[name][1],
            f"snapshot arm joint {name} does not use its MoveIt acceleration limit",
        )
    report.facts["moveit_joint_limits_sha256"] = digest
    report.facts["normalized_arm_joint_dynamics"] = normalized


def mimic_map(root: ET.Element) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in root.findall("./joint"):
        mimic = item.find("mimic")
        if mimic is not None and mimic.attrib.get("joint"):
            out[item.attrib["name"]] = mimic.attrib["joint"]
    return out


def group(root: ET.Element, name: str) -> ET.Element | None:
    return root.find(f"./group[@name='{name}']")


def group_joints(root: ET.Element, name: str) -> set[str]:
    item = group(root, name)
    if item is None:
        return set()
    return {joint.attrib["name"] for joint in item.findall("./joint") if "name" in joint.attrib}


def group_chain(root: ET.Element, name: str) -> tuple[str | None, str | None] | None:
    item = group(root, name)
    if item is None:
        return None
    chain = item.find("./chain")
    if chain is None:
        return None
    return (chain.attrib.get("base_link"), chain.attrib.get("tip_link"))


def group_state_joints(root: ET.Element, group_name: str, state_name: str) -> dict[str, str]:
    state = root.find(f"./group_state[@group='{group_name}'][@name='{state_name}']")
    if state is None:
        return {}
    return {
        joint.attrib["name"]: joint.attrib.get("value", "")
        for joint in state.findall("./joint")
        if "name" in joint.attrib
    }


def disable_collision_set(root: ET.Element) -> set[tuple[str, str, str]]:
    values = set()
    for item in root.findall("./disable_collisions"):
        link1 = item.attrib.get("link1")
        link2 = item.attrib.get("link2")
        reason = item.attrib.get("reason", "")
        if link1 and link2:
            values.add((link1, link2, reason))
    return values


def require_mapping(value: object, path: str, report: Report) -> dict[str, object]:
    if isinstance(value, dict):
        return value
    report.failures.append(f"{path} must be an object")
    return {}


def require_text(value: object, path: str, report: Report) -> str:
    if isinstance(value, str) and value:
        return value
    report.failures.append(f"{path} must be a non-empty string")
    return ""


def require_list(value: object, path: str, report: Report) -> list[object]:
    if isinstance(value, list):
        return value
    report.failures.append(f"{path} must be a list")
    return []


def validate_capture_metadata(
    capture_json: Path,
    runtime_urdf: Path,
    runtime_srdf: Path,
    report: Report,
) -> None:
    root = Path(__file__).resolve().parents[2]

    def resolve_recorded_artifact(path_text: str) -> Path:
        path = Path(path_text)
        if path.is_absolute():
            return path.resolve()
        return (root / path).resolve()

    try:
        capture = json.loads(capture_json.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - validation surface
        report.failures.append(f"failed to read runtime capture metadata: {exc}")
        return

    metadata = require_mapping(capture, "capture", report)
    report.require(
        metadata.get("schema") == CAPTURE_SCHEMA,
        f"runtime capture schema must be {CAPTURE_SCHEMA}",
    )
    report.require(
        require_text(metadata.get("node"), "capture.node", report)
        == EXPECTED_RUNTIME_OWNER_NODE,
        f"runtime capture node must be {EXPECTED_RUNTIME_OWNER_NODE}",
    )
    report.require(
        require_text(
            metadata.get("robot_description_param"),
            "capture.robot_description_param",
            report,
        )
        == "robot_description",
        "runtime capture must read robot_description",
    )
    report.require(
        require_text(
            metadata.get("robot_description_semantic_param"),
            "capture.robot_description_semantic_param",
            report,
        )
        == "robot_description_semantic",
        "runtime capture must read robot_description_semantic",
    )

    manymove = require_mapping(metadata.get("manymove"), "capture.manymove", report)
    report.require(
        require_text(manymove.get("image"), "capture.manymove.image", report)
        == EXPECTED_MANYMOVE_IMAGE,
        f"runtime capture image must be {EXPECTED_MANYMOVE_IMAGE}",
    )
    report.require(
        require_text(
            manymove.get("runtime_owner_node"),
            "capture.manymove.runtime_owner_node",
            report,
        )
        == EXPECTED_RUNTIME_OWNER_NODE,
        f"runtime owner node must be {EXPECTED_RUNTIME_OWNER_NODE}",
    )
    launch_command = require_list(
        manymove.get("launch_command"),
        "capture.manymove.launch_command",
        report,
    )
    report.require(
        launch_command == EXPECTED_LAUNCH_COMMAND,
        "runtime capture launch command does not match the Lite6 FoundationPose scenario",
    )

    xarm = require_mapping(metadata.get("xarm_ros2"), "capture.xarm_ros2", report)
    report.require(
        require_text(xarm.get("repo"), "capture.xarm_ros2.repo", report)
        == EXPECTED_XARM_REPO,
        f"xarm_ros2 repo must be {EXPECTED_XARM_REPO}",
    )
    report.require(
        require_text(xarm.get("branch"), "capture.xarm_ros2.branch", report)
        == EXPECTED_XARM_BRANCH,
        f"xarm_ros2 branch must be {EXPECTED_XARM_BRANCH}",
    )
    report.require(
        require_text(xarm.get("commit"), "capture.xarm_ros2.commit", report)
        == EXPECTED_XARM_COMMIT,
        f"xarm_ros2 commit must be {EXPECTED_XARM_COMMIT}",
    )

    artifacts = require_mapping(metadata.get("artifacts"), "capture.artifacts", report)
    urdf_record = require_text(artifacts.get("urdf"), "capture.artifacts.urdf", report)
    srdf_record = require_text(artifacts.get("srdf"), "capture.artifacts.srdf", report)
    if urdf_record:
        report.require(
            resolve_recorded_artifact(urdf_record) == runtime_urdf.resolve(),
            "runtime capture URDF path does not match --runtime-urdf",
        )
    if srdf_record:
        report.require(
            resolve_recorded_artifact(srdf_record) == runtime_srdf.resolve(),
            "runtime capture SRDF path does not match --runtime-srdf",
        )
    captured_at = require_text(
        metadata.get("captured_at_utc"), "capture.captured_at_utc", report
    )
    if captured_at:
        try:
            dt.datetime.fromisoformat(captured_at)
        except ValueError:
            report.failures.append("capture.captured_at_utc must be ISO-8601")

    report.facts["runtime_capture_schema"] = metadata.get("schema")
    report.facts["runtime_capture_xarm_branch"] = xarm.get("branch")
    report.facts["runtime_capture_xarm_commit"] = xarm.get("commit")


def validate_urdf(runtime: ET.Element, snapshot: ET.Element, snapshot_dir: Path, report: Report) -> None:
    runtime_links = link_names(runtime)
    snapshot_links = link_names(snapshot)
    report.facts["runtime_link_count"] = len(runtime_links)
    report.facts["snapshot_link_count"] = len(snapshot_links)
    report.require(
        snapshot.attrib.get("version") == "1.2",
        "canonical snapshot must declare URDF version 1.2 for acceleration limits",
    )

    report.require("base_link" not in snapshot_links, "canonical snapshot must not add base_link alias")
    report.require(
        joint(snapshot, "base_link_to_link_base") is None,
        "canonical snapshot must not add base_link_to_link_base",
    )

    for name in (*ARM_JOINTS, *FINGER_JOINTS):
        report.require(joint(runtime, name) is not None, f"runtime URDF missing joint {name}")
        report.require(joint(snapshot, name) is not None, f"snapshot URDF missing joint {name}")

    for name in ARM_JOINTS:
        runtime_sig = joint_signature(runtime, name)
        snapshot_sig = joint_signature(snapshot, name)
        report.require(
            runtime_sig == snapshot_sig,
            f"snapshot arm joint {name} does not match runtime signature",
        )

    validate_moveit_joint_dynamics(snapshot, snapshot_dir, report)

    runtime_mimic = mimic_map(runtime)
    snapshot_mimic = mimic_map(snapshot)
    report.require(
        any(
            source in FINGER_JOINTS and target in FINGER_JOINTS
            for source, target in runtime_mimic.items()
        ),
        "runtime URDF does not expose Lite6 sibling-finger mimic",
    )
    report.require(
        snapshot_mimic == runtime_mimic,
        "canonical snapshot must preserve runtime mimic declarations",
    )

    for tag in ("gazebo", "ros2_control", "transmission"):
        report.require(
            len(snapshot.findall(f".//{tag}")) == len(runtime.findall(f".//{tag}")),
            f"canonical snapshot must preserve runtime <{tag}> tag count",
        )

    refs = mesh_refs(snapshot)
    report.require(refs, "snapshot URDF has no mesh references")
    for ref in refs:
        report.require(
            ref.startswith(SNAPSHOT_PREFIX),
            f"snapshot mesh URI is not vendored under {SNAPSHOT_PREFIX}: {ref}",
        )
        relative = ref.removeprefix(SNAPSHOT_PREFIX)
        report.require((snapshot_dir / relative).exists(), f"snapshot mesh is missing: {relative}")
    report.require(
        not any(ref.startswith(XARM_PREFIX) for ref in refs),
        "canonical snapshot must not retain xarm_description mesh URIs",
    )


def validate_srdf(runtime: ET.Element, snapshot: ET.Element, report: Report) -> None:
    runtime_lite6_joints = group_joints(runtime, "lite6")
    snapshot_lite6_joints = group_joints(snapshot, "lite6")
    report.require(
        set(ARM_JOINTS).issubset(runtime_lite6_joints),
        "runtime SRDF lite6 group does not cover joint1..joint6",
    )
    report.require(
        snapshot_lite6_joints == runtime_lite6_joints,
        "canonical snapshot SRDF lite6 group must preserve runtime explicit joint group",
    )

    runtime_gripper = group_joints(runtime, "lite_gripper")
    snapshot_gripper = group_joints(snapshot, "lite_gripper")
    report.require(
        "right_finger_joint" in runtime_gripper,
        "runtime SRDF lite_gripper group missing right_finger_joint",
    )
    report.require(
        "right_finger_joint" in snapshot_gripper,
        "snapshot SRDF lite_gripper group missing right_finger_joint",
    )
    report.require(
        snapshot_gripper == runtime_gripper,
        "canonical snapshot SRDF lite_gripper group must preserve both runtime finger joints",
    )

    runtime_open = group_state_joints(runtime, "lite_gripper", "open")
    snapshot_open = group_state_joints(snapshot, "lite_gripper", "open")
    runtime_close = group_state_joints(runtime, "lite_gripper", "close")
    snapshot_close = group_state_joints(snapshot, "lite_gripper", "close")
    report.require(
        runtime_open.get("right_finger_joint") == snapshot_open.get("right_finger_joint") == "0.0081",
        "open preset for right_finger_joint must be 0.0081 in runtime and snapshot",
    )
    report.require(
        runtime_close.get("right_finger_joint") == snapshot_close.get("right_finger_joint") == "0",
        "close preset for right_finger_joint must be 0 in runtime and snapshot",
    )
    report.require(
        group_state_joints(snapshot, "lite_gripper", "closed").get("right_finger_joint") == "0",
        "snapshot SRDF must include closed alias for ManyForge UI compatibility",
    )
    report.require(
        group_state_joints(snapshot, "lite_gripper", "closed").get("left_finger_joint") == "0",
        "snapshot SRDF closed alias must include left_finger_joint",
    )

    snapshot_disabled = disable_collision_set(snapshot)
    runtime_disabled = disable_collision_set(runtime)
    report.require(
        EXPECTED_DISABLE_COLLISIONS.issubset(snapshot_disabled),
        "snapshot SRDF is missing expected Lite6 disable-collision entries",
    )
    report.require(
        EXPECTED_DISABLE_COLLISIONS.issubset(runtime_disabled),
        "runtime SRDF is missing expected Lite6 disable-collision entries",
    )


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    default_snapshot = root / "manyforge_example_models" / "models" / "lite6_lite_gripper"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-urdf", type=Path, required=True)
    parser.add_argument("--runtime-srdf", type=Path, required=True)
    parser.add_argument("--capture-json", type=Path)
    parser.add_argument("--snapshot-dir", type=Path, default=default_snapshot)
    parser.add_argument("--json-report", type=Path)
    args = parser.parse_args()

    snapshot_dir = args.snapshot_dir.resolve()
    runtime_urdf = parse_xml(args.runtime_urdf.resolve())
    runtime_srdf = parse_xml(args.runtime_srdf.resolve())
    snapshot_urdf = parse_xml(snapshot_dir / "lite6_lite_gripper.urdf")
    snapshot_srdf = parse_xml(snapshot_dir / "lite6_lite_gripper.srdf")

    report = Report()
    if args.capture_json is not None:
        validate_capture_metadata(
            args.capture_json.resolve(),
            args.runtime_urdf.resolve(),
            args.runtime_srdf.resolve(),
            report,
        )
    validate_urdf(runtime_urdf, snapshot_urdf, snapshot_dir, report)
    validate_srdf(runtime_srdf, snapshot_srdf, report)

    payload = {
        "passed": not report.failures,
        "failures": report.failures,
        "warnings": report.warnings,
        "facts": report.facts,
    }
    if args.json_report is not None:
        args.json_report.parent.mkdir(parents=True, exist_ok=True)
        args.json_report.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    json.dump(payload, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0 if not report.failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
