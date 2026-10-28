#!/usr/bin/env python3
"""Check one robot data folder offline before a ManyForge build (E1).

Usage (in the workbench container, from the repository root):

    python3 scripts/robots/check_robot.py manyforge_example_models/models/<family>
    python3 scripts/robots/check_robot.py <folder> --scenario examples/<name>.scenario

The folder is a model bundle: the URDF, SRDF and meshes one model family row
of `manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json`
names as its `source.resource_root`. The check joins that row, the robot's
Mock component profile rows, its Mock controller row
(`manyforge_ros_bringup/config/mock_controller_profiles.yaml`) and, with
`--scenario`, the Scenario's components of this robot. Every problem is
printed as `error: <where>: <what>`; the exit status is 1 when there is one.
Nothing runs and nothing is written.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

import mock_controller_profiles

DESCRIPTION = (
    "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
)
COMPONENT_TYPES = "manyforge_deployment/resources/maintained/catalog/component_type"
RUNTIME_SCHEMA = (
    "manyforge_deployment/resources/closed_schema/value/"
    "manyforge.value_schema.runtime.ros2_control_execution.v1.yaml"
)
REQUIRED_FRAME_ROLES = ("base", "flange", "tcp")
GENERATOR_HINT = (
    "run manyforge_deployment/tools/generate_maintained_deployment_occurrences.py "
    "--update (docs/authoring/ADDING_A_ROBOT.md, step 5)"
)


@dataclass
class Report:
    errors: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def error(self, where: str, what: str) -> None:
        self.errors.append(f"error: {where}: {what}")

    def note(self, what: str) -> None:
        self.notes.append(f"ok: {what}")


@dataclass
class Model:
    links: set[str]
    joints: dict[str, str]  # name -> type
    mimic_joints: set[str]
    mesh_uris: list[str]
    groups: set[str]
    group_states: dict[str, set[str]]  # group -> state names


def _xml(path: Path, report: Report, label: str) -> ElementTree.Element | None:
    try:
        return ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError) as error:
        report.error(label, f"cannot read {path}: {error}")
        return None


def _load_model(urdf: Path, srdf: Path, report: Report) -> Model | None:
    urdf_root = _xml(urdf, report, "URDF")
    srdf_root = _xml(srdf, report, "SRDF")
    if urdf_root is None or srdf_root is None:
        return None
    joints = {
        joint.get("name", ""): joint.get("type", "")
        for joint in urdf_root.iter("joint")
    }
    group_states: dict[str, set[str]] = {}
    for state in srdf_root.iter("group_state"):
        group_states.setdefault(state.get("group", ""), set()).add(state.get("name", ""))
    return Model(
        links={link.get("name", "") for link in urdf_root.iter("link")},
        joints=joints,
        mimic_joints={
            joint.get("name", "")
            for joint in urdf_root.iter("joint")
            if joint.find("mimic") is not None
        },
        mesh_uris=[mesh.get("filename", "") for mesh in urdf_root.iter("mesh")],
        groups={group.get("name", "") for group in srdf_root.iter("group")},
        group_states=group_states,
    )


def _roles(rows: list[dict[str, Any]], key: str, value: str) -> dict[str, Any]:
    return {row[key]: row[value] for row in rows if key in row and value in row}


def check_family(
    root: Path, family: dict[str, Any], report: Report
) -> tuple[Model | None, dict[str, Any]]:
    """The family row against its own URDF/SRDF; returns the model and roles."""
    family_id = family["model_family_id"]
    where = f"model family {family_id}"
    if family.get("model_asset_id") != family_id:
        report.error(where, "model_asset_id must equal model_family_id")
    source = family.get("source", {})
    prefix = source.get("accepted_package_uri_prefix", "")
    resource_root = root / source.get("resource_root", "")
    if not prefix.startswith("package://") or not prefix.endswith("/"):
        report.error(where, f"source.accepted_package_uri_prefix {prefix!r} must be "
                     "package://<package>/<path>/")
    for kind in ("urdf", "srdf"):
        if source.get(f"{kind}_projection_kind") == "identity" and source.get(
            f"historical_{kind}_path"
        ) != source.get(f"{kind}_path"):
            report.error(where, f"source.historical_{kind}_path must equal "
                         f"source.{kind}_path for an identity projection")
    urdf = root / source.get("urdf_path", "")
    srdf = root / source.get("srdf_path", "")
    model = _load_model(urdf, srdf, report)
    semantics = family.get("semantics", {})
    roles = {
        "frames": _roles(semantics.get("frame_role_bindings", []),
                         "frame_role_id", "source_link_name"),
        "groups": _roles(semantics.get("model_group_role_bindings", []),
                         "model_group_role_id", "source_group_name"),
        "resources": _roles(semantics.get("resource_role_bindings", []),
                            "resource_role_id", "source_joint_names"),
        "collisions": _roles(semantics.get("collision_body_role_bindings", []),
                             "collision_body_role_id", "source_link_names"),
    }
    if model is None:
        return None, roles
    for role in REQUIRED_FRAME_ROLES:
        if role not in roles["frames"]:
            report.error(where, f"semantics.frame_role_bindings has no {role!r} role")
    root_role = semantics.get("root_frame_role_id")
    if root_role not in roles["frames"]:
        report.error(where, f"root_frame_role_id {root_role!r} names no frame role")
    for role, link in roles["frames"].items():
        if link not in model.links:
            report.error(where, f"frame role {role!r}: link {link!r} is not in {urdf.name}")
    for role, links in roles["collisions"].items():
        for link in links:
            if link not in model.links:
                report.error(where, f"collision role {role!r}: link {link!r} is not "
                             f"in {urdf.name}")
    if "arm" not in roles["groups"]:
        report.error(where, "semantics.model_group_role_bindings has no 'arm' group")
    for role, group in roles["groups"].items():
        if group not in model.groups:
            report.error(where, f"group role {role!r}: group {group!r} is not in "
                         f"{srdf.name}")
    arm = roles["resources"].get("arm_joints")
    if not arm:
        report.error(where, "semantics.resource_role_bindings has no 'arm_joints'")
    fingers = roles["resources"].get("fingers")
    for role, joints in roles["resources"].items():
        for joint in joints:
            if joint not in model.joints:
                report.error(where, f"resource role {role!r}: joint {joint!r} is not "
                             f"in {urdf.name}")
            elif model.joints[joint] in ("fixed", "floating", "planar"):
                report.error(where, f"resource role {role!r}: joint {joint!r} is "
                             f"{model.joints[joint]}, not a commanded joint")
            elif joint in model.mimic_joints:
                report.error(where, f"resource role {role!r}: joint {joint!r} is a "
                             "mimic follower; name the joint it follows")
    if fingers is not None:
        if len(fingers) != 1:
            report.error(where, "resource role 'fingers' must name the single "
                         "commanded gripper joint")
        if "gripper" not in roles["groups"]:
            report.error(where, "a 'fingers' role needs a 'gripper' group role")
        else:
            states = model.group_states.get(roles["groups"]["gripper"], set())
            for state in ("open", "closed"):
                if state not in states:
                    report.error(where, f"SRDF group {roles['groups']['gripper']!r} "
                                 f"has no group_state {state!r}")
    for uri in model.mesh_uris:
        if not uri.startswith("package://"):
            report.error(where, f"mesh {uri!r} is not a package:// URI")
        elif not uri.startswith(prefix):
            report.error(where, f"mesh {uri!r} is outside {prefix}")
        elif not (resource_root / uri[len(prefix):]).is_file():
            report.error(where, f"mesh {uri!r} has no file under {resource_root}")
    for name in (f"{family_id}.component.json",):
        if not (root / COMPONENT_TYPES / name).is_file():
            report.error(where, f"{COMPONENT_TYPES}/{name} is missing; {GENERATOR_HINT}")
    if not report.errors:
        report.note(f"{where}: roles resolve in {urdf.name} and {srdf.name}, "
                    f"{len(model.mesh_uris)} mesh URIs resolve")
    return model, roles


def _schema_mock_enum(root: Path) -> set[str] | None:
    """The closed schema's `mock_profile` enum, or None when it has none."""

    def find(value: Any) -> set[str] | None:
        if isinstance(value, dict):
            if value.get("field_id") == "mock_profile":
                for constraint in value["value_schema"].get("constraints", []):
                    if constraint.get("kind") == "enum":
                        return set(constraint["values"])
                return None
            value = list(value.values())
        if isinstance(value, list):
            for item in value:
                found = find(item)
                if found is not None:
                    return found
        return None

    return find(yaml.safe_load((root / RUNTIME_SCHEMA).read_text(encoding="utf-8")))


