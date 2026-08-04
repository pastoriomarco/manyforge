#!/usr/bin/env python3
"""Qualify retained URDF/SRDF bytes with consumer-independent parsers.

This is deliberately outside RoboPlan, Pinocchio, the collision engine, and
Composer.  urdfdom and yourdfpy parse the exact retained URDF as an ordinary
robot model; the small SRDF verifier below implements the public SRDF element
and reference rules needed by the retained Lite6 fixture without importing a
ManyForge compiler or consumer parser.
"""

from __future__ import annotations

import argparse
import hashlib
from io import BytesIO
import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
import subprocess
import sys
from typing import Any
import xml.etree.ElementTree as ET

from yourdfpy import URDF


_MANIFEST_API_VERSION = "manyforge.i3.retained-consumer-fixture.v3"
_MAX_DOCUMENT_BYTES = 16 * 1024 * 1024


class QualificationError(RuntimeError):
    """The retained model failed the independent standards qualification."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise QualificationError(message)


def _read_bounded_regular_file(path: Path, label: str) -> bytes:
    _require(path.is_file(), f"{label} is not a regular file: {path}")
    _require(not path.is_symlink(), f"{label} must not be a symlink: {path}")
    size = path.stat().st_size
    _require(size > 0, f"{label} is empty")
    _require(
        size <= _MAX_DOCUMENT_BYTES,
        f"{label} exceeds the {_MAX_DOCUMENT_BYTES}-byte test bound",
    )
    data = path.read_bytes()
    _require(len(data) == size, f"{label} changed while it was read")
    return data


def _sha256(data: bytes) -> str:
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _exact_keys(value: dict[str, Any], expected: set[str], label: str) -> None:
    actual = set(value)
    _require(
        actual == expected,
        f"{label} keys differ: missing={sorted(expected - actual)}, "
        f"extra={sorted(actual - expected)}",
    )


def _required_text(value: Any, label: str) -> str:
    _require(isinstance(value, str) and bool(value), f"{label} must be text")
    return value


def _parse_manifest(data: bytes) -> dict[str, Any]:
    try:
        decoded = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise QualificationError(f"fixture manifest is invalid JSON: {error}") from error
    _require(isinstance(decoded, dict), "fixture manifest root must be an object")
    _exact_keys(
        decoded,
        {
            "api_version",
            "model_instance_id",
            "retained_bundle_id",
            "planning_product_id",
            "viewer_product_id",
            "symbol_role_map_product_id",
            "control_domain_id",
            "control_product_id",
            "urdf_sha256",
            "srdf_sha256",
            "control_sha256",
            "symbol_rewrites",
            "coordinates",
            "control_binding_coordinate_maps",
            "target_role_maps",
        },
        "fixture manifest",
    )
    _require(
        decoded["api_version"] == _MANIFEST_API_VERSION,
        "fixture manifest API version differs",
    )
    _required_text(decoded["model_instance_id"], "model_instance_id")
    _required_text(decoded["retained_bundle_id"], "retained_bundle_id")
    _required_text(decoded["planning_product_id"], "planning_product_id")
    _required_text(decoded["viewer_product_id"], "viewer_product_id")
    _required_text(
        decoded["symbol_role_map_product_id"],
        "symbol_role_map_product_id",
    )
    _required_text(decoded["control_domain_id"], "control_domain_id")
    _required_text(decoded["control_product_id"], "control_product_id")
    _required_text(decoded["urdf_sha256"], "urdf_sha256")
    _required_text(decoded["srdf_sha256"], "srdf_sha256")
    _required_text(decoded["control_sha256"], "control_sha256")
    _require(isinstance(decoded["symbol_rewrites"], list), "symbol_rewrites must be an array")
    _require(isinstance(decoded["coordinates"], list), "coordinates must be an array")
    _require(
        isinstance(decoded["control_binding_coordinate_maps"], list)
        and len(decoded["control_binding_coordinate_maps"]) == 1,
        "control_binding_coordinate_maps must contain the selected target binding",
    )
    _require(
        isinstance(decoded["target_role_maps"], list)
        and len(decoded["target_role_maps"]) == 1,
        "target_role_maps must contain the selected logical target",
    )
    return decoded


def _parse_control_product(data: bytes, manifest: dict[str, Any]) -> dict[str, Any]:
    try:
        control = json.loads(data)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise QualificationError(f"control product is invalid JSON: {error}") from error
    _require(isinstance(control, dict), "control product root must be an object")
    _exact_keys(
        control,
        {
            "api_version",
            "control_domain_id",
            "control_domain_revision",
            "controller_topology_id",
            "topology_revision",
            "control_coordinator_endpoint_role",
            "containment_granularity",
            "implementation_profile",
            "desired_configuration",
            "bindings",
            "controllers",
            "activation_sets",
            "routes",
            "source_conformance",
        },
        "control product",
    )
    _require(
        control["api_version"] == "manyforge.control_domain_control.v1",
        "control product API version differs",
    )
    _require(
        control["control_domain_id"] == manifest["control_domain_id"],
        "control product belongs to another retained domain",
    )
    for field in (
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
        "control_coordinator_endpoint_role",
        "containment_granularity",
    ):
        _required_text(control[field], f"control.{field}")

    profile = control["implementation_profile"]
    _require(isinstance(profile, dict), "control implementation profile must be an object")
    _exact_keys(
        profile,
        {
            "implementation_profile_id",
            "implementation_profile_revision",
            "generated_controller_manager_name",
        },
        "control implementation profile",
    )
    _require(
        profile["implementation_profile_id"] == "manyforge.reference_ros2_control",
        "control product selects another implementation profile",
    )

    manifest_binding = manifest["control_binding_coordinate_maps"][0]
    expected_resources = {
        (resource["resource_role_id"], resource["resource_id"])
        for resource in manifest_binding["resources"]
    }
    bindings = control["bindings"]
    _require(isinstance(bindings, list) and len(bindings) == 1, "control product must contain one binding")
    binding = bindings[0]
    _require(isinstance(binding, dict), "control binding must be an object")
    _require(
        binding["control_binding_id"] == manifest_binding["control_binding_id"],
        "control product binding differs from the retained coordinate map",
    )
    for side in ("command_claims", "state_claims"):
        claims = binding[side]
        _require(isinstance(claims, list) and bool(claims), f"control binding {side} must be non-empty")
        observed_resources: set[tuple[str, str]] = set()
        observed_symbols: set[str] = set()
        for index, claim in enumerate(claims):
            _require(isinstance(claim, dict), f"{side}[{index}] must be an object")
            _exact_keys(
                claim,
                {"resource_role_id", "resource_id", "interface_name", "logical_interface_symbol"},
                f"{side}[{index}]",
            )
            observed_resources.add(
                (
                    _required_text(claim["resource_role_id"], f"{side}[{index}].resource_role_id"),
                    _required_text(claim["resource_id"], f"{side}[{index}].resource_id"),
                )
            )
            _require(claim["interface_name"] == "position", f"{side}[{index}] is not a position interface")
            symbol = _required_text(claim["logical_interface_symbol"], f"{side}[{index}].logical_interface_symbol")
            _require(symbol not in observed_symbols, f"{side} repeats logical symbol {symbol}")
            observed_symbols.add(symbol)
        _require(
            observed_resources == expected_resources,
            f"control binding {side} resources differ from the retained coordinate map",
        )

    controllers = control["controllers"]
    _require(isinstance(controllers, list) and len(controllers) == 1, "direct control fixture must contain one controller")
    controller = controllers[0]
    _require(controller["controller_role_id"] == "hardware", "direct controller role differs")
    implementation = controller["implementation"]
    _require(isinstance(implementation, dict), "controller implementation must be an object")
    _exact_keys(
        implementation,
        {"implementation_id", "implementation_revision", "implementation_type"},
        "controller implementation",
    )
    _require(
        implementation["implementation_id"]
        == "manyforge.ros2_control.joint_trajectory_controller.direct",
        "direct fixture selected the wrong controller implementation",
    )
    _require(
        implementation["implementation_type"]
        == "joint_trajectory_controller/JointTrajectoryController",
        "direct fixture selected the wrong controller plugin type",
    )

    routes = control["routes"]
    _require(isinstance(routes, list) and len(routes) == 1, "direct control fixture must contain one route")
    route = routes[0]
    _require(route["controller_role_ids"] == ["hardware"], "direct route changed controller order")
    _require(route["path_edges"] == [], "direct route unexpectedly contains a chain edge")
    endpoint_modes = {
        endpoint["endpoint_role_id"]: (
            endpoint["is_command_endpoint"],
            endpoint["is_state_endpoint"],
        )
        for endpoint in route["endpoints"]
    }
    _require(
        endpoint_modes == {"command": (True, False), "state": (False, True)},
        "control route endpoint roles differ from the retained binding",
    )
    desired = control["desired_configuration"]
    _require(desired["activation_set_id"] == "arm_direct", "direct desired activation differs")
    _require(
        desired["binding_routes"]
        == [
            {
                "control_binding_id": binding["control_binding_id"],
                "control_binding_revision": binding["control_binding_revision"],
                "control_route_id": route["control_route_id"],
            }
        ],
        "control desired route differs from its retained binding product",
    )
    conformance = control["source_conformance"]
    _require(isinstance(conformance, dict), "control source conformance must be an object")
    _exact_keys(conformance, {"api_version", "source_conformance_digest"}, "control source conformance")
    _require(
        conformance["api_version"] == "manyforge.control_source_conformance.v1",
        "control source-conformance API differs",
    )
    _required_text(conformance["source_conformance_digest"], "control source-conformance digest")
    return control


@dataclass(frozen=True)
class _UrdfJoint:
    name: str
    kind: str
    parent: str
    child: str
    mimic_joint: str | None
    mimic_multiplier: float
    mimic_offset: float


@dataclass(frozen=True)
class _UrdfFacts:
    robot_name: str
    root_link: str
    links: frozenset[str]
    joints: dict[str, _UrdfJoint]


def _run_urdfdom(checker: Path, urdf_path: Path, robot_name: str) -> None:
    _require(checker.is_file(), f"urdfdom checker is absent: {checker}")
    _require(os.access(checker, os.X_OK), f"urdfdom checker is not executable: {checker}")
    try:
        result = subprocess.run(
            [str(checker), str(urdf_path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise QualificationError(f"urdfdom checker could not run: {error}") from error
    _require(
        result.returncode == 0,
        "urdfdom rejected retained URDF: " + (result.stderr or result.stdout).strip(),
    )
    _require(
        f"robot name is: {robot_name}" in result.stdout,
        "urdfdom output did not identify the retained robot",
    )
    _require(
        "Successfully Parsed XML" in result.stdout,
        "urdfdom did not report a successful ordinary URDF parse",
    )


def _parse_urdf_with_yourdfpy(urdf_bytes: bytes) -> _UrdfFacts:
    try:
        model = URDF.load(BytesIO(urdf_bytes), load_meshes=False)
    except Exception as error:  # yourdfpy exposes several parser exception types.
        raise QualificationError(f"yourdfpy rejected retained URDF: {error}") from error

    robot_name = _required_text(model.robot.name, "URDF robot name")
    links = [link.name for link in model.robot.links]
    _require(all(isinstance(name, str) and name for name in links), "URDF has an unnamed link")
    _require(len(set(links)) == len(links), "URDF has duplicate link names")
    link_set = frozenset(links)
    _require(model.base_link in link_set, "URDF root link is not in its link table")

    joints: dict[str, _UrdfJoint] = {}
    child_links: set[str] = set()
    mimic_count = 0
    for parsed in model.robot.joints:
        name = _required_text(parsed.name, "URDF joint name")
        _require(name not in joints, f"URDF has duplicate joint {name}")
        parent = _required_text(parsed.parent, f"URDF joint {name} parent")
        child = _required_text(parsed.child, f"URDF joint {name} child")
        _require(parent in link_set, f"URDF joint {name} has unknown parent link")
        _require(child in link_set, f"URDF joint {name} has unknown child link")
        _require(parent != child, f"URDF joint {name} is a self edge")
        _require(child not in child_links, f"URDF link {child} has multiple parent joints")
        child_links.add(child)

        mimic_joint: str | None = None
        mimic_multiplier = 1.0
        mimic_offset = 0.0
        if parsed.mimic is not None:
            mimic_count += 1
            mimic_joint = _required_text(parsed.mimic.joint, f"URDF joint {name} mimic target")
            mimic_multiplier = float(parsed.mimic.multiplier)
            mimic_offset = float(parsed.mimic.offset)
            _require(
                math.isfinite(mimic_multiplier) and math.isfinite(mimic_offset),
                f"URDF joint {name} has non-finite mimic coefficients",
            )
        joints[name] = _UrdfJoint(
            name,
            _required_text(parsed.type, f"URDF joint {name} type"),
            parent,
            child,
            mimic_joint,
            mimic_multiplier,
            mimic_offset,
        )

    for joint in joints.values():
        if joint.mimic_joint is None:
            continue
        _require(joint.mimic_joint in joints, f"URDF joint {joint.name} has an unknown mimic target")
        _require(joint.mimic_joint != joint.name, f"URDF joint {joint.name} mimics itself")

    _require(len(link_set) > 1, "retained Lite6 URDF is unexpectedly empty")
    _require(len(joints) > 1, "retained Lite6 URDF has no useful kinematic tree")
    _require(mimic_count > 0, "retained Lite6 fixture no longer exercises mimic semantics")
    return _UrdfFacts(robot_name, model.base_link, link_set, joints)


def _manifest_symbol_sets(manifest: dict[str, Any]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for index, raw in enumerate(manifest["symbol_rewrites"]):
        _require(isinstance(raw, dict), f"symbol_rewrites[{index}] must be an object")
        _exact_keys(
            raw,
            {"compiled_name", "kind", "model_instance_id", "scope_name", "source_name"},
            f"symbol_rewrites[{index}]",
        )
        kind = _required_text(raw["kind"], f"symbol_rewrites[{index}].kind")
        name = _required_text(raw["compiled_name"], f"symbol_rewrites[{index}].compiled_name")
        _require(
            raw["model_instance_id"] == manifest["model_instance_id"],
            f"symbol_rewrites[{index}] crosses model instances",
        )
        _require(
            name not in result.setdefault(kind, set()),
            f"duplicate compiled {kind} symbol {name}",
        )
        result[kind].add(name)
    return result


def _qualify_coordinate_map(manifest: dict[str, Any], urdf: _UrdfFacts) -> None:
    coordinates: dict[str, dict[str, Any]] = {}
    for index, raw in enumerate(manifest["coordinates"]):
        _require(isinstance(raw, dict), f"coordinates[{index}] must be an object")
        _exact_keys(
            raw,
            {
                "compiled_joint_name",
                "mimic_multiplier",
                "mimic_offset",
                "mimic_source_compiled_joint_name",
                "model_instance_id",
                "source_joint_index",
                "source_joint_name",
            },
            f"coordinates[{index}]",
        )
        name = _required_text(raw["compiled_joint_name"], f"coordinates[{index}].compiled_joint_name")
        _require(name not in coordinates, f"duplicate coordinate {name}")
        _require(name in urdf.joints, f"coordinate {name} is absent from retained URDF")
        _require(
            raw["model_instance_id"] == manifest["model_instance_id"],
            f"coordinate {name} crosses model instances",
        )
        coordinates[name] = raw

    symbols = _manifest_symbol_sets(manifest)
    _require(
        urdf.links - {urdf.root_link} == symbols.get("link", set()),
        "retained URDF links differ from the compiler map plus cell root",
    )
    _require(
        set(coordinates) == symbols.get("joint", set()),
        "coordinate and compiled source-joint maps differ",
    )
    fixed_cell_joints = set(urdf.joints) - set(coordinates)
    _require(
        len(fixed_cell_joints) == 1,
        "retained URDF must add exactly one compiler-owned cell attachment joint",
    )
    only_fixed = urdf.joints[next(iter(fixed_cell_joints))]
    _require(only_fixed.kind == "fixed", "compiler-owned cell attachment is not fixed")
    _require(only_fixed.parent == urdf.root_link, "cell attachment does not begin at the URDF root")

    for name, raw in coordinates.items():
        parsed = urdf.joints[name]
        source = raw["mimic_source_compiled_joint_name"]
        _require(
            source is None or isinstance(source, str) and bool(source),
            f"coordinate {name} has an invalid mimic source",
        )
        multiplier = raw["mimic_multiplier"]
        offset = raw["mimic_offset"]
        _require(
            isinstance(multiplier, (int, float))
            and not isinstance(multiplier, bool)
            and math.isfinite(float(multiplier)),
            f"coordinate {name} has an invalid mimic multiplier",
        )
        _require(
            isinstance(offset, (int, float))
            and not isinstance(offset, bool)
            and math.isfinite(float(offset)),
            f"coordinate {name} has an invalid mimic offset",
        )
        _require(source == parsed.mimic_joint, f"coordinate {name} mimic target differs from URDF")
        if source is not None:
            _require(
                math.isclose(float(multiplier), parsed.mimic_multiplier, rel_tol=0.0, abs_tol=0.0)
                and math.isclose(float(offset), parsed.mimic_offset, rel_tol=0.0, abs_tol=0.0),
                f"coordinate {name} mimic coefficients differ from URDF",
            )


def _require_xml_attributes(element: ET.Element, required: set[str], optional: set[str], label: str) -> None:
    actual = set(element.attrib)
    _require(required <= actual, f"{label} is missing attributes {sorted(required - actual)}")
    _require(
        actual <= required | optional,
        f"{label} has unknown attributes {sorted(actual - required - optional)}",
    )
    for name in actual:
        _required_text(element.attrib[name], f"{label}.{name}")


def _require_no_mixed_text(element: ET.Element, label: str) -> None:
    _require(not (element.text or "").strip(), f"{label} has unexpected text")
    for child in element:
        _require(not (child.tail or "").strip(), f"{label} has unexpected child-tail text")


def _parse_finite_numbers(value: str, label: str) -> tuple[float, ...]:
    fields = value.split()
    _require(bool(fields), f"{label} has no numeric value")
    try:
        parsed = tuple(float(field) for field in fields)
    except ValueError as error:
        raise QualificationError(f"{label} is not numeric") from error
    _require(all(math.isfinite(item) for item in parsed), f"{label} contains a non-finite number")
    return parsed


def _parse_srdf_root(srdf_bytes: bytes) -> ET.Element:
    upper = srdf_bytes.upper()
    _require(b"<!DOCTYPE" not in upper and b"<!ENTITY" not in upper, "SRDF contains a DTD or entity declaration")
    try:
        root = ET.fromstring(srdf_bytes)
    except (ET.ParseError, UnicodeDecodeError) as error:
        raise QualificationError(f"SRDF is not well-formed XML: {error}") from error
    _require(root.tag == "robot", "SRDF root must be <robot>")
    return root


def _directed_chain_joints(urdf: _UrdfFacts, base: str, tip: str) -> set[str]:
    children: dict[str, list[_UrdfJoint]] = {}
    for joint in urdf.joints.values():
        children.setdefault(joint.parent, []).append(joint)
    stack: list[tuple[str, tuple[str, ...]]] = [(base, ())]
    while stack:
        link, path = stack.pop()
        if link == tip:
            return set(path)
        for joint in children.get(link, []):
            stack.append((joint.child, path + (joint.name,)))
    raise QualificationError(f"SRDF chain {base}->{tip} is not a directed URDF chain")


def _validate_srdf(srdf_bytes: bytes, urdf: _UrdfFacts, symbols: dict[str, set[str]]) -> tuple[int, int, int, int]:
    root = _parse_srdf_root(srdf_bytes)
    _require_xml_attributes(root, {"name"}, set(), "SRDF robot")
    _require(root.attrib["name"] == urdf.robot_name, "URDF and SRDF robot names differ")
    _require_no_mixed_text(root, "SRDF robot")

    groups: dict[str, ET.Element] = {}
    states: dict[str, ET.Element] = {}
    end_effectors: dict[str, ET.Element] = {}
    disabled_pairs: set[tuple[str, str]] = set()
    passive_joints: set[str] = set()

    for child in root:
        label = f"SRDF <{child.tag}>"
        _require(
            child.tag
            in {
                "group",
                "group_state",
                "end_effector",
                "virtual_joint",
                "passive_joint",
                "disable_collisions",
            },
            f"SRDF has unknown top-level element <{child.tag}>",
        )
        _require_no_mixed_text(child, label)
        if child.tag == "group":
            _require_xml_attributes(child, {"name"}, set(), label)
            name = child.attrib["name"]
            _require(name not in groups, f"SRDF has duplicate group {name}")
            groups[name] = child
        elif child.tag == "group_state":
            _require_xml_attributes(child, {"name", "group"}, set(), label)
            name = child.attrib["name"]
            _require(name not in states, f"SRDF has duplicate group_state {name}")
            states[name] = child
        elif child.tag == "end_effector":
            _require_xml_attributes(child, {"name", "parent_link", "group"}, {"parent_group"}, label)
            _require(len(child) == 0, f"{label} must not have children")
            name = child.attrib["name"]
            _require(name not in end_effectors, f"SRDF has duplicate end_effector {name}")
            end_effectors[name] = child
        elif child.tag == "virtual_joint":
            _require_xml_attributes(child, {"name", "type", "parent_frame", "child_link"}, set(), label)
            _require(len(child) == 0, f"{label} must not have children")
            _require(child.attrib["type"] in {"fixed", "floating", "planar"}, "SRDF virtual_joint has unknown type")
            _require(child.attrib["child_link"] in urdf.links, "SRDF virtual_joint has unknown child link")
        elif child.tag == "passive_joint":
            _require_xml_attributes(child, {"name"}, set(), label)
            _require(len(child) == 0, f"{label} must not have children")
            name = child.attrib["name"]
            _require(name in urdf.joints, f"SRDF passive_joint {name} is absent from URDF")
            _require(name not in passive_joints, f"SRDF has duplicate passive_joint {name}")
            passive_joints.add(name)
        else:
            _require_xml_attributes(child, {"link1", "link2"}, {"reason"}, label)
            _require(len(child) == 0, f"{label} must not have children")
            link1 = child.attrib["link1"]
            link2 = child.attrib["link2"]
            _require(
                link1 in urdf.links and link2 in urdf.links,
                "SRDF disabled-collision pair references an unknown link",
            )
            _require(link1 != link2, "SRDF disabled-collision pair is a self pair")
            pair = tuple(sorted((link1, link2)))
            _require(pair not in disabled_pairs, "SRDF has a duplicate disabled-collision pair")
            disabled_pairs.add(pair)

    _require(set(groups) == symbols.get("srdf_group", set()), "SRDF groups differ from compiler symbol map")
    _require(
        set(states) == symbols.get("srdf_group_state", set()),
        "SRDF group states differ from compiler symbol map",
    )
    _require(
        set(end_effectors) == symbols.get("srdf_end_effector", set()),
        "SRDF end effectors differ from compiler symbol map",
    )

    direct_group_joints: dict[str, set[str]] = {}
    subgroup_refs: dict[str, set[str]] = {}
    for name, group in groups.items():
        joints: set[str] = set()
        subgroup_refs[name] = set()
        for member in group:
            member_label = f"SRDF group {name} <{member.tag}>"
            _require(
                member.tag in {"joint", "link", "chain", "group"},
                f"SRDF group {name} has unknown member <{member.tag}>",
            )
            _require_no_mixed_text(member, member_label)
            _require(len(member) == 0, f"{member_label} must not have children")
            if member.tag in {"joint", "link", "group"}:
                _require_xml_attributes(member, {"name"}, set(), member_label)
                reference = member.attrib["name"]
                if member.tag == "joint":
                    _require(reference in urdf.joints, f"SRDF group {name} has unknown joint {reference}")
                    joints.add(reference)
                elif member.tag == "link":
                    _require(reference in urdf.links, f"SRDF group {name} has unknown link {reference}")
                else:
                    subgroup_refs[name].add(reference)
            else:
                _require_xml_attributes(member, {"base_link", "tip_link"}, set(), member_label)
                base = member.attrib["base_link"]
                tip = member.attrib["tip_link"]
                _require(base in urdf.links and tip in urdf.links, f"SRDF group {name} chain has an unknown endpoint")
                joints.update(_directed_chain_joints(urdf, base, tip))
        direct_group_joints[name] = joints

    for group, references in subgroup_refs.items():
        _require(references <= set(groups), f"SRDF group {group} references an unknown subgroup")

    expanded: dict[str, set[str]] = {}
    visiting: set[str] = set()

    def expand_group(name: str) -> set[str]:
        if name in expanded:
            return expanded[name]
        _require(name not in visiting, f"SRDF subgroup cycle reaches {name}")
        visiting.add(name)
        value = set(direct_group_joints[name])
        for subgroup in subgroup_refs[name]:
            value.update(expand_group(subgroup))
        visiting.remove(name)
        expanded[name] = value
        return value

    for name in groups:
        expand_group(name)

    for name, state in states.items():
        group_name = state.attrib["group"]
        _require(group_name in groups, f"SRDF group_state {name} references unknown group {group_name}")
        state_joints: set[str] = set()
        for member in state:
            member_label = f"SRDF group_state {name} <{member.tag}>"
            _require(member.tag == "joint", f"SRDF group_state {name} has non-joint child")
            _require_xml_attributes(member, {"name", "value"}, set(), member_label)
            _require_no_mixed_text(member, member_label)
            _require(len(member) == 0, f"{member_label} must not have children")
            joint = member.attrib["name"]
            _require(joint in urdf.joints, f"SRDF group_state {name} has unknown joint {joint}")
            _require(joint in expanded[group_name], f"SRDF group_state {name} joint {joint} is outside group")
            _require(joint not in state_joints, f"SRDF group_state {name} repeats joint {joint}")
            state_joints.add(joint)
            _parse_finite_numbers(member.attrib["value"], f"SRDF group_state {name} joint {joint}")

    for name, end_effector in end_effectors.items():
        group = end_effector.attrib["group"]
        _require(group in groups, f"SRDF end_effector {name} references unknown group {group}")
        _require(end_effector.attrib["parent_link"] in urdf.links, f"SRDF end_effector {name} has unknown parent link")
        parent_group = end_effector.attrib.get("parent_group")
        _require(parent_group is None or parent_group in groups, f"SRDF end_effector {name} has unknown parent group")

    _require(len(groups) >= 2, "retained Lite6 SRDF has insufficient semantic groups")
    _require(bool(states), "retained Lite6 SRDF has no group states")
    _require(bool(end_effectors), "retained Lite6 SRDF has no end effector")
    _require(bool(disabled_pairs), "retained Lite6 SRDF has no disabled-collision semantics")
    return len(groups), len(states), len(end_effectors), len(disabled_pairs)


def _expect_srdf_rejection(root: ET.Element, urdf: _UrdfFacts, symbols: dict[str, set[str]], label: str) -> None:
    try:
        _validate_srdf(ET.tostring(root, encoding="utf-8"), urdf, symbols)
    except QualificationError:
        return
    raise QualificationError(f"independent SRDF verifier accepted {label}")


def _self_check_srdf_rejections(srdf_bytes: bytes, urdf: _UrdfFacts, symbols: dict[str, set[str]]) -> None:
    unknown_group = _parse_srdf_root(srdf_bytes)
    group_state = unknown_group.find("group_state")
    _require(group_state is not None, "fixture lacks group_state for rejection self-check")
    group_state.set("group", "missing_compiled_group")
    _expect_srdf_rejection(unknown_group, urdf, symbols, "unknown group reference")

    unknown_link = _parse_srdf_root(srdf_bytes)
    disabled = unknown_link.find("disable_collisions")
    _require(disabled is not None, "fixture lacks disable_collisions for rejection self-check")
    disabled.set("link1", "missing_compiled_link")
    _expect_srdf_rejection(unknown_link, urdf, symbols, "unknown link reference")

    unknown_element = _parse_srdf_root(srdf_bytes)
    unknown_element.append(ET.Element("manyforge_private_view"))
    _expect_srdf_rejection(unknown_element, urdf, symbols, "consumer-private element")


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture-root", type=Path, required=True)
    parser.add_argument("--urdfdom-checker", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = _arguments()
    root = args.fixture_root.resolve(strict=True)
    manifest_bytes = _read_bounded_regular_file(root / "manifest.json", "fixture manifest")
    urdf_path = root / "retained-model.urdf"
    srdf_path = root / "retained-model.srdf"
    control_path = root / "retained-control.json"
    urdf_bytes = _read_bounded_regular_file(urdf_path, "retained URDF")
    srdf_bytes = _read_bounded_regular_file(srdf_path, "retained SRDF")
    control_bytes = _read_bounded_regular_file(control_path, "retained control product")
    manifest = _parse_manifest(manifest_bytes)
    _require(_sha256(urdf_bytes) == manifest["urdf_sha256"], "retained URDF digest differs from fixture manifest")
    _require(_sha256(srdf_bytes) == manifest["srdf_sha256"], "retained SRDF digest differs from fixture manifest")
    _require(
        _sha256(control_bytes) == manifest["control_sha256"],
        "retained control-product digest differs from fixture manifest",
    )
    control = _parse_control_product(control_bytes, manifest)

    urdf = _parse_urdf_with_yourdfpy(urdf_bytes)
    _run_urdfdom(args.urdfdom_checker.resolve(strict=True), urdf_path, urdf.robot_name)
    _require(
        _read_bounded_regular_file(urdf_path, "retained URDF") == urdf_bytes,
        "retained URDF changed while external parsers consumed it",
    )
    symbols = _manifest_symbol_sets(manifest)
    _qualify_coordinate_map(manifest, urdf)
    groups, states, end_effectors, disabled_pairs = _validate_srdf(srdf_bytes, urdf, symbols)
    _self_check_srdf_rejections(srdf_bytes, urdf, symbols)

    print(
        "retained Lite6 ordinary-model qualification passed: "
        f"links={len(urdf.links)} joints={len(urdf.joints)} "
        f"groups={groups} states={states} end_effectors={end_effectors} "
        f"disabled_pairs={disabled_pairs} "
        f"controllers={len(control['controllers'])}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (QualificationError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
