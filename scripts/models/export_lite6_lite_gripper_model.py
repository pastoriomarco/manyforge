#!/usr/bin/env python3
"""Export a self-contained Lite6 + UFACTORY lite-gripper model for ManyForge.

This is a generation-time tool. It consumes the operator's xarm_ros2 fork plus
the already-expanded ManyMove Lite6 URDF, then writes a static URDF/SRDF/mesh
bundle under manyforge_example_models. The canonical URDF/SRDF preserve the
ManyMove/xarm robot semantics and are not rewritten to satisfy one importer.
The generated ManyForge native lane does not depend on ROS, MoveIt, uf_ros_lib,
or xarm_ros2 at runtime.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
import shutil
import subprocess
import textwrap
import xml.etree.ElementTree as ET
from pathlib import Path

import yaml


REQUIRED_MESHES = (
    "meshes/lite6/visual/link_base.stl",
    "meshes/lite6/visual/link1.stl",
    "meshes/lite6/visual/link2.stl",
    "meshes/lite6/visual/link3.stl",
    "meshes/lite6/visual/link4.stl",
    "meshes/lite6/visual/link5.stl",
    "meshes/lite6/visual/link6.stl",
    "meshes/lite6/collision/link_base.stl",
    "meshes/lite6/collision/link1.stl",
    "meshes/lite6/collision/link2.stl",
    "meshes/lite6/collision/link3.stl",
    "meshes/lite6/collision/link4.stl",
    "meshes/lite6/collision/link5.stl",
    "meshes/lite6/collision/link6.stl",
    "meshes/gripper/lite/visual/gripper_lite_body.stl",
    "meshes/gripper/lite/visual/gripper_lite_finger.stl",
    "meshes/gripper/lite/collision/gripper_lite_body.stl",
    "meshes/gripper/lite/collision/gripper_lite_finger.stl",
)

STATIC_BUNDLE_MANIFEST = "STATIC_MANIFEST.json"

ARM_JOINTS = ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6")
MOVEIT_JOINT_LIMITS_SOURCE = Path(
    "xarm_moveit_config/config/lite6/joint_limits.yaml"
)
OWNED_MOVEIT_JOINT_LIMITS_SOURCE = (
    Path("upstream_source") / MOVEIT_JOINT_LIMITS_SOURCE
)


MANYMOVE_LIMITED_LITE6_BOUNDS = {
    "joint1": ("-3.1101767270538954", "3.1101767270538954"),
    "joint3": ("-0.061087", "3.1101767270538954"),
    "joint4": ("-3.1101767270538954", "3.1101767270538954"),
    "joint6": ("-3.1101767270538954", "3.1101767270538954"),
}


DISABLE_COLLISIONS = (
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
)


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_git(path: Path, *args: str) -> str:
    try:
        return subprocess.check_output(
            ["git", "-C", str(path), *args],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        return "unknown"


def require(path: Path, description: str) -> None:
    if not path.exists():
        raise FileNotFoundError(f"missing {description}: {path}")


def remove_elements_by_tag(root: ET.Element, tag: str) -> int:
    removed = 0
    for parent in list(root.iter()):
        for child in list(parent):
            if child.tag == tag:
                parent.remove(child)
                removed += 1
    return removed


def remove_xml_comments(root: ET.Element) -> int:
    removed = 0
    for parent in list(root.iter()):
        for child in list(parent):
            if not isinstance(child.tag, str):
                parent.remove(child)
                removed += 1
    return removed


def rewrite_mesh_uris(root: ET.Element) -> None:
    for mesh in root.iter("mesh"):
        filename = mesh.attrib.get("filename", "")
        prefix = "package://xarm_description/"
        if filename.startswith(prefix):
            mesh.set(
                "filename",
                "package://manyforge_example_models/models/"
                f"lite6_lite_gripper/{filename.removeprefix(prefix)}",
            )


def apply_manymove_limited_arm_bounds(root: ET.Element) -> None:
    """Match ManyMove's default `limited=True` Lite6 runtime contract.

    The reusable `lite6_grf.urdf` artifact is expanded with wider xarm bounds
    for a few arm joints, while the ManyMove Lite6 launch used as the
    qualification source defaults `limited=True`. The static ManyForge snapshot
    is qualified against that runtime model, so its planning limits must match
    the runtime URDF exactly.
    """
    for joint_name, (lower, upper) in MANYMOVE_LIMITED_LITE6_BOUNDS.items():
        joint = root.find(f"./joint[@name='{joint_name}']")
        if joint is None:
            raise ValueError(f"expanded Lite6 URDF is missing {joint_name}")
        limit = joint.find("limit")
        if limit is None:
            raise ValueError(f"expanded Lite6 URDF joint {joint_name} is missing <limit>")
        limit.set("lower", lower)
        limit.set("upper", upper)


def _positive_number(
    value: object, label: str, *, allow_text: bool = False
) -> float:
    if isinstance(value, bool) or not isinstance(
        value, (int, float, str) if allow_text else (int, float)
    ):
        raise ValueError(f"{label} must be numeric")
    try:
        result = float(value)
    except ValueError as exc:
        raise ValueError(f"{label} must be numeric") from exc
    if not math.isfinite(result) or result <= 0.0:
        raise ValueError(f"{label} must be finite and positive")
    return result


def load_moveit_joint_dynamics(
    source: Path,
) -> dict[str, tuple[float, float]]:
    """Read the exact Lite6 MoveIt planning envelope used by the source model."""
    require(source, "Lite6 MoveIt joint limits")
    payload = yaml.safe_load(source.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or not isinstance(
        payload.get("joint_limits"), dict
    ):
        raise ValueError("Lite6 MoveIt joint limits must contain joint_limits")
    values = payload["joint_limits"]
    if set(values) != set(ARM_JOINTS):
        raise ValueError("Lite6 MoveIt joint limits must cover exactly joint1..joint6")

    result: dict[str, tuple[float, float]] = {}
    for joint_name in ARM_JOINTS:
        row = values[joint_name]
        if not isinstance(row, dict):
            raise ValueError(f"MoveIt limits for {joint_name} must be a mapping")
        if row.get("has_velocity_limits") is not True:
            raise ValueError(f"MoveIt velocity limit is disabled for {joint_name}")
        if row.get("has_acceleration_limits") is not True:
            raise ValueError(f"MoveIt acceleration limit is disabled for {joint_name}")
        result[joint_name] = (
            _positive_number(
                row.get("max_velocity"),
                f"MoveIt max_velocity for {joint_name}",
            ),
            _positive_number(
                row.get("max_acceleration"),
                f"MoveIt max_acceleration for {joint_name}",
            ),
        )
    return result


def apply_moveit_joint_dynamics(
    root: ET.Element,
    moveit_joint_dynamics: dict[str, tuple[float, float]],
) -> None:
    """Materialize one conservative model-dynamics view in the owned URDF.

    The source URDF and MoveIt configuration remain distinct retained inputs.
    RoboPlan consumes the normalized URDF, so native, simulation, real-lane
    validation, direct-control descriptors, and the UI all observe the same
    conservative minimum without lane-specific constants.
    """
    if set(moveit_joint_dynamics) != set(ARM_JOINTS):
        raise ValueError("normalized Lite6 dynamics must cover exactly joint1..joint6")
    for joint_name in ARM_JOINTS:
        joint = root.find(f"./joint[@name='{joint_name}']")
        if joint is None:
            raise ValueError(f"expanded Lite6 URDF is missing {joint_name}")
        limit = joint.find("limit")
        if limit is None:
            raise ValueError(f"expanded Lite6 URDF joint {joint_name} is missing <limit>")
        urdf_velocity = _positive_number(
            limit.attrib.get("velocity"),
            f"URDF velocity for {joint_name}",
            allow_text=True,
        )
        moveit_velocity, moveit_acceleration = moveit_joint_dynamics[joint_name]
        effective_velocity = min(urdf_velocity, moveit_velocity)
        existing_acceleration = limit.attrib.get("acceleration")
        effective_acceleration = moveit_acceleration
        if existing_acceleration is not None:
            effective_acceleration = min(
                _positive_number(
                    existing_acceleration,
                    f"URDF acceleration for {joint_name}",
                    allow_text=True,
                ),
                moveit_acceleration,
            )
        limit.set("velocity", str(effective_velocity))
        # RoboPlan deliberately supports acceleration as an extended URDF limit.
        limit.set("acceleration", str(effective_acceleration))


def load_rewritten_urdf(
    source_urdf: Path,
    moveit_joint_dynamics: dict[str, tuple[float, float]],
) -> ET.Element:
    parser = ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))
    root = ET.parse(source_urdf, parser=parser).getroot()
    root.set("name", "lite6_lite_gripper")
    root.set("version", "1.2")
    rewrite_mesh_uris(root)
    apply_manymove_limited_arm_bounds(root)
    apply_moveit_joint_dynamics(root, moveit_joint_dynamics)
    return root


def write_xml(root: ET.Element, output_path: Path) -> None:
    ET.indent(root, space="  ")
    ET.ElementTree(root).write(output_path, encoding="utf-8", xml_declaration=True)


def write_urdf(
    source_urdf: Path,
    output_path: Path,
    moveit_joint_dynamics: dict[str, tuple[float, float]],
) -> None:
    """Write the canonical self-contained Lite6 URDF."""
    write_xml(load_rewritten_urdf(source_urdf, moveit_joint_dynamics), output_path)


def write_srdf(output_path: Path) -> None:
    """Write the canonical SRDF matching the ManyMove runtime capture."""
    lines = [
        '<?xml version="1.0"?>',
        "<!--",
        "  Static ManyForge SRDF for UFACTORY Lite6 + lite pneumatic gripper.",
        "  Materialized from xarm_moveit_config/srdf/xarm.srdf.xacro through",
        "  ManyMove's Lite6 bringup with prefix='', robot_type=lite, dof=6,",
        "  add_gripper=true.",
        "-->",
        '<robot name="lite6_lite_gripper">',
        '  <group name="lite6">',
        '    <joint name="world_joint"/>',
        '    <joint name="joint1"/>',
        '    <joint name="joint2"/>',
        '    <joint name="joint3"/>',
        '    <joint name="joint4"/>',
        '    <joint name="joint5"/>',
        '    <joint name="joint6"/>',
        '    <joint name="joint_eef"/>',
        '    <joint name="gripper_fix"/>',
        '    <joint name="joint_tcp"/>',
        "  </group>",
        '  <group_state name="home" group="lite6">',
        '    <joint name="joint1" value="0"/>',
        '    <joint name="joint2" value="0"/>',
        '    <joint name="joint3" value="0"/>',
        '    <joint name="joint4" value="0"/>',
        '    <joint name="joint5" value="0"/>',
        '    <joint name="joint6" value="0"/>',
        "  </group_state>",
        '  <group_state name="hold-up" group="lite6">',
        '    <joint name="joint1" value="0"/>',
        '    <joint name="joint2" value="0"/>',
        '    <joint name="joint3" value="0"/>',
        '    <joint name="joint4" value="0"/>',
        '    <joint name="joint5" value="-1.5708"/>',
        '    <joint name="joint6" value="0"/>',
        "  </group_state>",
        '  <group name="lite_gripper">',
        '    <link name="lite_gripper_base_link"/>',
        '    <link name="lite_gripper_left_finger"/>',
        '    <link name="lite_gripper_right_finger"/>',
        '    <link name="link_tcp"/>',
        '    <joint name="left_finger_joint"/>',
        '    <joint name="right_finger_joint"/>',
        "  </group>",
        '  <group_state name="open" group="lite_gripper">',
        '    <joint name="left_finger_joint" value="0.0081"/>',
        '    <joint name="right_finger_joint" value="0.0081"/>',
        "  </group_state>",
        '  <group_state name="close" group="lite_gripper">',
        '    <joint name="left_finger_joint" value="0"/>',
        '    <joint name="right_finger_joint" value="0"/>',
        "  </group_state>",
        '  <group_state name="closed" group="lite_gripper">',
        '    <joint name="left_finger_joint" value="0"/>',
        '    <joint name="right_finger_joint" value="0"/>',
        "  </group_state>",
        '  <end_effector name="lite_gripper" parent_link="link_tcp" group="lite_gripper"/>',
    ]
    for link1, link2, reason in DISABLE_COLLISIONS:
        lines.append(
            f'  <disable_collisions link1="{link1}" link2="{link2}" reason="{reason}"/>'
        )
    lines.append("</robot>")
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_isaac_topic_based_ros2_control_urdf(
    source_urdf: Path,
    output_path: Path,
    moveit_joint_dynamics: dict[str, tuple[float, float]],
) -> None:
    """Write the Isaac topic-based ros2_control URDF from the canonical model.

    This file is an execution resource for controller_manager, not a reduced
    planning model. It preserves the canonical links, joints, geometry, sibling
    mimic, Gazebo tags, and transmissions, while replacing the xarm-generated
    ros2_control blocks with the ManyForge-owned topic_based_ros2_control arm
    system used by the Isaac smoke lane. The gripper remains outside
    ros2_control in this lane because ManyForge drives it through the
    GripperCommand-to-Isaac-JointState bridge.
    """
    root = load_rewritten_urdf(source_urdf, moveit_joint_dynamics)
    root.set("name", "manyforge_isaac_lite6_topic_based")
    remove_xml_comments(root)
    remove_elements_by_tag(root, "ros2_control")

    ros2_control = ET.Element(
        "ros2_control",
        {"name": "IsaacLite6TopicBasedSystem", "type": "system"},
    )
    hardware = ET.SubElement(ros2_control, "hardware")
    ET.SubElement(hardware, "plugin").text = "topic_based_ros2_control/TopicBasedSystem"
    params = {
        "joint_commands_topic": "{joint_commands_topic}",
        "joint_states_topic": "{joint_states_topic}",
        "trigger_joint_command_threshold": "{trigger_joint_command_threshold}",
        "sum_wrapped_joint_states": "{sum_wrapped_joint_states}",
    }
    for name, value in params.items():
        ET.SubElement(hardware, "param", {"name": name}).text = value

    for joint_name in ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6"):
        joint = ET.SubElement(ros2_control, "joint", {"name": joint_name})
        ET.SubElement(joint, "command_interface", {"name": "position"})
        position = ET.SubElement(joint, "state_interface", {"name": "position"})
        ET.SubElement(position, "param", {"name": "initial_value"}).text = (
            "{" + f"{joint_name}_initial" + "}"
        )
        ET.SubElement(joint, "state_interface", {"name": "velocity"})

    insert_index = 0
    for idx, child in enumerate(list(root)):
        if child.tag == "joint" and child.attrib.get("name") == "world_joint":
            insert_index = idx + 1
            break
    root.insert(insert_index, ros2_control)
    write_xml(root, output_path)


def copy_meshes(xarm_description: Path, model_dir: Path) -> None:
    for rel in REQUIRED_MESHES:
        src = xarm_description / rel
        dst = model_dir / rel
        require(src, "xarm mesh")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def copy_moveit_joint_limits(source: Path, model_dir: Path) -> None:
    destination = model_dir / OWNED_MOVEIT_JOINT_LIMITS_SOURCE
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def write_readme(
    model_dir: Path,
    xarm_root: Path,
    source_urdf: Path,
    generated_at: str,
    moveit_joint_dynamics: dict[str, tuple[float, float]],
) -> None:
    sha = run_git(xarm_root, "rev-parse", "HEAD")
    branch = run_git(xarm_root, "rev-parse", "--abbrev-ref", "HEAD")
    unique_dynamics = set(moveit_joint_dynamics.values())
    if len(unique_dynamics) != 1:
        raise ValueError("Lite6 README requires one common six-joint dynamics envelope")
    velocity, acceleration = next(iter(unique_dynamics))
    text = f"""# UFACTORY Lite6 + Lite Pneumatic Gripper