def check_mock_lane(
    root: Path,
    family_id: str,
    roles: dict[str, Any],
    profiles: list[dict[str, Any]],
    report: Report,
) -> dict[str, str]:
    """Mock profile rows -> controller rows; returns profile id -> mock_profile."""
    try:
        rows = {
            row.mock_profile: row
            for row in mock_controller_profiles.load(
                root / mock_controller_profiles.DEFAULT_PATH
            )
        }
    except (OSError, mock_controller_profiles.MockControllerProfileError) as error:
        report.error(mock_controller_profiles.DEFAULT_PATH, str(error))
        return {}
    mock = {
        profile["runtime_restoration_profile_id"]: profile["ros2_control_runtime"]["mock_profile"]
        for profile in profiles
        if profile.get("model_family_id") == family_id
        and "mock_profile" in profile.get("ros2_control_runtime", {})
    }
    where = f"model family {family_id}"
    if not mock:
        report.error(where, "no component profile row selects a Mock controller row; "
                     "add {runtime_restoration_profile_id: <family>_mock_commanded_v0, "
                     "model_family_id, ros2_control_runtime: {mock_profile: <family>}, "
                     "source_fields: {}, stop_profiles: {}, acceptance_obligations: []}")
        return {}
    enum = _schema_mock_enum(root)
    for profile_id, mock_profile in mock.items():
        row = rows.get(mock_profile)
        if row is None:
            report.error(f"profile {profile_id}", f"mock_profile {mock_profile!r} has no "
                         f"row in {mock_controller_profiles.DEFAULT_PATH}")
            continue
        if "fingers" in roles["resources"] and row.gripper_controller is None:
            report.error(f"Mock controller row {mock_profile}", "the family has a "
                         "'fingers' role, so the row needs gripper_controller")
        if "fingers" not in roles["resources"] and row.gripper_controller is not None:
            report.error(f"Mock controller row {mock_profile}", "the family has no "
                         "'fingers' role; remove gripper_controller")
        if enum is not None and mock_profile not in enum:
            report.error(f"profile {profile_id}", f"mock_profile {mock_profile!r} is not "
                         f"in the mock_profile enum of {RUNTIME_SCHEMA}; Apply refuses "
                         "it until the id is accepted there")
    fixed = f"{family_id}.fixed_scale.component.json"
    if not (root / COMPONENT_TYPES / fixed).is_file():
        report.error(where, f"{COMPONENT_TYPES}/{fixed} is missing; {GENERATOR_HINT}")
    if not report.errors:
        report.note(f"{where}: Mock profiles {sorted(mock)} select controller rows "
                    f"{sorted(set(mock.values()))}")
    return mock


