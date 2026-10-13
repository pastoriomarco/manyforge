#!/usr/bin/env python3
"""Export retained UR5e/Robotiq and UF850/xArm model bundles.

The runtime consumes only the expanded URDF, SRDF and copied meshes.  ROS,
Xacro and the upstream repositories are generation-time dependencies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


UR_DESCRIPTION_COMMIT = "89bbe795f38a7ab00fb66fe8831dfff79dc99edf"
ROBOTIQ_DESCRIPTION_COMMIT = "587b6548640e366d50322068021e20b487372a75"
XARM_ROS2_COMMIT = "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
XARM_SDK_COMMIT = "2b6b595a956c2558e867c08001fda454212b271a"
MANYMOVE_COMMIT = "f3a4c65e154744403d721d6ef25d2a8ea5f3f1e3"


def repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def require_file(path: Path, label: str) -> Path:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"{label} must be one plain file: {path}")
    return path


def require_directory(path: Path, label: str) -> Path:
    if not path.is_dir() or path.is_symlink():
        raise ValueError(f"{label} must be one plain directory: {path}")
    return path.resolve()


def git_value(root: Path, *arguments: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *arguments], text=True
    ).strip()


def require_revision(root: Path, expected: str, label: str) -> None:
    actual = git_value(root, "rev-parse", "HEAD")
    if actual != expected:
        raise ValueError(f"{label} revision is {actual}; expected {expected}")


def add_package(prefix: Path, package: str, source: Path) -> None:
    marker = prefix / "share" / "ament_index" / "resource_index" / "packages"
    marker.mkdir(parents=True, exist_ok=True)
    (marker / package).touch()
    share = prefix / "share" / package
    share.parent.mkdir(parents=True, exist_ok=True)
    share.symlink_to(source, target_is_directory=True)


def run_xacro(source: Path, output: Path, prefix: Path, *arguments: str) -> None:
    environment = dict(os.environ)
    existing = environment.get("AMENT_PREFIX_PATH")
    environment["AMENT_PREFIX_PATH"] = (
        str(prefix) if not existing else f"{prefix}:{existing}"
    )
    executable = shutil.which("xacro") or "/opt/ros/lyrical/bin/xacro"
    with output.open("wb") as stream:
        subprocess.run(
            [executable, str(source), *arguments],
            check=True,
            stdout=stream,
            env=environment,
        )


def write_xml(root: ET.Element, target: Path) -> None:
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(
        target, encoding="utf-8", xml_declaration=True, short_empty_elements=True
    )


def copy_meshes(
    root: ET.Element,
    *,
    target: Path,
    bundle_id: str,
    package_roots: dict[str, Path],
    file_roots: tuple[Path, ...] = (),
) -> list[str]:
    copied: set[str] = set()
    bundle_prefix = f"package://manyforge_example_models/models/{bundle_id}/"
    for mesh in root.iter("mesh"):
        source_uri = mesh.attrib.get("filename")
        if not source_uri:
            continue
        source: Path | None = None
        relative: Path | None = None
        if source_uri.startswith("package://"):
            remainder = source_uri.removeprefix("package://")
            package, separator, package_relative = remainder.partition("/")
            if not separator or package not in package_roots:
                raise ValueError(f"unsupported mesh package URI: {source_uri}")
            source = package_roots[package] / package_relative
            if package == "xarm_description":
                relative = Path(package_relative)
            elif package == "ur_description":
                relative = Path("meshes") / Path(package_relative).relative_to("meshes")
            else:
                relative = Path("meshes") / package / Path(package_relative).name
        elif source_uri.startswith("file://"):
            source = Path(source_uri.removeprefix("file://")).resolve()
            owner = next(
                (
                    candidate
                    for candidate in file_roots
                    if source.is_relative_to(candidate)
                ),
                None,
            )
            if owner is None:
                raise ValueError(
                    f"mesh file is outside the accepted sources: {source_uri}"
                )
            owner_relative = source.relative_to(owner)
            if "2f_85" not in owner_relative.parts:
                raise ValueError(f"unexpected Robotiq mesh path: {source_uri}")
            relative = Path("meshes/robotiq_2f_85") / Path(
                *owner_relative.parts[owner_relative.parts.index("2f_85") + 1 :]
            )
        else:
            raise ValueError(f"unsupported mesh URI: {source_uri}")
        require_file(source, "referenced mesh")
        assert relative is not None
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        mesh.set("filename", bundle_prefix + relative.as_posix())
        copied.add(relative.as_posix())
    return sorted(copied)


def ur5e_wrapper() -> str:
    return """<?xml version=\"1.0\"?>