> **Current boundary:** this static, provenance-pinned model bundle remains
> usable as authoring/qualification input. It does not provide native, Isaac,
> or real-robot runtime availability. The generation command and runtime notes
> below record the captured source revision; do not reinterpret them as a
> current xArm checkout or runnable lane.

Self-contained ManyForge preview model for UFACTORY Lite6 with the lite
pneumatic gripper. `lite6_lite_gripper.urdf` and
`lite6_lite_gripper.srdf` are the canonical ManyForge-vendored robot model and
preserve the ManyMove/xarm semantics, including the sibling-finger mimic and
the complete Lite6/lite-gripper SRDF groups.

Generated by:

```bash
python scripts/models/export_lite6_lite_gripper_model.py \\
  --xarm-ros2 {xarm_root} \\
  --source-urdf {source_urdf}
```

Generation-time source:

- Repository: https://github.com/pastoriomarco/xarm_ros2
- Branch: `{branch}`
- Commit: `{sha}`
- Generated at: `{generated_at}`
- Semantic source: `xarm_moveit_config/srdf/_lite6_macro.srdf.xacro`
- Model dynamics source: `{MOVEIT_JOINT_LIMITS_SOURCE.as_posix()}`
- Arguments: `prefix=''`, `robot_type=lite`, `dof=6`, `add_gripper=true`,
  `limited=true`, `ros2_control_plugin=isaac`

