#!/usr/bin/env python3
"""Validate the one maintained N6 deployment/model-family authoring input."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Final

import yaml


API_VERSION: Final = "manyforge.maintained_deployment_sources.v1"
PROGRAM_API_VERSION: Final = "manyforge.program.v1"
_ID = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")
_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_EXPECTED_DEPLOYMENT_SOURCES: Final = frozenset(
    {
        "examples/assistant_modes_scene_authoring.deployment.yaml",
        "examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
        "examples/pick_and_place.deployment.yaml",
        "examples/pick_and_place_ur10e_robotiq.deployment.yaml",
        "examples/semantic_shape_pick.deployment.yaml",
        "examples/vision_guided_bin_pick.deployment.yaml",
        "examples/vision_guided_pose_pick.deployment.yaml",
    }
)
_EXPECTED_HISTORICAL_BLOBS: Final = {
    "examples/assistant_modes_scene_authoring.deployment.yaml": (
        "7149aba89619f3f1c426148ef2f2972cd0c730f0"
    ),
    "examples/lite6_lite_gripper_pick_and_place.deployment.yaml": (
        "d0141f0042f3b7af5850e651befb3e2550d907c0"
    ),
    "examples/pick_and_place.deployment.yaml": "bee699e23c42a7b8810dbf27b1f8bd261f514834",
    "examples/pick_and_place_ur10e_robotiq.deployment.yaml": (
        "f31c1af91814b9da7a1d121531c0c3c814001781"
    ),
    "examples/semantic_shape_pick.deployment.yaml": "115ab078116696847a9c037f0b20d5b3e39b1741",
    "examples/vision_guided_bin_pick.deployment.yaml": "332b8b6daa93cf4b0533e93a42736fd67b8654c5",
    "examples/vision_guided_pose_pick.deployment.yaml": "a65a0011ee0247cb3bb0281360fa4c46073bbf9d",
}
_EXPECTED_SPEED_MPS: Final = {
    source: 0.15 if "lite6_" in source else 0.25
    for source in _EXPECTED_DEPLOYMENT_SOURCES
}
_EXPECTED_OPEN_LOOP_SOURCES: Final = frozenset(
    {
        "examples/assistant_modes_scene_authoring.deployment.yaml",
        "examples/lite6_lite_gripper_pick_and_place.deployment.yaml",
        "examples/pick_and_place_ur10e_robotiq.deployment.yaml",
    }
)
_EXPECTED_MODEL_FAMILIES: Final = {
    "lite6_lite_gripper": {
        "frames": {
            "base": "link_base",
            "flange": "link_eef",
            "gripper_tcp": "link_tcp",
            "root": "world",
            "tcp": "link_tcp",
        },
        "groups": {"arm": "lite6", "gripper": "lite_gripper"},
        "resources": {
            "arm_joints": ("joint1", "joint2", "joint3", "joint4", "joint5", "joint6"),
            "fingers": ("right_finger_joint",),
        },
        "collisions": {
            "arm_collision": (
                "link1",
                "link2",
                "link3",
                "link4",
                "link5",
                "link6",
                "link_base",
            ),
            "gripper_collision": (
                "lite_gripper_base_link",
                "lite_gripper_left_finger",
                "lite_gripper_right_finger",
            ),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/lite6_lite_gripper/",
            "historical_srdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.srdf",
            "historical_urdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf",
            "resource_root": "manyforge_example_models/models/lite6_lite_gripper",
            "srdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.srdf",
            "srdf_projection_kind": "identity",
            "urdf_path": "manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf",
            "urdf_projection_kind": "identity",
        },
    },
    "ur10e_robotiq_2f_140": {
        "frames": {
            "base": "base_link",
            "flange": "flange",
            "gripper_tcp": "grasp_frame",
            "tcp": "tool0",
        },
        "groups": {"arm": "arm", "gripper": "gripper"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            ),
            "fingers": ("finger_joint",),
        },
        "collisions": {
            "arm_collision": (
                "base_link_inertia",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": (
                "left_inner_finger",
                "left_inner_finger_pad",
                "left_inner_knuckle",
                "left_outer_finger",
                "left_outer_knuckle",
                "right_inner_finger",
                "right_inner_finger_pad",
                "right_inner_knuckle",
                "right_outer_finger",
                "right_outer_knuckle",
                "robotiq_base_link",
            ),
        },
        "root": "base",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/ur10e_robotiq_2f_140/",
            "historical_srdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.srdf",
            "historical_urdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.urdf",
            "resource_root": "manyforge_example_models/models/ur10e_robotiq_2f_140",
            "srdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.urdf",
            "urdf_projection_kind": "remove_unsupported_collision_materials",
        },
    },
    "ur5_roboplan": {
        "frames": {
            "base": "base_link",
            "flange": "ee_link",
            "gripper_tcp": "tool0",
            "root": "world",
            "tcp": "tool0",
        },
        "groups": {"arm": "arm"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            )
        },
        "collisions": {
            "arm_collision": (
                "base_link",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": ("ee_link", "tool0"),
            "tool_collision": ("ee_link", "tool0"),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://roboplan_example_models/models/ur_robot_model/",
            "historical_srdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf",
            "historical_urdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf",
            "resource_root": ".docker/roboplan/roboplan_example_models/models/ur_robot_model",
            "srdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.urdf",
            "urdf_projection_kind": "remove_unused_xacro_namespace",
        },
    },
}


class SourceDescriptionError(ValueError):
    """The maintained authoring description is malformed or stale."""


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise SourceDescriptionError(f"duplicate JSON member {name!r}")
        result[name] = value
    return result


def _object(value: object, *, path: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise SourceDescriptionError(f"{path} must be an object")
    return value


def _array(value: object, *, path: str) -> list[Any]:
    if type(value) is not list:
        raise SourceDescriptionError(f"{path} must be an array")
    return value


def _exact(
    value: object,
    *,
    required: frozenset[str],
    path: str,
) -> dict[str, Any]:
    row = _object(value, path=path)
    missing = sorted(required - set(row))
    unknown = sorted(set(row) - required)
    if missing or unknown:
        raise SourceDescriptionError(
            f"{path} has wrong fields; missing={missing}, unknown={unknown}"
        )
    return row


def _text(value: object, *, path: str) -> str:
    if type(value) is not str or not value.strip() or len(value) > 4096:
        raise SourceDescriptionError(f"{path} must be bounded non-empty text")
    return value


def _identifier(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if _ID.fullmatch(text) is None:
        raise SourceDescriptionError(f"{path} must be a portable identifier")
    return text


def _source_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if candidate.is_absolute() or ".." in candidate.parts or text != candidate.as_posix():
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_file():
        raise SourceDescriptionError(f"{path} does not select a plain source file")
    return text


def _directory_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if candidate.is_absolute() or ".." in candidate.parts or text != candidate.as_posix():
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_dir():
        raise SourceDescriptionError(f"{path} does not select a plain source directory")
    return text


def _source_symbol(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if len(text) > 1024 or any(character.isspace() or not character.isprintable() for character in text):
        raise SourceDescriptionError(f"{path} must be one bounded source symbol")
    return text


def _canonical_unique(values: list[str], *, path: str, sort_values: bool = True) -> None:
    if len(values) != len(set(values)) or (sort_values and values != sorted(values)):
        raise SourceDescriptionError(f"{path} must be unique and canonical-sorted")


def _validate_model_semantics(
    value: object,
    *,
    family_id: str,
    path: str,
) -> None:
    semantics = _exact(
        value,
        required=frozenset(
            {
                "collision_body_role_bindings",
                "frame_role_bindings",
                "instance_cardinality",
                "model_group_role_bindings",
                "resource_role_bindings",
                "root_frame_role_id",
            }
        ),
        path=path,
    )
    if semantics["instance_cardinality"] != "multiple_per_cell":
        raise SourceDescriptionError(
            f"{path}.instance_cardinality must preserve multi-instance model use"
        )

    frame_roles: list[str] = []
    frame_bindings: dict[str, str] = {}
    for index, raw_binding in enumerate(
        _array(semantics["frame_role_bindings"], path=f"{path}.frame_role_bindings")
    ):
        binding_path = f"{path}.frame_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"frame_role_id", "source_link_name"}),
            path=binding_path,
        )
        frame_role = _identifier(
            binding["frame_role_id"], path=f"{binding_path}.frame_role_id"
        )
        frame_roles.append(frame_role)
        frame_bindings[frame_role] = _source_symbol(
            binding["source_link_name"], path=f"{binding_path}.source_link_name"
        )
    _canonical_unique(frame_roles, path=f"{path}.frame_role_bindings")

    group_roles: list[str] = []
    group_bindings: dict[str, str] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["model_group_role_bindings"],
            path=f"{path}.model_group_role_bindings",
        )
    ):
        binding_path = f"{path}.model_group_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"model_group_role_id", "source_group_name"}),
            path=binding_path,
        )
        group_role = _identifier(
            binding["model_group_role_id"],
            path=f"{binding_path}.model_group_role_id",
        )
        group_roles.append(group_role)
        group_bindings[group_role] = _source_symbol(
            binding["source_group_name"], path=f"{binding_path}.source_group_name"
        )
    _canonical_unique(group_roles, path=f"{path}.model_group_role_bindings")

    resource_roles: list[str] = []
    resource_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["resource_role_bindings"],
            path=f"{path}.resource_role_bindings",
        )
    ):
        binding_path = f"{path}.resource_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"resource_role_id", "source_joint_names"}),
            path=binding_path,
        )
        resource_role = _identifier(
            binding["resource_role_id"], path=f"{binding_path}.resource_role_id"
        )
        resource_roles.append(resource_role)
        joint_names = [
            _source_symbol(joint, path=f"{binding_path}.source_joint_names[{joint_index}]")
            for joint_index, joint in enumerate(
                _array(
                    binding["source_joint_names"],
                    path=f"{binding_path}.source_joint_names",
                )
            )
        ]
        if not joint_names or len(joint_names) != len(set(joint_names)):
            raise SourceDescriptionError(
                f"{binding_path}.source_joint_names must be non-empty and unique"
            )
        resource_bindings[resource_role] = tuple(joint_names)
    _canonical_unique(resource_roles, path=f"{path}.resource_role_bindings")

    collision_roles: list[str] = []
    collision_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["collision_body_role_bindings"],
            path=f"{path}.collision_body_role_bindings",
        )
    ):
        binding_path = f"{path}.collision_body_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"collision_body_role_id", "source_link_names"}),
            path=binding_path,
        )
        collision_role = _identifier(
            binding["collision_body_role_id"],
            path=f"{binding_path}.collision_body_role_id",
        )
        collision_roles.append(collision_role)
        link_names = [
            _source_symbol(link, path=f"{binding_path}.source_link_names[{link_index}]")
            for link_index, link in enumerate(
                _array(
                    binding["source_link_names"],
                    path=f"{binding_path}.source_link_names",
                )
            )
        ]
        if not link_names:
            raise SourceDescriptionError(f"{binding_path}.source_link_names must not be empty")
        _canonical_unique(link_names, path=f"{binding_path}.source_link_names")
        collision_bindings[collision_role] = tuple(link_names)
    _canonical_unique(collision_roles, path=f"{path}.collision_body_role_bindings")

    expected = _EXPECTED_MODEL_FAMILIES[family_id]
    root_frame_role = _identifier(
        semantics["root_frame_role_id"], path=f"{path}.root_frame_role_id"
    )
    if (
        frame_bindings != expected["frames"]
        or group_bindings != expected["groups"]
        or resource_bindings != expected["resources"]
        or collision_bindings != expected["collisions"]
        or root_frame_role != expected["root"]
        or root_frame_role not in frame_roles
    ):
        raise SourceDescriptionError(
            f"{path} changes the accepted source-local family role contract"
        )


def _program_roles(program_path: Path) -> tuple[set[str], dict[str, str]]:
    try:
        value = yaml.safe_load(program_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise SourceDescriptionError(
            f"cannot read Program source {program_path}: {error}"
        ) from error
    root = _object(value, path=str(program_path))
    if root.get("api_version") != PROGRAM_API_VERSION:
        raise SourceDescriptionError(f"{program_path} is not a direct v1 Program")
    component_roles = _object(root.get("component_roles"), path=f"{program_path}.component_roles")
    target_rows = _object(root.get("target_roles"), path=f"{program_path}.target_roles")
    target_roles: dict[str, str] = {}
    for name, raw_target in target_rows.items():
        target = _object(raw_target, path=f"{program_path}.target_roles.{name}")
        target_roles[name] = _identifier(
            target.get("component_role"),
            path=f"{program_path}.target_roles.{name}.component_role",
        )
    return set(component_roles), target_roles


def _validate_role_assignment(
    value: object,
    *,
    root: Path,
    component_occurrence_id: str,
    path: str,
) -> set[str]:
    row = _exact(
        value,
        required=frozenset({"program_source", "component_roles", "target_roles"}),
        path=path,
    )
    program_source = _source_path(row["program_source"], root=root, path=f"{path}.program_source")
    declared_components, declared_targets = _program_roles(root / program_source)
    components = _object(row["component_roles"], path=f"{path}.component_roles")
    if set(components) != declared_components:
        raise SourceDescriptionError(f"{path}.component_roles does not exactly cover the Program")
    for role, occurrence in components.items():
        _identifier(role, path=f"{path}.component_roles key")
        if occurrence != component_occurrence_id:
            raise SourceDescriptionError(
                f"{path}.component_roles.{role} must select the deployment occurrence"
            )

    targets = _object(row["target_roles"], path=f"{path}.target_roles")
    if set(targets) != set(declared_targets):
        raise SourceDescriptionError(f"{path}.target_roles does not exactly cover the Program")
    for role, raw_target in targets.items():
        target = _exact(
            raw_target,
            required=frozenset({"component_role", "command_target_id"}),
            path=f"{path}.target_roles.{role}",
        )
        component_role = _identifier(
            target["component_role"], path=f"{path}.target_roles.{role}.component_role"
        )
        _identifier(
            target["command_target_id"],
            path=f"{path}.target_roles.{role}.command_target_id",
        )
        if component_role != declared_targets[role]:
            raise SourceDescriptionError(
                f"{path}.target_roles.{role} changes the Program component role"
            )
    return set(targets)


def _validate_policy(value: object, *, target_roles: set[str], path: str) -> str:
    row = _object(value, path=path)
    kind = row.get("kind")
    if kind == "target_tcp_linear_speed_limit":
        row = _exact(
            row,
            required=frozenset(
                {
                    "kind",
                    "source_field",
                    "target_role_id",
                    "maximum_speed_mps",
                    "runtime_acceptance_step",
                    "acceptance_obligation_id",
                }
            ),
            path=path,
        )
        if row["source_field"] != "robot_config.global_max_tcp_linear_speed_mps":
            raise SourceDescriptionError(f"{path}.source_field is not the historical speed field")
        speed = row["maximum_speed_mps"]
        if type(speed) not in {int, float} or not math.isfinite(speed) or speed <= 0.0:
            raise SourceDescriptionError(f"{path}.maximum_speed_mps must be positive and finite")
        if (
            row["runtime_acceptance_step"] != "I8/P5"
            or row["acceptance_obligation_id"]
            != "retained_native_tcp_speed_limit"
        ):
            raise SourceDescriptionError(
                f"{path} changes the deferred speed owner/acceptance obligation"
            )
    elif kind == "open_loop_grasp_acknowledgement":
        row = _exact(
            row,
            required=frozenset(
                {
                    "kind",
                    "source_field",
                    "target_role_id",
                    "allowed",
                    "scope",
                    "rationale",
                    "runtime_acceptance_step",
                    "tool_acceptance_step",
                    "acceptance_obligation_id",
                }
            ),
            path=path,
        )
        if row["source_field"] != "grasp_risk_policy.open_loop_grasp":
            raise SourceDescriptionError(f"{path}.source_field is not the historical grasp field")
        if row["allowed"] is not True or row["scope"] != "simulation":
            raise SourceDescriptionError(f"{path} changes the historical open-loop scope")
        _text(row["rationale"], path=f"{path}.rationale")
        if (
            row["runtime_acceptance_step"] != "I8/P5"
            or row["tool_acceptance_step"] != "I11/T"
            or row["acceptance_obligation_id"] != "open_loop_grasp_scope_and_evidence"
        ):
            raise SourceDescriptionError(
                f"{path} changes the deferred grasp owner/acceptance obligation"
            )
    else:
        raise SourceDescriptionError(f"{path}.kind is outside the closed restoration union")
    target_role = _identifier(row["target_role_id"], path=f"{path}.target_role_id")
    if target_role not in target_roles:
        raise SourceDescriptionError(
            f"{path}.target_role_id is absent from every Program assignment"
        )
    return kind


def validate(source: Path, root: Path) -> None:
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_pairs
        )
    except (OSError, json.JSONDecodeError) as error:
        raise SourceDescriptionError(
            f"cannot read maintained source description: {error}"
        ) from error
    top = _exact(
        value,
        required=frozenset(
            {"api_version", "historical_source_commit", "model_families", "deployments"}
        ),
        path="$",
    )
    if top["api_version"] != API_VERSION:
        raise SourceDescriptionError(f"$.api_version must be {API_VERSION}")
    if (
        type(top["historical_source_commit"]) is not str
        or _HEX40.fullmatch(top["historical_source_commit"]) is None
    ):
        raise SourceDescriptionError("$.historical_source_commit must be a full Git object ID")

    family_ids: list[str] = []
    for index, raw_family in enumerate(_array(top["model_families"], path="$.model_families")):
        path = f"$.model_families[{index}]"
        family = _exact(
            raw_family,
            required=frozenset(
                {
                    "catalog_source",
                    "model_family_id",
                    "model_asset_id",
                    "semantics",
                    "source",
                }
            ),
            path=path,
        )
        family_id = _identifier(family["model_family_id"], path=f"{path}.model_family_id")
        family_ids.append(family_id)
        model_asset_id = _identifier(family["model_asset_id"], path=f"{path}.model_asset_id")
        if model_asset_id != family_id or family_id not in _EXPECTED_MODEL_FAMILIES:
            raise SourceDescriptionError(f"{path} changes the exact maintained model-family set")
        expected_catalog_source = (
            f"manyforge_deployment/resources/maintained/model_assets/"
            f"{model_asset_id}.model_asset.json"
        )
        catalog_source = _source_path(
            family["catalog_source"], root=root, path=f"{path}.catalog_source"
        )
        if catalog_source != expected_catalog_source:
            raise SourceDescriptionError(f"{path}.catalog_source is not the shared family source")
        _validate_model_semantics(
            family["semantics"], family_id=family_id, path=f"{path}.semantics"
        )
        model_source = _exact(
            family["source"],
            required=frozenset(
                {
                    "accepted_package_uri_prefix",
                    "historical_srdf_path",
                    "historical_urdf_path",
                    "resource_root",
                    "srdf_path",
                    "srdf_projection_kind",
                    "urdf_path",
                    "urdf_projection_kind",
                }
            ),
            path=f"{path}.source",
        )
        prefix = _text(
            model_source["accepted_package_uri_prefix"],
            path=f"{path}.source.accepted_package_uri_prefix",
        )
        if not prefix.startswith("package://") or not prefix.endswith("/"):
            raise SourceDescriptionError(f"{path}.source.accepted_package_uri_prefix is invalid")
        resource_root = _directory_path(
            model_source["resource_root"],
            root=root,
            path=f"{path}.source.resource_root",
        )
        srdf_path = _source_path(
            model_source["srdf_path"], root=root, path=f"{path}.source.srdf_path"
        )
        historical_srdf_path = _source_path(
            model_source["historical_srdf_path"],
            root=root,
            path=f"{path}.source.historical_srdf_path",
        )
        projection_kind = _text(
            model_source["srdf_projection_kind"],
            path=f"{path}.source.srdf_projection_kind",
        )
        expected_source = _EXPECTED_MODEL_FAMILIES[family_id]["source"]
        if projection_kind != expected_source["srdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.srdf_projection_kind changes the accepted projection"
            )
        if (projection_kind == "identity") != (srdf_path == historical_srdf_path):
            raise SourceDescriptionError(
                f"{path}.source SRDF projection paths contradict their declared kind"
            )
        urdf_path = _source_path(
            model_source["urdf_path"], root=root, path=f"{path}.source.urdf_path"
        )
        historical_urdf_path = _source_path(
            model_source["historical_urdf_path"],
            root=root,
            path=f"{path}.source.historical_urdf_path",
        )
        urdf_projection_kind = _text(
            model_source["urdf_projection_kind"],
            path=f"{path}.source.urdf_projection_kind",
        )
        if urdf_projection_kind != expected_source["urdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.urdf_projection_kind changes the accepted projection"
            )
        if (urdf_projection_kind == "identity") != (
            urdf_path == historical_urdf_path
        ):
            raise SourceDescriptionError(
                f"{path}.source URDF projection paths contradict their declared kind"
            )
        actual_source = {
            "accepted_package_uri_prefix": prefix,
            "historical_srdf_path": historical_srdf_path,
            "historical_urdf_path": historical_urdf_path,
            "resource_root": resource_root,
            "srdf_path": srdf_path,
            "srdf_projection_kind": projection_kind,
            "urdf_path": urdf_path,
            "urdf_projection_kind": urdf_projection_kind,
        }
        if actual_source != expected_source:
            raise SourceDescriptionError(
                f"{path}.source changes the accepted model-family source closure"
            )
    if (
        family_ids != sorted(family_ids)
        or len(set(family_ids)) != len(family_ids)
        or len(family_ids) != 3
    ):
        raise SourceDescriptionError(
            "$.model_families must contain the three canonical-sorted families"
        )

    deployment_sources: list[str] = []
    open_loop_sources: set[str] = set()
    speed_sources: set[str] = set()
    for index, raw_deployment in enumerate(_array(top["deployments"], path="$.deployments")):
        path = f"$.deployments[{index}]"
        row = _exact(
            raw_deployment,
            required=frozenset(
                {
                    "deployment_source",
                    "deployment_artifact_id",
                    "cell_id",
                    "historical_blob",
                    "model_family_id",
                    "component_occurrence_id",
                    "program_role_assignments",
                    "policy_restoration_inputs",
                }
            ),
            path=path,
        )
        deployment_source = _source_path(
            row["deployment_source"], root=root, path=f"{path}.deployment_source"
        )
        deployment_sources.append(deployment_source)
        _identifier(row["deployment_artifact_id"], path=f"{path}.deployment_artifact_id")
        _identifier(row["cell_id"], path=f"{path}.cell_id")
        if row["historical_blob"] != _EXPECTED_HISTORICAL_BLOBS[deployment_source]:
            raise SourceDescriptionError(
                f"{path}.historical_blob changes the pinned I0 source"
            )
        if row["model_family_id"] not in family_ids:
            raise SourceDescriptionError(f"{path}.model_family_id is unavailable")
        occurrence = _identifier(
            row["component_occurrence_id"], path=f"{path}.component_occurrence_id"
        )
        assignment_rows = _array(
            row["program_role_assignments"], path=f"{path}.program_role_assignments"
        )
        if not assignment_rows:
            raise SourceDescriptionError(f"{path}.program_role_assignments must not be empty")
        all_target_roles: set[str] = set()
        for assignment_index, assignment in enumerate(assignment_rows):
            all_target_roles.update(
                _validate_role_assignment(
                    assignment,
                    root=root,
                    component_occurrence_id=occurrence,
                    path=f"{path}.program_role_assignments[{assignment_index}]",
                )
            )
        policy_rows = _array(
            row["policy_restoration_inputs"], path=f"{path}.policy_restoration_inputs"
        )
        kinds: list[str] = []
        for policy_index, policy in enumerate(policy_rows):
            kind = _validate_policy(
                policy,
                target_roles=all_target_roles,
                path=f"{path}.policy_restoration_inputs[{policy_index}]",
            )
            kinds.append(kind)
            if (
                kind == "target_tcp_linear_speed_limit"
                and policy["maximum_speed_mps"]
                != _EXPECTED_SPEED_MPS[deployment_source]
            ):
                raise SourceDescriptionError(
                    f"{path}.policy_restoration_inputs[{policy_index}] "
                    "changes the historical speed"
                )
        if kinds.count("target_tcp_linear_speed_limit") != 1 or len(kinds) != len(set(kinds)):
            raise SourceDescriptionError(
                f"{path} must retain one speed input and no duplicate policy kind"
            )
        speed_sources.add(deployment_source)
        if "open_loop_grasp_acknowledgement" in kinds:
            open_loop_sources.add(deployment_source)

    if deployment_sources != sorted(deployment_sources) or len(
        set(deployment_sources)
    ) != len(deployment_sources):
        raise SourceDescriptionError("$.deployments must be unique and canonical-sorted")
    if (
        set(deployment_sources) != _EXPECTED_DEPLOYMENT_SOURCES
        or speed_sources != _EXPECTED_DEPLOYMENT_SOURCES
    ):
        raise SourceDescriptionError(
            "the exact seven maintained deployment/speed inputs are incomplete"
        )
    if open_loop_sources != _EXPECTED_OPEN_LOOP_SOURCES:
        raise SourceDescriptionError("the exact three historical open-loop inputs are incomplete")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.input.resolve(), args.workspace_root.resolve())
    except SourceDescriptionError as error:
        print(f"check_maintained_deployment_sources: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