<robot xmlns:xacro=\"http://wiki.ros.org/xacro\" name=\"ur5e_robotiq_2f_85\">
  <xacro:include filename=\"$(find ur_description)/urdf/ur_macro.xacro\"/>
  <xacro:include filename=\"$(find robotiq_description)/urdf/robotiq_2f_85_macro.urdf.xacro\"/>
  <link name=\"world\"/>
  <xacro:ur_robot name=\"ur5e\" tf_prefix=\"\" parent=\"world\"
    joint_limits_parameters_file=\"$(find ur_description)/config/ur5e/joint_limits.yaml\"
    kinematics_parameters_file=\"$(find ur_description)/config/ur5e/default_kinematics.yaml\"
    physical_parameters_file=\"$(find ur_description)/config/ur5e/physical_parameters.yaml\"
    visual_parameters_file=\"$(find ur_description)/config/ur5e/visual_parameters.yaml\"
    safety_limits=\"false\" force_abs_paths=\"false\">
    <origin xyz=\"0 0 0\" rpy=\"0 0 0\"/>
  </xacro:ur_robot>
  <xacro:robotiq_gripper name=\"robotiq_2f_85\" prefix=\"\" parent=\"tool0\"
    sim_ignition=\"false\" sim_isaac=\"false\" use_fake_hardware=\"false\"
    fake_sensor_commands=\"false\" include_ros2_control=\"false\">
    <origin xyz=\"0 0 0.01\" rpy=\"0 0 0\"/>
  </xacro:robotiq_gripper>