Historical runtime boundary:

- ManyForge native execution used this static mesh/model bundle only.
- It does not require xarm_ros2, uf_ros_lib, MoveIt, or ROS at runtime.
- Real-robot execution used xarm_ros2 externally through ROS 2 contracts.

Notes:

- Mesh URIs were rewritten from `package://xarm_description/...` to
  `package://manyforge_example_models/models/lite6_lite_gripper/...`.
- The canonical URDF preserves the expanded ManyMove runtime tags, including
  Gazebo, ros2_control, transmission, and mimic tags after package URI rewrite.
- Arm position limits are normalized to ManyMove's default Lite6
  `limited=true` runtime bounds. The MoveIt planning envelope is then combined
  conservatively with the expanded URDF and materialized once in the owned
  canonical URDF: `{velocity}` rad/s and `{acceleration}` rad/s^2 for each arm
  joint. The canonical document declares URDF 1.2 so independent consumers
  recognize the standard acceleration attribute. The pinned raw MoveIt input is retained at:
  `{OWNED_MOVEIT_JOINT_LIMITS_SOURCE.as_posix()}`.
  Historical native ManyForge
  did not load that YAML directly; current lanes consume its normalized model
  meaning through the same retained model adapter.
- The xarm fixed base mesh link remains `link_base`; no `base_link` alias is
  inserted into the canonical model.
