#!/usr/bin/env python3
"""Generate and verify the maintained catalog records.

It derives the repetitive catalog records (capability definitions, component
types, contracts, topologies, provider manifests, the runtime profile, Stop
policies) and the local site overlay from the maintained model families.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
from typing import Any, Final

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    load_strict_json_yaml_bytes,
)

from check_maintained_deployment_sources import (
    SourceDescriptionError,
    validate as validate_maintained_description,
)


DESCRIPTION_API: Final = "manyforge.maintained_deployment_sources.v1"
ASSISTANT_API: Final = "manyforge.maintained_assistant_profiles.v1"
CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1\0"
ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_ID: Final = (
    "manyforge.value_schema.runtime.ros2_control_execution.v1"
)
ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_REVISION: Final = (
    "sha256:1c557174fd45649c43d2e20239ec614635ca31aa79608d87727e35edcf8c2c90"
)
EMPTY_PROVIDER_CONTRACT_SCHEMA_ID: Final = (
    "manyforge.value_schema.provider_contract.empty.v1"
)
EMPTY_PROVIDER_CONTRACT_SCHEMA_REVISION: Final = (
    "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270"
)
MOTION_CONTROLLED_STOP_SCHEMA_ID: Final = (
    "manyforge.value_schema.provider_contract.motion_controlled_stop.v1"
)
MOTION_CONTROLLED_STOP_SCHEMA_REVISION: Final = (
    "sha256:65e6f4462981225916834490d4e3cb4d42b70611a2287e1b76fdb1cbcabcb113"
)
# R7 D12: the xArm-family arm and gripper lifecycle providers.
XARM_LIFECYCLE_PROVIDER_ID: Final = "xarm_arm_provider"
XARM_GRIPPER_LIFECYCLE_PROVIDER_ID: Final = "xarm_gripper_provider"
LIFECYCLE_INTENTS: Final = ("disable", "enable", "observe", "prepare", "recover")
ROS2_CONTROL_PROVIDER_ID: Final = "manyforge.provider.ros2_control"

CAPABILITIES: Final = {
    "io.gpio.set_output": (
        "sha256:b7f9e3ae6ed12dd59fd78193db902c6ad09528372ad0f5e2423f936833cb49f8",
        "target_binding",
        "capability_provider_binding",
    ),
    "io.gpio.get_input": (
        "sha256:abe7c9eed8b4b76ada84aa770ae3111691dfdd07001f4abd68a38bed56ed3d84",
        "target_binding",
        "capability_provider_binding",
    ),
    "io.gpio.wait_for_input": (
        "sha256:f5520f864f38d6f658039b8a338da9ef1fec014d70e9e354c5e40935717b4747",
        "target_binding",
        "capability_provider_binding",
    ),
    "assistant.bridge.request": (
        "sha256:221f74e397dcae43f8122e30f78cd1e11923b755abf51e70085bc30257f3def9",
        "cell_global",
        "capability_provider_binding",
    ),
    "assistant.model.invoke": (
        "sha256:f900f43ad7bbc61f49c09295a2a63e5fabe93eb57bc9c5f44c26e3c4eeeac158",
        "cell_global",
        "capability_provider_binding",
    ),
    "ai.model.invoke": (
        "sha256:c458ea512ad5b71be1a3a9f92908d8a36b2a9a8f5cb3b31556c42e0d468c9270",
        "target_binding",
        "capability_provider_binding",
    ),
    "component.lifecycle.disable": (
        "sha256:578cd871bc7248d18f1ccafe2659acb9d9ad35570c17b30758312d65b37fb4d1",
        "control_domain",
        "capability_provider_binding",
    ),
    "component.lifecycle.enable": (
        "sha256:c0d8704375f520e69f04dccbdbe1f8856bab2c0c185ea59cddc4f34f50cbc1f7",
        "control_domain",
        "capability_provider_binding",
    ),
    "component.lifecycle.observe": (
        "sha256:deb6815e681e8879c172e1a7dcd03a55154bc599efaa35167c9d23f54d6b9104",
        "control_domain",
        "capability_provider_binding",
    ),
    "component.lifecycle.prepare": (
        "sha256:c61d01109243632bd3a4184061863cee480a35f253aabac7719fea634e2defd3",
        "control_domain",
        "capability_provider_binding",
    ),
    "component.lifecycle.recover": (
        "sha256:7303e9db6f3c6d94b00ed8c448bb98abe156775f562e122cb8046799a7d41e9a",
        "control_domain",
        "capability_provider_binding",
    ),
    "execution.feedback.phase_events": (
        "sha256:37a4b6d145a96b0336ad03680c3222a63da28168764663cee7f5185fcbd467da",
        "target_binding",
        "control_binding",
    ),
    "execution.feedback.state_snapshots": (
        "sha256:bdcd8c70d34d34bba3735e6e1b191cade422d9b50223afed895465e0062be85a",
        "target_binding",
        "control_binding",
    ),
    "execution.stop.soft": (
        "sha256:3ed811803c31726c758e227fc52bd57854cc6e7fa09dff0fd117d41800d4f8b9",
        "target_binding",
        "control_binding",
    ),
    "execution.reset": (
        "sha256:24fa527d5e81814f8a395d1d3eeae3c3236c1249b05d84d2cd48abf3f2a27ebb",
        "target_binding",
        "control_binding",
    ),
    "execution.runtime_scale.operation_scoped": (
        "sha256:4ee00df7b3510c1490c83e247d12b73553ab53e4e6673da53ebea475fb756fad",
        "target_binding",
        "control_binding",
    ),
    "execution.contact.geometric": (
        "sha256:63a3d877d78d71a5932cce5c92d2932b740fbba6e88d00ca3b5dd94429e3eb3d",
        "target_binding",
        "control_binding",
    ),
    "execution.contact.geometric_signed_penetration": (
        "sha256:b2f0e892d401460c6b43b1c1b8b39a0cad5fcc130ba694c2c5e32d6cc2d8c323",
        "target_binding",
        "control_binding",
    ),
    "execution.trajectory.execute": (
        "sha256:7e5958db0906b84cb4e5c68ab0ef896f74dc0ba49a9ceb4d918b386abaf9f28c",
        "target_binding",
        "control_binding",
    ),
    "model.kinematics.fk": (
        "sha256:c99e2dc6d89129ca1d1d4a44d561d878b786df610d1ff241f6e09b5b100edb28",
        "target_binding",
        "capability_provider_binding",
    ),
    "model.limits.joint": (
        "sha256:4212f32bfc7e9b04cb11db023da86b9a8f75531b5d775674d559631d93b65053",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.motion.cartesian_path": (
        "sha256:be53c54ff0aa662db108113e9273e847e790d1b6f0adbba591d50f2237134a00",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.motion.joint_goal": (
        "sha256:0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.motion.pose_goal": (
        "sha256:422c7d9025d46e0fc660705a4f05171a235279872f91c820e952cf16792b8148",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.trajectory.time_parameterized": (
        "sha256:1fde55ff59f977844d1e42a115df0e66305953803e7f2830fec4821e3cd36385",
        "target_binding",
        "capability_provider_binding",
    ),
    "state.observed.current": (
        "sha256:c1ec4dacead10e33176830d634e9b2287e4b96322a54700c1c3910bcba461c02",
        "target_binding",
        "control_binding",
    ),
    "tool.gripper.command": (
        "sha256:a24b841e7c0323815a3576869d5819340bc6a98f1c2ffd5b47131e26698e9392",
        "target_binding",
        "control_binding",
    ),
    "tool.gripper.feedback": (
        "sha256:53cebcf32900bf3fab41aeb490a9176d3130ae53c1a289c954f1eb8d07738e23",
        "target_binding",
        "control_binding",
    ),
    "validation.trajectory.non_collision": (
        "sha256:3cd04895527038906ddcb9c6ca9fd87f4535eba41e5014b04917c66603f26653",
        "target_binding",
        "capability_provider_binding",
    ),
    "scene.pose.observe": (
        "sha256:7ce48309e3a8f884850dd1285cbd66e19da631fbbfd27d269828fb7048832a57",
        "target_binding",
        "capability_provider_binding",
    ),
    "scene.reference.resolve": (
        "sha256:4f66fab4656567598a11844b3b2572112b5239d9eaca4a538f90e082db7154d9",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.object.query": (
        "sha256:59bad10b1c6eea73c493dbba88cf15befe3345d32bbebd0b7c7fce4947531109",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.shape.box": (
        "sha256:76e9a3ca351e30e271ec75e3b6de1ea42a718f81cca35cda6bab4ee642e1e12d",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.shape.sphere": (
        "sha256:d84d92a0e0d9592ead27340278315b3a691c18a902fec44bc99626f36d9bc8f2",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.shape.cylinder": (
        "sha256:4ffc860d1851db9b3a7fd82f5e9bd88eb1eca624dba372852fb2912fab9e34d5",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.shape.mesh": (
        "sha256:5ea0a60f5d788c2133419b84f110f0edc033405c7bfd7385086888f09bd2779e",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.edit.add": (
        "sha256:9f7fbd7203abe565edfb6468d9e52fa15c028f3636615eb62a8d5e8e0c84fea5",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.edit.upsert": (
        "sha256:01668536a12a75af7024ce6dd1248ce5fd18f25f75d1fadfb953b4b7da0ed1f2",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.edit.remove": (
        "sha256:c19f72293c0b9f54b24e8f165b27112e3ad8496e1d319f0d20d74b7eee6951bc",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.edit.update_pose": (
        "sha256:2289472ab0392c94e512d8bfe65406ef7a7ffafb73b40381bd36f627248de4f3",
        "cell_global",
        "capability_provider_binding",
    ),
    "scene.edit.attach_detach": (
        "sha256:444c80303025e78b49df614d71c7df35d0f40867cb0901a8bd0117e65d50eed2",
        "cell_global",
        "capability_provider_binding",
    ),
    "collision.query.pre_execution": (
        "sha256:c45cdcea2f79fbef5d1bc95db064078560c7d5d161b72667a5ce2c72a7432eca",
        "cell_global",
        "capability_provider_binding",
    ),
    "collision.query.detailed": (
        "sha256:59cfd352c4a08e315f95c2cb93ada8d2a16a6d2155f83b1c7603e9643088f73b",
        "cell_global",
        "capability_provider_binding",
    ),
    "observability.trace.scene": (
        "sha256:aa079df5821ad6ad0fdfc15f15368ca6682cd17dd1c804998e4e954fe1979180",
        "cell_global",
        "capability_provider_binding",
    ),
}

ARM_CAPABILITIES: Final = tuple(
    sorted(
        {
            "execution.feedback.phase_events",
            "execution.feedback.state_snapshots",
            "execution.stop.soft",
            "execution.reset",
            "execution.contact.geometric",
            "execution.contact.geometric_signed_penetration",
            "execution.trajectory.execute",
            "model.kinematics.fk",
            "model.limits.joint",
            "planning.motion.cartesian_path",
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.trajectory.time_parameterized",
            "state.observed.current",
            "validation.trajectory.non_collision",
        }
    )
)
# The historical closure is the original 36 facilities less the removed
# operation Pause/Resume (maintainer, 2026-10-08). Scaling is an
# additional native-qualified realization, not an unqualified ROS claim.
NATIVE_ARM_CAPABILITIES: Final = tuple(
    sorted((*ARM_CAPABILITIES, "execution.runtime_scale.operation_scoped"))
)
GRIPPER_CAPABILITIES: Final = tuple(
    sorted(
        {
            "model.kinematics.fk",
            "model.limits.joint",
            "tool.gripper.command",
            "tool.gripper.feedback",
        }
    )
)

CELL_CAPABILITIES: Final = tuple(
    sorted(
        {
            "collision.query.detailed",
            "collision.query.pre_execution",
            "observability.trace.scene",
            "scene.edit.add",
            "scene.edit.attach_detach",
            "scene.edit.remove",
            "scene.edit.update_pose",
            "scene.edit.upsert",
            "scene.object.query",
            "scene.reference.resolve",
            "scene.shape.box",
            "scene.shape.cylinder",
            "scene.shape.mesh",
            "scene.shape.sphere",
        }
    )
)



SCHEMA_SOURCES: Final = {
    "capability_definition": "manyforge_deployment/resources/closed_schema/catalog/manyforge.capability_definition.v1.yaml",
    "command_schema": "manyforge_deployment/resources/closed_schema/catalog/manyforge.command_schema.v1.yaml",
    "component_type": "manyforge_deployment/resources/closed_schema/catalog/manyforge.component_type.v1.yaml",
    "control_interface_contract": "manyforge_deployment/resources/closed_schema/catalog/manyforge.control_interface_contract.v1.yaml",
    "controller_topology": "manyforge_deployment/resources/closed_schema/catalog/manyforge.controller_topology.v1.yaml",
    "model_asset": "manyforge_deployment/resources/closed_schema/catalog/manyforge.model_asset.v1.yaml",
    "provider_manifest": "manyforge_deployment/resources/closed_schema/catalog/manyforge.provider_manifest.v1.yaml",
    "runtime_profile": "manyforge_deployment/resources/closed_schema/catalog/manyforge.runtime_profile.v1.yaml",
    "stop_evidence_policy": "manyforge_deployment/resources/closed_schema/catalog/manyforge.stop_evidence_policy.v2.yaml",
}


class GenerationError(ValueError):
    """The maintained inputs or generated projection are inconsistent."""


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    ).encode("utf-8")


def pretty_bytes(value: object) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")


def sha256_digest(payload: bytes) -> str:
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def semantic_revision(
    value: dict[str, Any], *, domain: str, api_version: str, revision_field: str
) -> str:
    identity = copy.deepcopy(value)
    identity.pop(revision_field, None)
    preimage = (
        CONTENT_IDENTITY_PREFIX
        + domain.encode("ascii")
        + b"\0"
        + api_version.encode("ascii")
        + b"\0"
        + canonical_bytes(identity)
    )
    return sha256_digest(preimage)


def revisioned_document(
    value: dict[str, Any], *, domain: str, api_version: str, revision_field: str
) -> dict[str, Any]:
    result = copy.deepcopy(value)
    result[revision_field] = semantic_revision(
        result,
        domain=domain,
        api_version=api_version,
        revision_field=revision_field,
    )
    return result


def read_json(path: Path, label: str) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise GenerationError(f"{label} is not one plain file: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GenerationError(f"cannot read {label}: {error}") from error
    if type(value) is not dict:
        raise GenerationError(f"{label} root must be an object")
    return value


def read_yaml(path: Path, label: str) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise GenerationError(f"{label} is not one plain file: {path}")
    # JSON-model scalars only: YAML 1.1 typing reads JSON numbers such as
    # -4e-05 as strings and would copy them into generated Programs.
    try:
        return load_strict_json_yaml_bytes(path.read_bytes(), source=str(path))
    except (OSError, StrictArtifactError) as error:
        raise GenerationError(f"cannot read {label}: {error}") from error


def replace_file(path: Path, payload: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.is_symlink():
        raise GenerationError(f"refusing to replace symbolic link: {path}")
    temporary = path.with_name(f".{path.name}.manyforge-{os.getpid()}.tmp")
    if temporary.exists():
        raise GenerationError(f"refusing to replace existing temporary: {temporary}")
    try:
        temporary.write_bytes(payload)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def require_or_update(path: Path, expected: bytes, update: bool, label: str) -> None:
    if update:
        replace_file(path, expected)
        return
    if path.is_symlink() or not path.is_file() or path.read_bytes() != expected:
        raise GenerationError(f"{label} is absent or stale: {path}")


def require_obsolete_output_absent(path: Path, *, update: bool) -> None:
    """Remove only a named repository-owned output replaced by this generator."""

    if not path.exists():
        return
    if path.is_symlink() or not path.is_file():
        raise GenerationError(f"obsolete generated output is not a plain file: {path}")
    if not update:
        raise GenerationError(f"obsolete generated output remains: {path}")
    path.unlink()


def schema_revisions(root: Path) -> dict[str, str]:
    revisions: dict[str, str] = {}
    for kind, relative in SCHEMA_SOURCES.items():
        source = read_yaml(root / relative, f"{kind} closed schema")
        revision = source.get("schema_revision")
        if type(revision) is not str or not revision.startswith("sha256:"):
            raise GenerationError(f"{kind} closed schema has no exact revision")
        revisions[kind] = revision
    return revisions


def catalog_reference(
    *,
    kind: str,
    record_id: str,
    record_revision: str,
    schema_id: str,
    schema_revision: str,
    payload: bytes,
) -> dict[str, str]:
    return {
        "record_kind": kind,
        "record_id": record_id,
        "record_revision": record_revision,
        "schema_id": schema_id,
        "schema_revision": schema_revision,
        "content_digest": sha256_digest(payload),
        "content_addressed_store_key": (
            f"catalog/maintained/{kind}/{record_id}/{record_revision[7:]}"
        ),
    }


def generated_catalog_document(
    *,
    root: Path,
    relative_path: str,
    kind: str,
    record_id: str,
    record_revision: str,
    schema_id: str,
    schema_revision: str,
    value: dict[str, Any],
    canonical_output: bool = False,
) -> tuple[Path, bytes, dict[str, str]]:
    payload = canonical_bytes(value) if canonical_output else pretty_bytes(value)
    return (
        root / relative_path,
        payload,
        catalog_reference(
            kind=kind,
            record_id=record_id,
            record_revision=record_revision,
            schema_id=schema_id,
            schema_revision=schema_revision,
            payload=payload,
        ),
    )


def capability_document(capability_id: str) -> dict[str, Any]:
    revision, scope, binding_kind = CAPABILITIES[capability_id]
    value = {
        "api_version": "manyforge.capability_definition.v1",
        "capability_id": capability_id,
        "scope": scope,
        "provider_binding_kind": binding_kind,
    }
    computed = semantic_revision(
        value,
        domain="catalog-record-revision/capability_definition",
        api_version="manyforge.capability_definition.v1",
        revision_field="capability_definition_revision",
    )
    if computed != revision:
        raise GenerationError(
            f"capability revision drift for {capability_id}: {computed} != {revision}"
        )
    value["capability_definition_revision"] = revision
    return value


def capability_selector(capability_id: str) -> dict[str, str]:
    return {
        "capability_id": capability_id,
        "capability_definition_revision": CAPABILITIES[capability_id][0],
    }


def component_type_document(
    family: dict[str, Any], *, runtime_scaling: bool = True
) -> dict[str, Any]:
    """Return a component type of one model family.

    Merge 6: a family without arm joints is a tool (its `gripper` target
    only, mounted through its root interface `mount`); a robot without
    gripper geometry offers its `flange` to one tool.
    """

    family_id = family["model_family_id"]
    semantics = family["semantics"]
    if not source_joint_names(family, "arm_joints"):
        return tool_component_type_document(family, runtime_scaling=runtime_scaling)
    collision_roles = {
        row["collision_body_role_id"]
        for row in semantics["collision_body_role_bindings"]
    }
    frame_roles = {row["frame_role_id"] for row in semantics["frame_role_bindings"]}
    # A robot whose model carries no gripper geometry mounts its tool as a
    # separate component on `flange`; its `tcp` is then the tool's (D1).
    offers_flange = "gripper_collision" not in collision_roles
    arm_collision_roles = [
        {
            "collision_body_role_id": role,
            "model_role_id": "arm_model",
            "asset_collision_body_role_id": role,
        }
        for role in ("arm_collision", "tool_collision")
        if role in collision_roles
    ]
    value = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": (
            f"{family_id}{'' if runtime_scaling else '.fixed_scale'}.component"
        ),
        "tags": ["articulated-arm"],
        "model_roles": [{"model_role_id": "arm_model", "cardinality": "exactly_one"}],
        "component_root": {
            "model_role_id": "arm_model",
            "frame_role_id": semantics["root_frame_role_id"],
        },
        "model_mounts": [],
        "command_targets": [
            {
                "command_target_id": "arm",
                "model_group_roles": [
                    {
                        "model_group_role_id": "planning_group",
                        "model_role_id": "arm_model",
                        "asset_model_group_role_id": "arm",
                    }
                ],
                "resource_roles": [
                    {
                        "resource_role_id": "arm_joints",
                        "source_kind": "model_role",
                        "model_role_id": "arm_model",
                        "asset_resource_role_id": "arm_joints",
                    }
                ],
                "frame_roles": [
                    {
                        "frame_role_id": role,
                        "model_role_id": "arm_model",
                        "asset_frame_role_id": role,
                    }
                    for role in (
                        ("base", "flange", "tcp") if offers_flange else ("base", "tcp")
                    )
                    if role in frame_roles
                ],
                "collision_body_roles": arm_collision_roles,
                "capabilities": [
                    capability_selector(capability_id)
                    for capability_id in (
                        NATIVE_ARM_CAPABILITIES if runtime_scaling else ARM_CAPABILITIES
                    )
                ],
            },
        ],
        "attachment_interfaces": (
            [
                {
                    "attachment_interface_id": "flange",
                    "model_role_id": "arm_model",
                    "frame_role_id": "flange",
                    "accepts_tags": ["tool"],
                    "max_attachments": 1,
                }
            ]
            if offers_flange
            else []
        ),
    }
    with_tcps(value, family)
    return revisioned_document(
        value,
        domain="catalog-record-revision/component_type",
        api_version="manyforge.component_type.v1",
        revision_field="component_type_revision",
    )


def tool_component_type_document(
    family: dict[str, Any], *, runtime_scaling: bool = True
) -> dict[str, Any]:
    """Return the component type of a tool family (merge 6).

    One `gripper` target on model role `tool_model`; the component root and
    the `mount` interface are the tool's root frame, the robot flange plane.
    The target exposes each TCP of the family (``tcp_frame_role_ids``, else
    ``tcp``) under its own name; the first is the default.
    """

    family_id = family["model_family_id"]
    semantics = family["semantics"]
    collision_roles = {
        row["collision_body_role_id"]
        for row in semantics["collision_body_role_bindings"]
    }
    root = semantics["root_frame_role_id"]
    contact = (
        {"contact_body_role_ids": ["gripper_fingers"]}
        if "gripper_fingers" in collision_roles
        else {}
    )
    value = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": (
            f"{family_id}{'' if runtime_scaling else '.fixed_scale'}.component"
        ),
        "tags": ["gripper", "tool"],
        "model_roles": [{"model_role_id": "tool_model", "cardinality": "exactly_one"}],
        "component_root": {"model_role_id": "tool_model", "frame_role_id": root},
        "model_mounts": [],
        "command_targets": [
            {
                "command_target_id": "gripper",
                "model_group_roles": [
                    {
                        "model_group_role_id": "planning_group",
                        "model_role_id": "tool_model",
                        "asset_model_group_role_id": "gripper",
                    }
                ],
                "resource_roles": [
                    {
                        "resource_role_id": "fingers",
                        "source_kind": "model_role",
                        "model_role_id": "tool_model",
                        "asset_resource_role_id": "fingers",
                    }
                ],
                "frame_roles": [
                    {
                        "frame_role_id": role,
                        "model_role_id": "tool_model",
                        "asset_frame_role_id": role,
                        **contact,
                    }
                    # A registered tool records its TCPs; a maintained one has `tcp`.
                    for role in family.get("tcp_frame_role_ids", ["tcp"])
                ],
                "collision_body_roles": [
                    {
                        "collision_body_role_id": role,
                        "model_role_id": "tool_model",
                        "asset_collision_body_role_id": role,
                    }
                    for role in ("gripper_collision", "gripper_fingers")
                    if role in collision_roles
                ],
                "capabilities": [
                    capability_selector(capability_id)
                    for capability_id in GRIPPER_CAPABILITIES
                ],
            }
        ],
        "attachment_interfaces": [
            {
                "attachment_interface_id": "mount",
                "model_role_id": "tool_model",
                "frame_role_id": root,
                "accepts_tags": ["articulated-arm"],
                "max_attachments": 1,
            }
        ],
    }
    # TCP-A (merge 6): tag the tool's TCPs and its default.
    with_tcps(value, family)
    return revisioned_document(
        value,
        domain="catalog-record-revision/component_type",
        api_version="manyforge.component_type.v1",
        revision_field="component_type_revision",
    )


def with_tcps(value: dict[str, Any], family: dict[str, Any]) -> None:
    """Tag the TCPs of one component type (merge 6, TCP-A).

    The rows are rules over the family's frame roles:

    - ``arm`` (the motion target) exposes the asset's ``flange`` and tags it
      and its ``tcp`` (when exposed) as TCPs; the default is ``tcp``, else
      ``flange``. With a tool mounted, D1 still resolves ``{arm, tcp}`` to the
      tool's default TCP.
    - ``gripper`` tags its ``tcp``, its default.
    - An asset offset frame role (``pose_in_source_link``) is a TCP of the
      family's first target (``arm`` on a robot, ``gripper`` on a tool).
    - A family that records its TCPs (``tcp_frame_role_ids``, from a component
      descriptor's ``tcps``) exposes and tags each of them on its first target
      under its own name; the first is the default.
    """

    semantics = family["semantics"]
    declared = family.get("tcp_frame_role_ids")
    asset_roles = {row["frame_role_id"] for row in semantics["frame_role_bindings"]}
    offsets = sorted(
        row["frame_role_id"]
        for row in semantics["frame_role_bindings"]
        if "pose_in_source_link" in row
    )
    for index, target in enumerate(value["command_targets"]):
        rows = target["frame_roles"]
        model_role = (
            rows[0]["model_role_id"] if rows else value["model_roles"][0]["model_role_id"]
        )
        exposed = {row["frame_role_id"] for row in rows}
        wanted = set(offsets) if index == 0 else set()
        if index == 0 and declared is not None:
            wanted.update(declared)
        if target["command_target_id"] == "arm" and "flange" in asset_roles:
            wanted.add("flange")
        for role in sorted(wanted - exposed):
            rows.append(
                {"frame_role_id": role, "model_role_id": model_role, "asset_frame_role_id": role}
            )
        tagged = wanted | ({"tcp"} & {row["frame_role_id"] for row in rows})
        for row in rows:
            if row["frame_role_id"] in tagged:
                row["tcp"] = True
        rows.sort(key=lambda row: row["frame_role_id"])
        if tagged:
            first = (declared[0],) if index == 0 and declared is not None else ()
            target["default_tcp_frame_role_id"] = next(
                role
                for role in (*first, "tcp", "flange", *sorted(tagged))
                if role in tagged
            )




def control_contract(target: str) -> dict[str, Any]:
    role = "arm_joints" if target == "arm" else "fingers"
    value = {
        "api_version": "manyforge.control_interface_contract.v1",
        "control_interface_contract_id": f"manyforge.ros2_control_{target}_position",
        "command_interfaces": [
            {"resource_role_id": role, "interface_name": "position"}
        ],
        "state_interfaces": [{"resource_role_id": role, "interface_name": "position"}],
        "ordering": [role],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/control_interface_contract",
        api_version="manyforge.control_interface_contract.v1",
        revision_field="control_interface_revision",
    )


def controller_topology(
    target: str,
    contract: dict[str, Any],
    *,
    chained: bool = False,
) -> dict[str, Any]:
    if chained and target != "arm":
        raise GenerationError("only the representative ROS arm topology may be chained")
    prefix = "ros2_control"
    activation = f"{target}_direct"
    route = f"{target}_position"
    controller_role = "trajectory" if target == "arm" else "gripper_action"
    topology_suffix = "arm_jtc_admittance" if chained else f"{target}_direct"
    claim = {
        "control_interface_contract_id": contract["control_interface_contract_id"],
        "control_interface_revision": contract["control_interface_revision"],
        "claimed_directions": ["command", "state"],
    }
    interface = {
        "resource_role_id": "arm_joints",
        "interface_name": "position",
    }
    if chained:
        activation = "arm_chain"
        controllers = [
            {
                "controller_role_id": "admittance",
                "control_interface_claims": [claim],
                "exported_reference_interfaces": [interface],
                "exported_state_interfaces": [interface],
            },
            {
                "controller_role_id": "trajectory",
                "control_interface_claims": [],
                "exported_reference_interfaces": [],
                "exported_state_interfaces": [],
            },
        ]
        chain_edges = [
            {
                "upstream_controller_role_id": "trajectory",
                "downstream_controller_role_id": "admittance",
                "reference_interfaces": [interface],
                "state_interfaces": [interface],
            }
        ]
        active_roles = ["admittance", "trajectory"]
        active_edges = [
            {
                "upstream_controller_role_id": "trajectory",
                "downstream_controller_role_id": "admittance",
            }
        ]
        route_roles = ["trajectory", "admittance"]
    else:
        controllers = [
            {
                "controller_role_id": controller_role,
                "control_interface_claims": [claim],
                "exported_reference_interfaces": [],
                "exported_state_interfaces": [],
            }
        ]
        chain_edges = []
        active_roles = [controller_role]
        active_edges = []
        route_roles = [controller_role]
    value = {
        "api_version": "manyforge.controller_topology.v1",
        "controller_topology_id": f"manyforge.{prefix}_{topology_suffix}",
        "controllers": controllers,
        "chain_edges": chain_edges,
        "activation_sets": [
            {
                "activation_set_id": activation,
                "active_controller_role_ids": active_roles,
                "active_chain_edges": active_edges,
            }
        ],
        "legal_routes": [
            {
                "control_route_id": route,
                "control_interface_contract_id": contract[
                    "control_interface_contract_id"
                ],
                "control_interface_revision": contract["control_interface_revision"],
                "ordered_controller_role_ids": route_roles,
                "compatible_activation_set_ids": [activation],
            }
        ],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/controller_topology",
        api_version="manyforge.controller_topology.v1",
        revision_field="topology_revision",
    )


def software_hard_provider_contract_ref() -> dict[str, str]:
    return {
        "provider_contract_role_id": "software_hard_stop",
        "contract_schema_id": EMPTY_PROVIDER_CONTRACT_SCHEMA_ID,
        "contract_schema_revision": EMPTY_PROVIDER_CONTRACT_SCHEMA_REVISION,
    }


def software_hard_provider_contract() -> dict[str, Any]:
    return {
        "provider_contract_ref": software_hard_provider_contract_ref(),
        "stop_command_support": {
            "command_kind": "registered",
            "registered_command_kind_id": "manyforge.stop.software_hard",
            "command_endpoint_role_ids": [],
        },
        "stop_evidence_support": {
            "evidence_kind": "motion_rest",
            "evidence_endpoint_role_ids": ["state"],
        },
    }


def gripper_logical_cancel_provider_contract_ref() -> dict[str, str]:
    return {
        "provider_contract_role_id": "gripper_logical_cancel",
        "contract_schema_id": EMPTY_PROVIDER_CONTRACT_SCHEMA_ID,
        "contract_schema_revision": EMPTY_PROVIDER_CONTRACT_SCHEMA_REVISION,
    }


def gripper_logical_cancel_provider_contract() -> dict[str, Any]:
    return {
        "provider_contract_ref": gripper_logical_cancel_provider_contract_ref(),
        "stop_command_support": {
            "command_kind": "logical_cancel",
            "command_endpoint_role_ids": [],
        },
        "stop_evidence_support": {
            "evidence_kind": "logical_cancellation_no_continuing_effect",
            "evidence_endpoint_role_ids": [],
        },
    }


def motion_controlled_stop_provider_contract_ref() -> dict[str, str]:
    return {
        "provider_contract_role_id": "stop_contract",
        "contract_schema_id": MOTION_CONTROLLED_STOP_SCHEMA_ID,
        "contract_schema_revision": MOTION_CONTROLLED_STOP_SCHEMA_REVISION,
    }


def motion_controlled_stop_provider_contract() -> dict[str, Any]:
    return {
        "provider_contract_ref": motion_controlled_stop_provider_contract_ref(),
        "stop_command_support": {
            "command_kind": "motion_controlled_stop",
            "command_endpoint_role_ids": ["command"],
        },
        "stop_evidence_support": {
            "evidence_kind": "motion_rest",
            "evidence_endpoint_role_ids": ["state"],
        },
    }


def provider_manifest(
    provider_id: str,
    capability_ids: tuple[str, ...],
    endpoint_roles: list[str],
    *,
    include_native_stop_support: bool = False,
) -> dict[str, Any]:
    provider_contracts: list[dict[str, Any]] = []
    if include_native_stop_support:
        provider_contracts.extend(
            [
                motion_controlled_stop_provider_contract(),
                software_hard_provider_contract(),
            ]
        )
        provider_contracts.sort(
            key=lambda row: row["provider_contract_ref"]["provider_contract_role_id"]
        )
    value = {
        "api_version": "manyforge.provider_manifest.v1",
        "provider_manifest_id": provider_id,
        "offered_capabilities": [
            {
                **capability_selector(capability_id),
                "required_endpoint_role_ids": endpoint_roles,
            }
            for capability_id in sorted(capability_ids)
        ],
        "provider_contracts": provider_contracts,
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def lifecycle_only_manifest(provider_id: str, intents: tuple[str, ...]) -> dict[str, Any]:
    """A provider offering only the given component lifecycle capabilities."""
    return revisioned_document(
        {
            "api_version": "manyforge.provider_manifest.v1",
            "provider_manifest_id": provider_id,
            "offered_capabilities": [
                {
                    **capability_selector(f"component.lifecycle.{intent}"),
                    "required_endpoint_role_ids": [],
                }
                for intent in sorted(intents)
            ],
            "provider_contracts": [],
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def io_lifecycle_manifest(
    provider_id: str, capability_ids: tuple[str, ...], endpoint_roles: list[str]
) -> dict[str, Any]:
    """A model-free provider: every lifecycle capability plus its offers."""
    manifest = lifecycle_only_manifest(provider_id, LIFECYCLE_INTENTS)
    manifest["offered_capabilities"].extend(
        {**capability_selector(capability_id),
         "required_endpoint_role_ids": endpoint_roles}
        for capability_id in capability_ids
    )
    manifest["offered_capabilities"].sort(key=lambda row: row["capability_id"])
    return revisioned_document(
        manifest, domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1", revision_field="provider_manifest_revision")


def io_signal_provider_manifest() -> dict[str, Any]:
    return io_lifecycle_manifest(
        "manyforge.provider.io_signals",
        ("io.gpio.get_input", "io.gpio.wait_for_input", "io.gpio.set_output"),
        ["io_signal_service"],
    )


def pose_source_provider_manifest() -> dict[str, Any]:
    return io_lifecycle_manifest(
        "manyforge.provider.pose_source", ("scene.pose.observe",), []
    )


def ai_model_provider_manifest() -> dict[str, Any]:
    return io_lifecycle_manifest(
        "manyforge.provider.ai_model", ("ai.model.invoke",), []
    )


def xarm_lifecycle_manifest() -> dict[str, Any]:
    """The xArm arm: Recover clears the vendor error and warning."""
    return lifecycle_only_manifest(XARM_LIFECYCLE_PROVIDER_ID, ("observe", "recover"))


def xarm_gripper_lifecycle_manifest() -> dict[str, Any]:
    return lifecycle_only_manifest(XARM_GRIPPER_LIFECYCLE_PROVIDER_ID, LIFECYCLE_INTENTS)


def native_lifecycle_provider_manifest(
    lifecycle_manifest: dict[str, Any],
    capability_ids: tuple[str, ...],
    *,
    include_motion_controlled_stop: bool,
) -> dict[str, Any]:
    lifecycle_capabilities = {
        offer["capability_id"]: offer
        for offer in lifecycle_manifest["offered_capabilities"]
    }
    offered_capabilities = list(lifecycle_capabilities.values())
    offered_capabilities.extend(
        {
            **capability_selector(capability_id),
            "required_endpoint_role_ids": ["command", "state"],
        }
        for capability_id in capability_ids
    )
    offered_capabilities.sort(key=lambda row: row["capability_id"])
    provider_contracts = [software_hard_provider_contract()]
    if include_motion_controlled_stop:
        provider_contracts.append(motion_controlled_stop_provider_contract())
    provider_contracts.sort(
        key=lambda row: row["provider_contract_ref"]["provider_contract_role_id"]
    )
    return revisioned_document(
        {
            "api_version": "manyforge.provider_manifest.v1",
            "provider_manifest_id": lifecycle_manifest["provider_manifest_id"],
            "offered_capabilities": offered_capabilities,
            "provider_contracts": provider_contracts,
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def ros2_control_provider_manifest(
    control_capabilities: tuple[str, ...],
    planner_capabilities: tuple[str, ...],
    provider_id: str = ROS2_CONTROL_PROVIDER_ID,
) -> dict[str, Any]:
    lifecycle_capabilities = tuple(
        f"component.lifecycle.{intent}" for intent in LIFECYCLE_INTENTS
    )
    endpoint_roles = {
        **{
            capability_id: ["command", "state"]
            for capability_id in control_capabilities
        },
        **{
            capability_id: ["planner_scene_projection"]
            for capability_id in planner_capabilities
        },
        **{capability_id: [] for capability_id in lifecycle_capabilities},
    }
    return revisioned_document(
        {
            "api_version": "manyforge.provider_manifest.v1",
            "provider_manifest_id": provider_id,
            "offered_capabilities": [
                {
                    **capability_selector(capability_id),
                    "required_endpoint_role_ids": endpoint_roles[capability_id],
                }
                for capability_id in sorted(endpoint_roles)
            ],
            "provider_contracts": sorted(
                [
                    motion_controlled_stop_provider_contract(),
                    software_hard_provider_contract(),
                    gripper_logical_cancel_provider_contract(),
                ],
                key=lambda row: row["provider_contract_ref"][
                    "provider_contract_role_id"
                ],
            ),
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def gripper_logical_cancel_stop_policy() -> dict[str, Any]:
    # DX-755/Q58: command invalidation is logical evidence, not ROS ACK or rest.
    contract_ref = gripper_logical_cancel_provider_contract_ref()
    value = {
        "api_version": "manyforge.stop_evidence_policy.v2",
        "stop_evidence_policy_id": "manyforge.gripper_logical_cancellation",
        "disposition": "stoppable",
        "containment_granularity": "resource",
        "escalation_stages": [
            {
                "stop_escalation_stage_id": "gripper_logical_cancel",
                "stage_kind": "nominal",
                "containment_granularity": "resource",
                "containment_command": {
                    "command": {
                        "command_kind": "logical_cancel",
                        "provider_contract_ref": contract_ref,
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "logical_cancel_fenced",
                        "evidence": {
                            "evidence_kind": "logical_cancellation_no_continuing_effect",
                            "provider_contract_ref": contract_ref,
                        },
                        "source_requirement": {
                            "source_kind": "command_target_status",
                            "freshness_budget_ns": 100_000_000,
                        },
                        "observation_semantics": {
                            "observation_kind": "command_result"
                        },
                    }
                ],
            }
        ],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/stop_evidence_policy",
        api_version="manyforge.stop_evidence_policy.v2",
        revision_field="stop_evidence_policy_revision",
    )


def gripper_position_cancel_stop_policy() -> dict[str, Any]:
    # DX-775: stock action cancellation holds; logical evidence claims no rest.
    value = gripper_logical_cancel_stop_policy()
    value.pop("stop_evidence_policy_revision")
    value["stop_evidence_policy_id"] = "manyforge.gripper_position_cancellation"
    hard_stage = copy.deepcopy(value["escalation_stages"][0])
    hard_stage["stop_escalation_stage_id"] = "software_hard"
    hard_stage["stage_kind"] = "software_hard"
    hard_stage["containment_command"]["command"] = {
        "command_kind": "registered",
        "provider_contract_ref": software_hard_provider_contract_ref(),
        "registered_command_kind_id": "manyforge.stop.software_hard",
    }
    hard_stage["evidence_requirements"][0]["evidence_item_id"] = "hard_cancel_fenced"
    value["escalation_stages"].append(hard_stage)
    return revisioned_document(
        value,
        domain="catalog-record-revision/stop_evidence_policy",
        api_version="manyforge.stop_evidence_policy.v2",
        revision_field="stop_evidence_policy_revision",
    )


def controller_goal_cancel_stop_policy() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.stop_evidence_policy.v2",
        "stop_evidence_policy_id": "manyforge.controller_goal_terminal",
        "disposition": "stoppable",
        "containment_granularity": "resource",
        "escalation_stages": [
            {
                "stop_escalation_stage_id": "nominal_controller_goal_cancel",
                "stage_kind": "nominal",
                "containment_granularity": "resource",
                "containment_command": {
                    "command": {"command_kind": "controller_goal_cancel"},
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "goal_terminal",
                        "evidence": {"evidence_kind": "controller_goal_terminal"},
                        "source_requirement": {
                            "source_kind": "controller_goal_status",
                            "freshness_budget_ns": 100_000_000,
                        },
                        "observation_semantics": {
                            "observation_kind": "post_command_sample"
                        },
                    }
                ],
            },
            {
                "stop_escalation_stage_id": "software_hard",
                "stage_kind": "software_hard",
                "containment_granularity": "resource",
                "containment_command": {
                    "command": {
                        "command_kind": "registered",
                        "provider_contract_ref": software_hard_provider_contract_ref(),
                        "registered_command_kind_id": "manyforge.stop.software_hard",
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "hard_rest",
                        "evidence": {
                            "evidence_kind": "motion_rest",
                            "provider_contract_ref": software_hard_provider_contract_ref(),
                        },
                        "source_requirement": {
                            "source_kind": "binding_state_authority",
                            "freshness_budget_ns": 100_000_000,
                            "state_endpoint_role_id": "state",
                            "allowed_authority_classes": [
                                "commanded",
                                "measured",
                                "predicted",
                            ],
                        },
                        "observation_semantics": {
                            "observation_kind": "post_command_dwell",
                            "minimum_dwell_ns": 10_000_000,
                        },
                    }
                ],
            },
        ],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/stop_evidence_policy",
        api_version="manyforge.stop_evidence_policy.v2",
        revision_field="stop_evidence_policy_revision",
    )


def motion_controlled_stop_policy() -> dict[str, Any]:
    controlled_ref = motion_controlled_stop_provider_contract_ref()
    value = {
        "api_version": "manyforge.stop_evidence_policy.v2",
        "stop_evidence_policy_id": "manyforge.arm_motion_controlled_stop",
        "disposition": "stoppable",
        "containment_granularity": "resource",
        "escalation_stages": [
            {
                "stop_escalation_stage_id": "motion_controlled_stop",
                "stage_kind": "nominal",
                "containment_granularity": "resource",
                "containment_command": {
                    "command": {
                        "command_kind": "motion_controlled_stop",
                        "provider_contract_ref": controlled_ref,
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "controlled_rest",
                        "evidence": {
                            "evidence_kind": "motion_rest",
                            "provider_contract_ref": controlled_ref,
                        },
                        "source_requirement": {
                            "source_kind": "binding_state_authority",
                            "freshness_budget_ns": 100_000_000,
                            "state_endpoint_role_id": "state",
                            "allowed_authority_classes": [
                                "commanded",
                                "measured",
                                "predicted",
                            ],
                        },
                        "observation_semantics": {
                            "observation_kind": "post_command_dwell",
                            "minimum_dwell_ns": 10_000_000,
                        },
                    }
                ],
            },
            {
                "stop_escalation_stage_id": "software_hard",
                "stage_kind": "software_hard",
                "containment_granularity": "resource",
                "containment_command": {
                    "command": {
                        "command_kind": "registered",
                        "provider_contract_ref": software_hard_provider_contract_ref(),
                        "registered_command_kind_id": "manyforge.stop.software_hard",
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "hard_rest",
                        "evidence": {
                            "evidence_kind": "motion_rest",
                            "provider_contract_ref": software_hard_provider_contract_ref(),
                        },
                        "source_requirement": {
                            "source_kind": "binding_state_authority",
                            "freshness_budget_ns": 100_000_000,
                            "state_endpoint_role_id": "state",
                            "allowed_authority_classes": [
                                "commanded",
                                "measured",
                                "predicted",
                            ],
                        },
                        "observation_semantics": {
                            "observation_kind": "post_command_dwell",
                            "minimum_dwell_ns": 10_000_000,
                        },
                    }
                ],
            },
        ],
    }
    # Preserve the qualified hard floor's scope and evidence. This prepared
    # row realizes old-main's direct cancel when no fault profile is authored.
    cancel_stage = copy.deepcopy(value["escalation_stages"][-1])
    cancel_stage["stop_escalation_stage_id"] = "runtime_collision_controller_goal_cancel"
    cancel_stage["stage_kind"] = "controlled_fallback"
    cancel_stage["containment_command"] = {
        "command": {"command_kind": "controller_goal_cancel"},
    }
    cancel_stage["evidence_requirements"][0]["evidence_item_id"] = "runtime_collision_cancel_rest"
    value["escalation_stages"].insert(1, cancel_stage)
    return revisioned_document(
        value,
        domain="catalog-record-revision/stop_evidence_policy",
        api_version="manyforge.stop_evidence_policy.v2",
        revision_field="stop_evidence_policy_revision",
    )


def ros2_control_runtime_profile() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.runtime_profile.v1",
        "runtime_profile_id": "manyforge.ros2_control_execution_runtime",
        "configuration_schema_ref": {
            "schema_id": ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_ID,
            "schema_revision": ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_REVISION,
        },
        "process_contract": {"lifecycle_control": "manager_managed"},
        "supervision_contract": {
            "status_endpoint_role_id": "ros2_control_execution_status"
        },
        "offered_runtime_role_ids": ["ros2_control_execution"],
        "source_endpoint_role_ids": [
            "logical_scene_services",
            "planner_scene_projection",
            "ros2_control_execution_status",
        ],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/runtime_profile",
        api_version="manyforge.runtime_profile.v1",
        revision_field="runtime_profile_revision",
    )


def catalog_sources(
    root: Path,
    description: dict[str, Any],
) -> tuple[
    dict[str, tuple[Path, bytes, dict[str, str]]],
    dict[str, dict[str, str]],
    list[tuple[Path, bytes, str]],
]:
    schemas = schema_revisions(root)
    sources: dict[str, tuple[Path, bytes, dict[str, str]]] = {}

    for capability_id in sorted(CAPABILITIES):
        document = capability_document(capability_id)
        key = f"capability:{capability_id}"
        sources[key] = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"capability_definition/{capability_id}.json"
            ),
            kind="capability_definition",
            record_id=capability_id,
            record_revision=document["capability_definition_revision"],
            schema_id="manyforge.capability_definition.v1",
            schema_revision=schemas["capability_definition"],
            value=document,
        )

    contracts = {
        ("ros2_control", target): control_contract(target)
        for target in ("arm", "gripper")
    }
    for (runtime_kind, target), document in contracts.items():
        key_prefix = "ros2_control:"
        sources[f"contract:{key_prefix}{target}"] = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"control_interface_contract/{document['control_interface_contract_id']}.json"
            ),
            kind="control_interface_contract",
            record_id=document["control_interface_contract_id"],
            record_revision=document["control_interface_revision"],
            schema_id="manyforge.control_interface_contract.v1",
            schema_revision=schemas["control_interface_contract"],
            value=document,
        )
        topology = controller_topology(target, document)
        sources[f"topology:{key_prefix}{target}"] = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"controller_topology/{topology['controller_topology_id']}.json"
            ),
            kind="controller_topology",
            record_id=topology["controller_topology_id"],
            record_revision=topology["topology_revision"],
            schema_id="manyforge.controller_topology.v1",
            schema_revision=schemas["controller_topology"],
            value=topology,
        )
    chained_topology = controller_topology(
        "arm", contracts[("ros2_control", "arm")], chained=True
    )
    sources["topology:ros2_control:arm_chain"] = generated_catalog_document(
        root=root,
        relative_path=(
            "manyforge_deployment/resources/maintained/catalog/"
            f"controller_topology/{chained_topology['controller_topology_id']}.json"
        ),
        kind="controller_topology",
        record_id=chained_topology["controller_topology_id"],
        record_revision=chained_topology["topology_revision"],
        schema_id="manyforge.controller_topology.v1",
        schema_revision=schemas["controller_topology"],
        value=chained_topology,
    )

    control_capabilities = tuple(
        sorted(
            capability_id
            for capability_id in set(NATIVE_ARM_CAPABILITIES + GRIPPER_CAPABILITIES)
            if CAPABILITIES[capability_id][2] == "control_binding"
        )
    )
    planner_capabilities = tuple(
        sorted(
            capability_id
            for capability_id in set(ARM_CAPABILITIES + GRIPPER_CAPABILITIES)
            if CAPABILITIES[capability_id][2] == "capability_provider_binding"
        )
    )
    arm_lifecycle_manifest = xarm_lifecycle_manifest()
    gripper_lifecycle_manifest = xarm_gripper_lifecycle_manifest()
    native_arm_capabilities = tuple(
        sorted(
            capability_id
            for capability_id in NATIVE_ARM_CAPABILITIES
            if CAPABILITIES[capability_id][2] == "control_binding"
        )
    )
    native_gripper_capabilities = tuple(
        sorted(
            capability_id
            for capability_id in GRIPPER_CAPABILITIES
            if CAPABILITIES[capability_id][2] == "control_binding"
        )
    )
    native_arm_manifest = native_lifecycle_provider_manifest(
        arm_lifecycle_manifest,
        native_arm_capabilities,
        include_motion_controlled_stop=True,
    )
    native_gripper_manifest = native_lifecycle_provider_manifest(
        gripper_lifecycle_manifest,
        native_gripper_capabilities,
        include_motion_controlled_stop=False,
    )
    io_signal_manifest = io_signal_provider_manifest()
    pose_source_manifest = pose_source_provider_manifest()
    ai_model_manifest = ai_model_provider_manifest()
    manifests = {
        "io_signals": io_signal_manifest,
        "pose_source": pose_source_manifest,
        "ai_model": ai_model_manifest,
        "logical_scene": provider_manifest(
            "manyforge.provider.logical_scene",
            CELL_CAPABILITIES,
            ["logical_scene_services"],
        ),
        "assistant_bridge": provider_manifest(
            "manyforge.provider.assistant_bridge",
            ("assistant.bridge.request",),
            ["assistant_request"],
        ),
        "model_endpoint": provider_manifest(
            "manyforge.provider.model_endpoint",
            ("assistant.model.invoke",),
            ["model_inference"],
        ),
        "lite6_arm": native_arm_manifest,
        "lite6_gripper": native_gripper_manifest,
        "ros2_control": ros2_control_provider_manifest(
            tuple(
                value
                for value in control_capabilities
                if value != "execution.runtime_scale.operation_scoped"
            ),
            planner_capabilities,
        ),
    }
    for key, document in manifests.items():
        sources[f"provider:{key}"] = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"provider_manifest/{document['provider_manifest_id']}.json"
            ),
            kind="provider_manifest",
            record_id=document["provider_manifest_id"],
            record_revision=document["provider_manifest_revision"],
            schema_id="manyforge.provider_manifest.v1",
            schema_revision=schemas["provider_manifest"],
            value=document,
            canonical_output=(key in ("lite6_arm", "lite6_gripper")),
        )

    for key, policy in (
        ("stop:arm", motion_controlled_stop_policy()),
        ("stop:goal_cancel", controller_goal_cancel_stop_policy()),
        ("stop:gripper_logical_cancel", gripper_logical_cancel_stop_policy()),
        ("stop:gripper_position_cancel", gripper_position_cancel_stop_policy()),
    ):
        sources[key] = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"stop_evidence_policy/{policy['stop_evidence_policy_id']}.json"
            ),
            kind="stop_evidence_policy",
            record_id=policy["stop_evidence_policy_id"],
            record_revision=policy["stop_evidence_policy_revision"],
            schema_id="manyforge.stop_evidence_policy.v2",
            schema_revision=schemas["stop_evidence_policy"],
            value=policy,
        )

    ros_runtime = ros2_control_runtime_profile()
    sources["runtime:ros2_control"] = generated_catalog_document(
        root=root,
        relative_path=(
            "manyforge_deployment/resources/maintained/catalog/"
            f"runtime_profile/{ros_runtime['runtime_profile_id']}.json"
        ),
        kind="runtime_profile",
        record_id=ros_runtime["runtime_profile_id"],
        record_revision=ros_runtime["runtime_profile_revision"],
        schema_id="manyforge.runtime_profile.v1",
        schema_revision=schemas["runtime_profile"],
        value=ros_runtime,
        canonical_output=True,
    )

    component_references: dict[str, dict[str, str]] = {}
    # Every family with a ROS controller profile (Isaac or offline mock, also
    # those only composed by generated multi-robot cells) has the fixed-scale
    # ROS component type. Merge 6: every family has one.
    fixed_scale_families = {
        family["model_family_id"] for family in description["model_families"]
    }
    for family in description["model_families"]:
        document = component_type_document(family)
        family_id = family["model_family_id"]
        source = generated_catalog_document(
            root=root,
            relative_path=(
                "manyforge_deployment/resources/maintained/catalog/"
                f"component_type/{document['component_type_id']}.json"
            ),
            kind="component_type",
            record_id=document["component_type_id"],
            record_revision=document["component_type_revision"],
            schema_id="manyforge.component_type.v1",
            schema_revision=schemas["component_type"],
            value=document,
        )
        sources[f"component:{family_id}"] = source
        component_references[family_id] = source[2]
        if family_id in fixed_scale_families:
            # A target declaration is a required capability set, not optional
            # advertising. Reuse the generator for the fixed-scale profile;
            # never promise native scaling through an unqualified ROS provider.
            fixed = component_type_document(family, runtime_scaling=False)
            sources[f"component:fixed_scale:{family_id}"] = generated_catalog_document(
                root=root,
                relative_path=(
                    "manyforge_deployment/resources/maintained/catalog/"
                    f"component_type/{fixed['component_type_id']}.json"
                ),
                kind="component_type",
                record_id=fixed["component_type_id"],
                record_revision=fixed["component_type_revision"],
                schema_id="manyforge.component_type.v1",
                schema_revision=schemas["component_type"],
                value=fixed,
            )

    command_path = (
        root / "manyforge_deployment/resources/catalog/gripper.command_position.v1.yaml"
    )
    command = read_yaml(command_path, "gripper command schema")
    command_payload = command_path.read_bytes()
    sources["command_schema"] = (
        command_path,
        command_payload,
        catalog_reference(
            kind="command_schema",
            record_id=command["command_schema_id"],
            record_revision=command["command_schema_revision"],
            schema_id="manyforge.command_schema.v1",
            schema_revision=schemas["command_schema"],
            payload=command_payload,
        ),
    )

    for family in description["model_families"]:
        family_id = family["model_family_id"]
        model_path = root / family["catalog_source"]
        model = read_json(model_path, f"{family_id} model catalog")
        payload = model_path.read_bytes()
        sources[f"model:{family_id}"] = (
            model_path,
            payload,
            catalog_reference(
                kind="model_asset",
                record_id=model["model_asset_id"],
                record_revision=model["model_revision"],
                schema_id="manyforge.model_asset.v1",
                schema_revision=schemas["model_asset"],
                payload=payload,
            ),
        )
    # R7 package 5 (2026-10-25): the build-owned C++ copy of the supervised
    # Lite 6 lifecycle catalog was removed with the supervised mode;
    # manyforge_xarm_control mirrors the xArm plans above and tests the digest.
    lifecycle_outputs: list[tuple[Path, bytes, str]] = []
    return sources, component_references, lifecycle_outputs


def source_joint_names(family: dict[str, Any], role: str) -> list[str]:
    for row in family["semantics"]["resource_role_bindings"]:
        if row["resource_role_id"] == role:
            return list(row["source_joint_names"])
    return []




















def site_overlay(
    root: Path, profiles: dict[str, Any]
) -> tuple[Path, bytes, dict[str, str]]:
    values: dict[str, dict[str, Any]] = {}
    for profile in profiles["profiles"]:
        for endpoint in profile["model_endpoints"]:
            reference = endpoint["site_overlay_value_ref"]
            values[reference] = {
                "site_overlay_value_ref": reference,
                "semantic_kind": "external_resource",
                "resource_kind": "model_endpoint",
                "value": {
                    "value_kind": "string",
                    "value": endpoint["endpoint_value"],
                },
            }
        provider = profile["provider_alias"]
        if provider is not None:
            reference = provider["site_overlay_value_ref"]
            values[reference] = {
                "site_overlay_value_ref": reference,
                "semantic_kind": "external_resource",
                "resource_kind": "service_endpoint",
                "value": {
                    "value_kind": "string",
                    "value": provider["endpoint_value"],
                },
            }
    value = revisioned_document(
        {
            "api_version": "manyforge.site_overlay.v1",
            "site_overlay_id": "manyforge_local_examples",
            "values": [values[key] for key in sorted(values)],
        },
        domain="site-overlay-revision",
        api_version="manyforge.site_overlay.v1",
        revision_field="site_overlay_revision",
    )
    path = (
        root
        / "manyforge_deployment/resources/maintained/site_overlays/manyforge_local_examples.site_overlay.json"
    )
    payload = pretty_bytes(value)
    reference = {
        "site_overlay_id": value["site_overlay_id"],
        "site_overlay_revision": value["site_overlay_revision"],
        "non_secret_digest": sha256_digest(canonical_bytes(value)),
    }
    return path, payload, reference


































def generate(args: argparse.Namespace) -> None:
    root = args.workspace_root.resolve()
    description_path = args.description.resolve()
    validate_maintained_description(description_path, root)
    description = read_json(description_path, "maintained description")
    profiles = read_json(args.assistant_profiles.resolve(), "assistant profiles")
    if description.get("api_version") != DESCRIPTION_API:
        raise GenerationError("maintained description has the wrong API version")
    if profiles.get("api_version") != ASSISTANT_API:
        raise GenerationError("assistant profiles have the wrong API version")

    source_rows, component_refs, lifecycle_outputs = catalog_sources(root, description)
    del component_refs
    outputs: list[tuple[Path, bytes, str]] = []
    outputs.extend(lifecycle_outputs)
    for key, (path, payload, unused_ref) in source_rows.items():
        del unused_ref
        if key.startswith("model:") or key == "command_schema":
            continue
        outputs.append((path, payload, "maintained catalog source"))

    overlay_path, overlay_payload, _overlay_ref = site_overlay(root, profiles)
    outputs.append((overlay_path, overlay_payload, "maintained site overlay"))

    for path, payload, label in outputs:
        require_or_update(path, payload, args.update, label)
    # Retired products; keep archival profile, planning and ROS outputs.
    for relative in (
        "manyforge_deployment/resources/maintained/maintained_historical_capability_closure.v1.json",
        "manyforge_deployment/resources/maintained/catalog/provider_manifest/manyforge.provider.native_control.json",
        "manyforge_deployment/resources/maintained/catalog/controller_topology/manyforge.native_arm_direct.json",
        "manyforge_deployment/resources/maintained/catalog/controller_topology/manyforge.native_gripper_direct.json",
        "manyforge_deployment/resources/maintained/catalog/control_interface_contract/manyforge.native_arm_position.json",
        "manyforge_deployment/resources/maintained/catalog/control_interface_contract/manyforge.native_gripper_position.json",
        "manyforge_roboplan/resources/lifecycle/native_control.lifecycle_plan_catalog.json",
        # R7 D12 (2026-10-25): renamed to xarm_arm_provider/xarm_gripper_provider.
        "manyforge_deployment/resources/maintained/catalog/provider_manifest/lite6_arm_provider.json",
        "manyforge_deployment/resources/maintained/catalog/provider_manifest/lite6_gripper_provider.json",
        # Merge 6: the Real Lite 6 profile now selects the split `lite6` arm,
        # which needs no arm-only variant.
        "manyforge_deployment/resources/maintained/catalog/component_type/lite6_lite_gripper.fixed_scale.arm_only.component.json",
    ):
        require_obsolete_output_absent(root / relative, update=args.update)


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--update", action="store_true")
    parser.add_argument("--description", type=Path, required=True)
    parser.add_argument("--assistant-profiles", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args)
    except (GenerationError, SourceDescriptionError, OSError) as error:
        print(
            f"generate_maintained_deployment_occurrences: {error}", file=os.sys.stderr
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