</robot>
"""


def add_ur5e_gripper_semantics(srdf_path: Path) -> None:
    root = ET.parse(srdf_path).getroot()
    arm = root.find("./group[@name='ur_manipulator']")
    if arm is None:
        raise ValueError("ManyMove UR SRDF lost ur_manipulator")
    gripper = ET.Element("group", {"name": "robotiq_2f_85_gripper"})
    for joint in (
        "robotiq_85_left_knuckle_joint",
        "robotiq_85_right_knuckle_joint",
        "robotiq_85_left_inner_knuckle_joint",
        "robotiq_85_right_inner_knuckle_joint",
        "robotiq_85_left_finger_tip_joint",
        "robotiq_85_right_finger_tip_joint",
    ):
        ET.SubElement(gripper, "joint", {"name": joint})
    root.insert(list(root).index(arm) + 1, gripper)
    for name, value in (("open", "0.0"), ("closed", "0.8")):
        state = ET.Element(
            "group_state", {"name": name, "group": "robotiq_2f_85_gripper"}
        )
        ET.SubElement(
            state,
            "joint",
            {"name": "robotiq_85_left_knuckle_joint", "value": value},
        )
        root.insert(list(root).index(gripper) + (1 if name == "open" else 2), state)
    root.insert(
        list(root).index(gripper) + 3,
        ET.Element(
            "end_effector",
            {
                "name": "robotiq_2f_85",
                "parent_link": "tool0",
                "group": "robotiq_2f_85_gripper",
            },
        ),
    )
    write_xml(root, srdf_path)


def bound_ur5e_robotiq_mimic_joints(root: ET.Element) -> None:
    """Make the upstream bounded mimic chain valid for Pinocchio.

    The upstream followers are declared continuous while mimicking one bounded
    revolute master. Pinocchio represents those joint kinds with different
    configuration dimensions and therefore rejects the chain. Preserve each
    mimic law and give every follower the exact transformed master range.
    """

    expected = {
        "robotiq_85_left_inner_knuckle_joint",
        "robotiq_85_right_inner_knuckle_joint",
        "robotiq_85_left_finger_tip_joint",
        "robotiq_85_right_finger_tip_joint",
    }
    master = root.find("./joint[@name='robotiq_85_left_knuckle_joint']")
    if master is None or master.attrib.get("type") != "revolute":
        raise ValueError("Robotiq bounded master joint is absent or incompatible")
    master_limit = master.find("limit")
    if master_limit is None:
        raise ValueError("Robotiq bounded master joint has no limit")
    master_lower = float(master_limit.attrib["lower"])
    master_upper = float(master_limit.attrib["upper"])
    converted: set[str] = set()
    for name in expected:
        joint = root.find(f"./joint[@name='{name}']")
        if joint is None or joint.attrib.get("type") != "continuous":
            raise ValueError(f"Robotiq mimic follower is absent or no longer continuous: {name}")
        mimic = joint.find("mimic")
        if mimic is None or mimic.attrib.get("joint") != master.attrib["name"]:
            raise ValueError(f"Robotiq mimic follower lost its master: {name}")
        multiplier = float(mimic.attrib.get("multiplier", "1"))
        offset = float(mimic.attrib.get("offset", "0"))
        limits = sorted(
            (master_lower * multiplier + offset, master_upper * multiplier + offset)
        )
        joint.set("type", "revolute")
        limit = ET.Element(
            "limit",
            {
                "effort": master_limit.attrib["effort"],
                "lower": str(limits[0]),
                "upper": str(limits[1]),
                "velocity": master_limit.attrib["velocity"],
            },
        )
        joint.insert(list(joint).index(mimic), limit)
        converted.add(name)
    if converted != expected:
        raise ValueError("Robotiq bounded mimic projection is incomplete")


def add_uf850_gripper_semantics(srdf_path: Path) -> None:
    root = ET.parse(srdf_path).getroot()
    group = root.find("./group[@name='uf850_gripper']")
    if group is None:
        raise ValueError("xArm SRDF lost the uf850_gripper group")
    for joint_name in (
        "left_finger_joint",
        "left_inner_knuckle_joint",
        "right_outer_knuckle_joint",
        "right_finger_joint",
        "right_inner_knuckle_joint",
    ):
        ET.SubElement(group, "joint", {"name": joint_name})
    close = root.find("./group_state[@name='close'][@group='uf850_gripper']")
    if close is None:
        raise ValueError("xArm SRDF lost the uf850_gripper close state")
    closed = ET.fromstring(ET.tostring(close, encoding="unicode"))
    closed.set("name", "closed")
    root.insert(list(root).index(close) + 1, closed)
    write_xml(root, srdf_path)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def documented_divergences(model: str) -> list[dict[str, str]]:
    if model == "ur5e_robotiq_2f_85":
        return [
            {
                "document": "ur5e_robotiq_2f_85.urdf",
                "change": (
                    "Normalize XML and mesh URIs; represent four continuous mimic "
                    "followers as bounded revolute joints"
                ),
                "effect": (
                    "Mimic laws and transformed motion ranges are unchanged; the bounded "
                    "representation is compatible with Pinocchio"
                ),
            },
            {
                "document": "ur5e_robotiq_2f_85.srdf",
                "change": (
                    "Add the Robotiq gripper group, open/closed states, and end-effector "
                    "declaration"
                ),
                "effect": "ManyMove arm groups and collision semantics remain unchanged",
            },
            {
                "document": "prepared structural view",
                "change": "Remove two direct collision/surface runtime-physics extensions",
                "effect": (
                    "Collision elements and their geometry remain; the preparation receipt "
                    "records the removal"
                ),
            },
        ]
    return [
        {
            "document": "uf850_xarm_gripper.urdf",
            "change": "XML serialization normalization and mesh URI localization",
            "effect": "No kinematic, physical, visual, or collision geometry changes",
        },
        {
            "document": "uf850_xarm_gripper.srdf",
            "change": (
                "Add the five existing URDF mimic joints to the gripper group and add a "
                "closed alias for the upstream close state"
            ),
            "effect": (
                "drive_joint remains the only commanded joint; no joint or mimic law is "
                "introduced"
            ),
        },
        {
            "document": "prepared structural view",
            "change": (
                "Remove top-level Gazebo, ros2_control, and transmission extensions and one "
                "exact duplicate unordered SRDF collision-disable row"
            ),
            "effect": (
                "Structural geometry and unique collision semantics remain; the preparation "
                "receipt records every removal"
            ),
        },
    ]


def write_manifest(
    target: Path,
    *,
    bundle_id: str,
    sources: list[dict[str, str]],
) -> None:
    files = []
    for path in sorted(target.rglob("*")):
        if path.is_file() and path.name != "STATIC_MANIFEST.json":
            files.append(
                {
                    "path": path.relative_to(target).as_posix(),
                    "size_bytes": path.stat().st_size,
                    "sha256": digest(path),
                }
            )
    value = {
        "schema": "manyforge.retained_robot_model_manifest.v1",
        "model_id": bundle_id,
        "sources": sources,
        "documented_divergences": documented_divergences(bundle_id),
        "files": files,
    }
    (target / "STATIC_MANIFEST.json").write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def export_ur5e(
    args: argparse.Namespace, target: Path, temporary: Path
) -> list[dict[str, str]]:
    ur = require_directory(args.ur_description, "UR description repository")
    robotiq = require_directory(args.robotiq_description, "Robotiq repository")
    require_revision(ur, UR_DESCRIPTION_COMMIT, "UR description")
    require_revision(robotiq, ROBOTIQ_DESCRIPTION_COMMIT, "Robotiq description")
    add_package(temporary, "ur_description", ur)
    robotiq_package = robotiq / "robotiq_description"
    add_package(temporary, "robotiq_description", robotiq_package)
    wrapper = temporary / "ur5e_robotiq_2f_85.urdf.xacro"
    wrapper.write_text(ur5e_wrapper(), encoding="utf-8")
    upstream = target / "upstream_source"
    upstream.mkdir()
    expanded_urdf = upstream / "ur5e_robotiq_2f_85.expanded.urdf"
    run_xacro(wrapper, expanded_urdf, temporary)
    urdf = target / "ur5e_robotiq_2f_85.urdf"
    shutil.copy2(expanded_urdf, urdf)
    urdf_root = ET.parse(urdf).getroot()
    copy_meshes(
        urdf_root,
        target=target,
        bundle_id="ur5e_robotiq_2f_85",
        package_roots={"ur_description": ur, "robotiq_description": robotiq_package},
        file_roots=(robotiq_package,),
    )
    bound_ur5e_robotiq_mimic_joints(urdf_root)
    write_xml(urdf_root, urdf)
    expanded_srdf = upstream / "ur5e_robotiq_2f_85.expanded.srdf"
    srdf = target / "ur5e_robotiq_2f_85.srdf"
    manymove = require_directory(
        repository_root().parent / "manymove", "ManyMove repository"
    )
    require_revision(manymove, MANYMOVE_COMMIT, "ManyMove")
    manymove_srdf = manymove / "manymove_bringup/srdf/ur_with_robotiq.srdf.xacro"
    run_xacro(
        require_file(manymove_srdf, "ManyMove UR SRDF"),
        expanded_srdf,
        temporary,
        "name:=ur5e_robotiq_2f_85",
    )
    shutil.copy2(expanded_srdf, srdf)
    add_ur5e_gripper_semantics(srdf)
    licenses = target / "licenses"
    licenses.mkdir()
    shutil.copy2(
        require_file(ur / "LICENSE", "UR license"),
        licenses / "Universal_Robots_ROS2_Description.LICENSE",
    )
    shutil.copy2(
        require_file(robotiq / "LICENSE", "Robotiq license"),
        licenses / "ros2_robotiq_gripper.LICENSE",
    )
    shutil.copy2(
        require_file(manymove / "LICENSE", "ManyMove license"),
        licenses / "manymove.LICENSE",
    )
    return [
        {
            "repository": "https://github.com/UniversalRobots/Universal_Robots_ROS2_Description",
            "commit": UR_DESCRIPTION_COMMIT,
        },
        {
            "repository": "https://github.com/NVIDIA-ISAAC-ROS/ros2_robotiq_gripper",
            "commit": ROBOTIQ_DESCRIPTION_COMMIT,
        },
        {
            "repository": "https://github.com/pastoriomarco/manymove",
            "commit": MANYMOVE_COMMIT,
            "role": "SRDF collision semantics",
        },
    ]


def export_uf850(
    args: argparse.Namespace, target: Path, temporary: Path
) -> list[dict[str, str]]:
    xarm = require_directory(args.xarm_ros2, "xarm_ros2 repository")
    require_revision(xarm, XARM_ROS2_COMMIT, "xarm_ros2")
    submodule = git_value(xarm, "rev-parse", "HEAD:xarm_sdk/cxx")
    if submodule != XARM_SDK_COMMIT:
        raise ValueError(
            f"xarm SDK revision is {submodule}; expected {XARM_SDK_COMMIT}"
        )
    add_package(temporary, "xarm_description", xarm / "xarm_description")
    add_package(temporary, "xarm_moveit_config", xarm / "xarm_moveit_config")
    upstream = target / "upstream_source"
    upstream.mkdir()
    expanded_urdf = upstream / "uf850_xarm_gripper.expanded.urdf"
    run_xacro(
        xarm / "xarm_description/urdf/xarm_device.urdf.xacro",
        expanded_urdf,
        temporary,
        "robot_type:=uf850",
        "dof:=6",
        "add_gripper:=true",
        "limited:=true",
    )
    urdf = target / "uf850_xarm_gripper.urdf"
    shutil.copy2(expanded_urdf, urdf)
    urdf_root = ET.parse(urdf).getroot()
    copy_meshes(
        urdf_root,
        target=target,
        bundle_id="uf850_xarm_gripper",
        package_roots={"xarm_description": xarm / "xarm_description"},
    )
    write_xml(urdf_root, urdf)
    expanded_srdf = upstream / "uf850_xarm_gripper.expanded.srdf"
    srdf = target / "uf850_xarm_gripper.srdf"
    run_xacro(
        xarm / "xarm_moveit_config/srdf/xarm.srdf.xacro",
        expanded_srdf,
        temporary,
        "robot_type:=uf850",
        "dof:=6",
        "add_gripper:=true",
    )
    shutil.copy2(expanded_srdf, srdf)
    add_uf850_gripper_semantics(srdf)
    licenses = target / "licenses"
    licenses.mkdir()
    shutil.copy2(
        require_file(xarm / "LICENSE", "xarm_ros2 license"),
        licenses / "xarm_ros2.LICENSE",
    )
    upstream_config = target / "upstream_source/xarm_moveit_config/config"
    for name in ("uf850", "uf850_gripper"):
        destination = upstream_config / name
        destination.mkdir(parents=True)
        shutil.copy2(
            require_file(
                xarm / f"xarm_moveit_config/config/{name}/joint_limits.yaml",
                f"{name} joint limits",
            ),
            destination / "joint_limits.yaml",
        )
    return [
        {
            "repository": "https://github.com/pastoriomarco/xarm_ros2",
            "branch": "jazzy_no_gazebo",
            "commit": XARM_ROS2_COMMIT,
            "submodule_commit": XARM_SDK_COMMIT,
        }
    ]


def readme_text(model: str) -> str:
    if model == "ur5e_robotiq_2f_85":
        return f"""# UR5e + Robotiq 2F-85