- `STATIC_MANIFEST.json` records the generated URDF/SRDF/license/mesh file
  sizes and SHA-256 digests for reproducible drift checks.
"""
    (model_dir / "README.md").write_text(text, encoding="utf-8")


def write_license(model_dir: Path, xarm_root: Path) -> None:
    license_src = xarm_root / "LICENSE"
    require(license_src, "xarm_ros2 license")
    licenses = model_dir / "licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(license_src, licenses / "xarm_ros2.LICENSE")


def static_bundle_files(model_dir: Path) -> list[Path]:
    files: list[Path] = [
        model_dir / "README.md",
        model_dir / "lite6_lite_gripper.urdf",
        model_dir / "lite6_lite_gripper.srdf",
        model_dir / "licenses" / "xarm_ros2.LICENSE",
        model_dir / OWNED_MOVEIT_JOINT_LIMITS_SOURCE,
    ]
    files.extend(model_dir / rel for rel in REQUIRED_MESHES)
    return sorted(files, key=lambda path: path.relative_to(model_dir).as_posix())


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_static_manifest(
    model_dir: Path,
    xarm_root: Path,
    source_urdf: Path,
    generated_at: str,
) -> None:
    files = []
    for path in static_bundle_files(model_dir):
        require(path, "Lite6 static bundle file")
        files.append(
            {
                "path": path.relative_to(model_dir).as_posix(),
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )

    manifest = {
        "schema": "manyforge.lite6_static_model_manifest.v1",
        "generated_at_utc": generated_at,
        "source": {
            "repository": "https://github.com/pastoriomarco/xarm_ros2",
            "branch": run_git(xarm_root, "rev-parse", "--abbrev-ref", "HEAD"),
            "commit": run_git(xarm_root, "rev-parse", "HEAD"),
            "source_urdf": str(source_urdf),
            "semantic_source": "xarm_moveit_config/srdf/_lite6_macro.srdf.xacro",
            "moveit_joint_limits": {
                "upstream_path": MOVEIT_JOINT_LIMITS_SOURCE.as_posix(),
                "owned_snapshot_path": OWNED_MOVEIT_JOINT_LIMITS_SOURCE.as_posix(),
                "sha256": sha256_file(
                    model_dir / OWNED_MOVEIT_JOINT_LIMITS_SOURCE
                ),
            },
            "arguments": {
                "prefix": "",
                "robot_type": "lite",
                "dof": 6,
                "add_gripper": True,
                "limited": True,
                "ros2_control_plugin": "isaac",
            },
        },
        "files": files,
    }
    (model_dir / STATIC_BUNDLE_MANIFEST).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def validate_sources(xarm_root: Path, source_urdf: Path) -> None:
    require(xarm_root / "xarm_description" / "meshes", "xarm_description meshes")
    require(
        xarm_root / "xarm_moveit_config" / "srdf" / "_lite6_macro.srdf.xacro",
        "Lite6 SRDF macro",
    )
    require(xarm_root / MOVEIT_JOINT_LIMITS_SOURCE, "Lite6 MoveIt joint limits")
    require(source_urdf, "expanded Lite6 URDF")


def main() -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xarm-ros2", type=Path, required=True)
    parser.add_argument(
        "--source-urdf",
        type=Path,
        default=(
            root
            / "manyforge_example_models"
            / "models"
            / "lite6_lite_gripper"
            / "manymove_runtime_capture"
            / "manymove_lite6_runtime.urdf"
        ),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "manyforge_example_models" / "models" / "lite6_lite_gripper",
    )
    parser.add_argument(
        "--isaac-topic-based-urdf-output",
        type=Path,
        default=(
            root
            / "manyforge_ros_bringup"
            / "urdf"
            / "isaac_lite6_topic_based.ros2_control.urdf"
        ),
        help=(
            "Full-model Isaac topic_based_ros2_control URDF template to refresh. "
            "Use an empty string to skip."
        ),
    )
    args = parser.parse_args()

    xarm_root = args.xarm_ros2.resolve()
    source_urdf = args.source_urdf.resolve()
    model_dir = args.output.resolve()
    validate_sources(xarm_root, source_urdf)
    moveit_joint_limits_source = xarm_root / MOVEIT_JOINT_LIMITS_SOURCE
    moveit_joint_dynamics = load_moveit_joint_dynamics(
        moveit_joint_limits_source
    )

    model_dir.mkdir(parents=True, exist_ok=True)
    generated_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    copy_meshes(xarm_root / "xarm_description", model_dir)
    copy_moveit_joint_limits(moveit_joint_limits_source, model_dir)
    write_urdf(
        source_urdf,
        model_dir / "lite6_lite_gripper.urdf",
        moveit_joint_dynamics,
    )
    write_srdf(model_dir / "lite6_lite_gripper.srdf")
    if str(args.isaac_topic_based_urdf_output):
        isaac_urdf_output = args.isaac_topic_based_urdf_output.resolve()
        isaac_urdf_output.parent.mkdir(parents=True, exist_ok=True)
        write_isaac_topic_based_ros2_control_urdf(
            source_urdf, isaac_urdf_output, moveit_joint_dynamics
        )
    write_license(model_dir, xarm_root)
    write_readme(
        model_dir,
        xarm_root,
        source_urdf,
        generated_at,
        moveit_joint_dynamics,
    )
    write_static_manifest(model_dir, xarm_root, source_urdf, generated_at)

    print(
        textwrap.dedent(
            f"""\
            Wrote Lite6 model bundle:
              {model_dir}
            Source xarm commit:
              {run_git(xarm_root, 'rev-parse', 'HEAD')}
            """
        ).strip()
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