def check_scenario(
    scenario: Path,
    family_id: str,
    model: Model,
    roles: dict[str, Any],
    mock: dict[str, str],
    report: Report,
) -> None:
    """This robot's components in one Scenario's deployment.yaml."""
    try:
        deployment = yaml.safe_load((scenario / "deployment.yaml").read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        report.error(str(scenario), f"cannot read deployment.yaml: {error}")
        return
    components = [
        component
        for component in deployment.get("components", [])
        if component.get("component_profile_id") in mock
    ]
    if not components:
        report.error(str(scenario), f"no component selects a Mock profile of "
                     f"{family_id} ({sorted(mock)})")
        return
    arm = list(roles["resources"].get("arm_joints", []))
    fingers = roles["resources"].get("fingers")
    for component in components:
        component_id = component.get("component_id", "?")
        where = f"{scenario.name} component {component_id}"
        configuration = component.get("configuration", {})
        runtime = configuration.get("runtime_configuration", {})
        if runtime.get("mock_profile") != mock[component["component_profile_id"]]:
            report.error(where, f"runtime_configuration.mock_profile must be "
                         f"{mock[component['component_profile_id']]!r}, as its profile row")
        initial = runtime.get("initial_joint_positions_rad", [])
        if len(initial) != len(arm):
            report.error(where, f"initial_joint_positions_rad has {len(initial)} values "
                         f"for {len(arm)} arm joints {arm}")
        stop_joints = [
            row.get("source_joint_name")
            for row in component.get("target_settings", {})
            .get("arm", {})
            .get("controlled_stop", {})
            .get("per_joint_limits", [])
        ]
        if stop_joints != arm:
            report.error(where, f"target_settings.arm.controlled_stop.per_joint_limits "
                         f"must name the arm joints in order {arm}, got {stop_joints}")
        fault = set(
            configuration.get("default_fault_stop_profile", {})
            .get("per_joint_max_deceleration_rad_s2", {})
        )
        if not set(arm) <= fault:
            report.error(where, "default_fault_stop_profile.per_joint_max_deceleration_rad_s2 "
                         f"must name every arm joint {arm}")
        grippers = runtime.get("gripper_target_configurations", [])
        streams = sorted(row.get("stream_id") for row in runtime.get("component_streams", []))
        expected_streams = ["measured_joints"] + (["gripper_feedback"] if grippers else [])
        if streams != sorted(expected_streams):
            report.error(where, f"component_streams must declare exactly "
                         f"{sorted(expected_streams)}, got {streams}")
        if fingers and not grippers:
            report.error(where, "the family has a gripper ('fingers' role) but "
                         "gripper_target_configurations is empty")
        gripper_group = roles["groups"].get("gripper")
        for row in grippers:
            for key in ("tcp_frame", "attach_frame", "mount_frame"):
                if row.get(key) not in model.links:
                    report.error(where, f"gripper {key} {row.get(key)!r} is not a URDF link")
            for link in row.get("touch_links", []):
                if link not in model.links:
                    report.error(where, f"gripper touch link {link!r} is not a URDF link")
            for key in ("open_state_ref", "closed_state_ref"):
                if row.get(key) not in model.group_states.get(gripper_group, set()):
                    report.error(where, f"gripper {key} {row.get(key)!r} is not a "
                                 f"group_state of SRDF group {gripper_group!r}")
        limit = (
            component.get("target_settings", {}).get("arm", {})
            .get("maximum_tcp_linear_speed_mps")
        )
        if not isinstance(limit, (int, float)) or limit <= 0:
            report.error(where, "target_settings.arm.maximum_tcp_linear_speed_mps must be "
                         "a positive number at or below the vendor TCP limit")
    if not report.errors:
        report.note(f"{scenario.name}: {len(components)} component(s) of {family_id} agree "
                    "with the family and its Mock rows")


def check(
    root: Path, folder: Path, scenario: Path | None = None
) -> Report:
    report = Report()
    try:
        description = json.loads((root / DESCRIPTION).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        report.error(DESCRIPTION, f"cannot read: {error}")
        return report
    folder = folder.resolve()
    families = [
        family
        for family in description.get("model_families", [])
        if (root / family.get("source", {}).get("resource_root", "")).resolve() == folder
    ]
    if len(families) != 1:
        report.error(str(folder), f"{len(families)} model family rows in {DESCRIPTION} "
                     "name this folder as source.resource_root; add exactly one "
                     "(docs/authoring/ADDING_A_ROBOT.md, step 2)")
        return report
    family = families[0]
    model, roles = check_family(root, family, report)
    mock = check_mock_lane(
        root,
        family["model_family_id"],
        roles,
        description.get("runtime_restoration_profiles", []),
        report,
    )
    if scenario is not None and model is not None and mock:
        check_scenario(scenario, family["model_family_id"], model, roles, mock, report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Check one robot data folder (model bundle, family row, Mock rows)."
    )
    parser.add_argument("folder", type=Path, help="the model bundle folder")
    parser.add_argument("--scenario", type=Path, help="also check this Scenario folder")
    parser.add_argument(
        "--workspace-root", type=Path, default=Path(__file__).resolve().parents[2]
    )
    arguments = parser.parse_args(argv)
    report = check(arguments.workspace_root.resolve(), arguments.folder, arguments.scenario)
    for line in report.errors or report.notes:
        print(line)
    return 1 if report.errors else 0


if __name__ == "__main__":
    sys.exit(main())