Self-contained, generation-time-expanded ManyForge model. The runtime does
not depend on ROS, Xacro, MoveIt, or the upstream repositories.

Sources and licenses:

- UR5e structure, physical parameters, limits and meshes: official
  `Universal_Robots_ROS2_Description` at `{UR_DESCRIPTION_COMMIT}`; see
  `licenses/Universal_Robots_ROS2_Description.LICENSE`.
- Robotiq 2F-85 structure and meshes: NVIDIA Isaac ROS
  `ros2_robotiq_gripper` at `{ROBOTIQ_DESCRIPTION_COMMIT}`; see
  `licenses/ros2_robotiq_gripper.LICENSE`.
- Base SRDF collision semantics: ManyMove at `{MANYMOVE_COMMIT}`; see
  `licenses/manymove.LICENSE`. The ordinary gripper group and open/closed
  states are added here because the ManyMove SRDF controls only the arm.

The arm TCP remains the source-compatible `tool0` used by ManyMove. The
Robotiq mount frame is separately exposed as `robotiq_85_base_link`; a
site-calibrated grasp TCP can therefore be introduced without changing the
vendor geometry. Exact expanded upstream XML is retained unchanged under
`upstream_source/`.

Documented divergences in the deployable projection:

- The primary URDF normalizes XML and mesh URIs. It changes the four Robotiq
  mimic followers from `continuous` to bounded `revolute` joints with limits
  derived exactly from the existing master joint and mimic multiplier. This
  preserves their motion while making the bounded mimic chain valid in
  Pinocchio. Links, inertials, visuals and collision geometry are unchanged.
- The primary SRDF adds the Robotiq gripper group, open/closed states and
  end-effector declaration. ManyMove's arm groups and collision semantics are
  unchanged.
- The prepared structural view removes two direct `collision/surface`
  runtime-physics extensions. It retains each collision and its geometry and
  records the removal count in the preparation receipt.

`STATIC_MANIFEST.json` binds all retained source, license and mesh bytes.
Redistribution must retain the notices above.
"""
    return f"""# UF850 + xArm Gripper

Self-contained, generation-time-expanded ManyForge model. The runtime does
not depend on ROS, Xacro, MoveIt, `uf_ros_lib`, or xarm_ros2.

The arm, standard xArm gripper, physical parameters, kinematics, MoveIt joint
limits and meshes come from `https://github.com/pastoriomarco/xarm_ros2`,
branch `jazzy_no_gazebo`, commit `{XARM_ROS2_COMMIT}` with SDK submodule
`{XARM_SDK_COMMIT}`. See `licenses/xarm_ros2.LICENSE` (BSD 3-Clause). The
upstream `xarm_description/package.xml` still carries a placeholder license
field, so this bundle records and retains the repository-root license that
governs the source tree instead of repeating that incomplete metadata.

The model uses the same `robot_type=uf850`, `dof=6`, `limited=true`,
`add_gripper=true` configuration as the ManyMove UF850 example. The upstream
gripper link group and `close` state are retained; its existing mimic joints
are made explicit group members and a ManyForge-compatible `closed` alias is
added. Exact expanded upstream XML is retained unchanged under
`upstream_source/`.

Documented divergences in the deployable projection:

- The primary URDF has only normalized XML serialization and localized mesh
  URIs. Kinematics, physical data, visuals and collision geometry are unchanged.
- The primary SRDF adds the five existing URDF mimic joints to the gripper
  group and adds `closed` as an alias for the upstream `close` state.
  `drive_joint` remains the only commanded joint; no joint or mimic law is
  introduced.
- The prepared structural view removes top-level Gazebo, `ros2_control` and
  transmission extensions plus one exact duplicate unordered SRDF
  collision-disable row. It retains structural geometry and unique collision
  semantics and records every removal in the preparation receipt.

`STATIC_MANIFEST.json` binds all retained source, license, limit and mesh
bytes. Redistribution must retain the notice above.
"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model", required=True, choices=("ur5e_robotiq_2f_85", "uf850_xarm_gripper")
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=repository_root() / "manyforge_example_models/models",
    )
    parser.add_argument("--ur-description", type=Path)
    parser.add_argument("--robotiq-description", type=Path)
    parser.add_argument("--xarm-ros2", type=Path)
    args = parser.parse_args()
    output_root = args.output_root.resolve() / args.model
    if output_root.exists():
        raise ValueError(f"refusing to overwrite retained model bundle: {output_root}")
    output_root.mkdir(parents=True)
    try:
        with tempfile.TemporaryDirectory(prefix="manyforge-model-export-") as directory:
            temporary = Path(directory)
            if args.model == "ur5e_robotiq_2f_85":
                if args.ur_description is None or args.robotiq_description is None:
                    parser.error(
                        "UR5e export requires --ur-description and --robotiq-description"
                    )
                sources = export_ur5e(args, output_root, temporary)
            else:
                if args.xarm_ros2 is None:
                    parser.error("UF850 export requires --xarm-ros2")
                sources = export_uf850(args, output_root, temporary)
        readme = output_root / "README.md"
        readme.write_text(readme_text(args.model), encoding="utf-8")
        write_manifest(output_root, bundle_id=args.model, sources=sources)
    except Exception:
        shutil.rmtree(output_root)
        raise


if __name__ == "__main__":
    main()
