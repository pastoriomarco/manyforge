#!/usr/bin/env python3
"""Generate and verify the maintained native and Isaac deployment occurrences.

This is an authoring projection, not a deployment validator.  It derives the
repetitive graph/catalog records from the maintained source description and
delegates all graph/provider/deployment revision production and verification
to the C++ ``manyforge_deployment_revision`` owner.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Final

import yaml
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    raw_sha256_digest,
)
from manyforge_behavior.model_source_capture import (
    capture_deployment_model_sources,
)

from check_maintained_deployment_sources import (
    SourceDescriptionError,
    validate as validate_maintained_description,
)


ZERO_DIGEST: Final = "sha256:" + "0" * 64
DESCRIPTION_API: Final = "manyforge.maintained_deployment_sources.v1"
ASSISTANT_API: Final = "manyforge.maintained_assistant_profiles.v1"
SCENARIO_PROJECT_API: Final = "manyforge.scenario.project.v2"
DEPLOYMENT_INTENT_API: Final = "manyforge.deployment_intent.v2"
SCENARIO_SOURCE_SNAPSHOT_API: Final = "manyforge.scenario.source_snapshot.v1"
CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1\0"
NATIVE_RUNTIME_CONFIGURATION_SCHEMA_ID: Final = (
    "manyforge.value_schema.runtime.native_execution.v1"
)
NATIVE_RUNTIME_CONFIGURATION_SCHEMA_REVISION: Final = (
    "sha256:6b540dac39434e05a2cb5da209206e2105b824b9f56704a74f0d88f327944689"
)
ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_ID: Final = (
    "manyforge.value_schema.runtime.ros2_control_execution.v1"
)
ROS2_CONTROL_RUNTIME_CONFIGURATION_SCHEMA_REVISION: Final = (
    "sha256:4ff0c6d3d2e4cf9eae515f0ba3343404e3f30939a08f886aadfe26fa57fe173d"
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
    "sha256:58ec6382a268329e9d8001eb53aac31259b5eb95a3690095aa4bbe12ac51d83b"
)
LIFECYCLE_CATALOG_API_VERSION: Final = "manyforge.brand_lifecycle_plan_catalog.v1"
LITE6_LIFECYCLE_PROVIDER_ID: Final = "lite6_arm_provider"
LITE6_GRIPPER_LIFECYCLE_PROVIDER_ID: Final = "lite6_gripper_provider"
LITE6_NATIVE_MODEL_FAMILY_ID: Final = "lite6_lite_gripper"
LIFECYCLE_INTENTS: Final = ("disable", "enable", "observe", "prepare", "recover")
ROS2_CONTROL_PROVIDER_ID: Final = "manyforge.provider.ros2_control"

CAPABILITIES: Final = {
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
    "execution.pause": (
        "sha256:20f45f87bb6ad07f4b180c2e7c781347016c64c89808ae04a41f1dff50b7085e",
        "target_binding",
        "control_binding",
    ),
    "execution.resume": (
        "sha256:442663e2f2283bb503f89a21e760b732150424cd44c504a55de26e621f99e142",
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
    "planning.profile.balanced": (
        "sha256:14c53dad299a42d6604eac14240d015e22a4bc1adb363fb252111e62a2d2ad90",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.profile.first_feasible": (
        "sha256:e0f578ef9af205b3c7c184fb1124ffb4fe3c35874b5e993358b43a2dc43d2674",
        "target_binding",
        "capability_provider_binding",
    ),
    "planning.profile.quality_bounded": (
        "sha256:7f570672270455207d5fb8eb210cd1dd22a99d7909421cf0dc8036c32e4beffb",
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
            "execution.pause",
            "execution.resume",
            "execution.reset",
            "execution.contact.geometric",
            "execution.contact.geometric_signed_penetration",
            "execution.trajectory.execute",
            "model.kinematics.fk",
            "model.limits.joint",
            "planning.motion.cartesian_path",
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.profile.balanced",
            "planning.profile.first_feasible",
            "planning.profile.quality_bounded",
            "planning.trajectory.time_parameterized",
            "state.observed.current",
            "validation.trajectory.non_collision",
        }
    )
)
# The historical closure remains the original 36 facilities. Scaling is an
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

LEGACY_CAPABILITY_REPLACEMENTS: Final = {
    "collision.query.detailed": "context.collision.check.detailed",
    "collision.query.pre_execution": "context.collision.check",
    "model.kinematics.fk": "context.kinematics.fk",
    "model.limits.joint": "context.limits.joint",
    "scene.object.query": "context.scene.object_query",
    "scene.reference.resolve": "context.pose.resolve_reference",
    "state.observed.current": "context.state.current",
    "validation.trajectory.non_collision": "context.validation.trajectory",
}

I8_REQUALIFIED_CAPABILITIES: Final = frozenset(
    {
        "collision.query.detailed",
        "collision.query.pre_execution",
        "execution.contact.geometric",
        "execution.contact.geometric_signed_penetration",
        "execution.stop.soft",
    }
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
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise GenerationError(f"cannot read {label}: {error}") from error
    if type(value) is not dict:
        raise GenerationError(f"{label} root must be an object")
    return value


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
    family_id = family["model_family_id"]
    semantics = family["semantics"]
    group_roles = {
        row["model_group_role_id"] for row in semantics["model_group_role_bindings"]
    }
    resource_roles = {
        row["resource_role_id"] for row in semantics["resource_role_bindings"]
    }
    collision_roles = {
        row["collision_body_role_id"]
        for row in semantics["collision_body_role_bindings"]
    }
    physical_gripper = "gripper" in group_roles and "fingers" in resource_roles
    arm_collision_roles = [
        {
            "collision_body_role_id": role,
            "model_role_id": "arm_model",
            "asset_collision_body_role_id": role,
        }
        for role in ("arm_collision", "tool_collision")
        if role in collision_roles
    ]
    gripper_group_roles = []
    if physical_gripper:
        gripper_group_roles.append(
            {
                "model_group_role_id": "planning_group",
                "model_role_id": "arm_model",
                "asset_model_group_role_id": "gripper",
            }
        )
    gripper_resource = {
        "resource_role_id": "fingers",
        "source_kind": "model_role" if physical_gripper else "component_local",
    }
    if physical_gripper:
        gripper_resource.update(
            {
                "model_role_id": "arm_model",
                "asset_resource_role_id": "fingers",
            }
        )
    else:
        # The historical simulated UR5 tool_position values are linear metres.
        # Retain that semantic kind without inventing a URDF joint.
        gripper_resource["coordinate_kind"] = "linear"
    value = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": f"{family_id}{'' if runtime_scaling else '.fixed_scale'}.component",
        "tags": ["articulated-arm", "gripper"],
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
                        "frame_role_id": "base",
                        "model_role_id": "arm_model",
                        "asset_frame_role_id": "base",
                    },
                    {
                        "frame_role_id": "tcp",
                        "model_role_id": "arm_model",
                        "asset_frame_role_id": "tcp",
                    },
                ],
                "collision_body_roles": arm_collision_roles,
                "capabilities": [
                    capability_selector(capability_id)
                    for capability_id in (
                        NATIVE_ARM_CAPABILITIES if runtime_scaling else ARM_CAPABILITIES
                    )
                ],
            },
            {
                "command_target_id": "gripper",
                "model_group_roles": gripper_group_roles,
                "resource_roles": [gripper_resource],
                "frame_roles": [
                    {
                        "frame_role_id": "tcp",
                        "model_role_id": "arm_model",
                        "asset_frame_role_id": "gripper_tcp",
                    }
                ],
                "collision_body_roles": [
                    {
                        "collision_body_role_id": "gripper_collision",
                        "model_role_id": "arm_model",
                        "asset_collision_body_role_id": "gripper_collision",
                    }
                ],
                "capabilities": [
                    capability_selector(capability_id)
                    for capability_id in GRIPPER_CAPABILITIES
                ],
            },
        ],
        "attachment_interfaces": [],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/component_type",
        api_version="manyforge.component_type.v1",
        revision_field="component_type_revision",
    )


def control_contract(target: str, runtime_kind: str = "native") -> dict[str, Any]:
    role = "arm_joints" if target == "arm" else "fingers"
    prefix = "native" if runtime_kind == "native" else "ros2_control"
    value = {
        "api_version": "manyforge.control_interface_contract.v1",
        "control_interface_contract_id": f"manyforge.{prefix}_{target}_position",
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
    runtime_kind: str = "native",
    *,
    chained: bool = False,
) -> dict[str, Any]:
    if chained and (runtime_kind != "ros2_control" or target != "arm"):
        raise GenerationError("only the representative ROS arm topology may be chained")
    prefix = "native" if runtime_kind == "native" else "ros2_control"
    activation = f"{target}_direct"
    route = f"{target}_position"
    controller_role = (
        "native_execution"
        if runtime_kind == "native"
        else ("trajectory" if target == "arm" else "gripper_action")
    )
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


def lifecycle_boolean(value: bool) -> dict[str, Any]:
    return {"kind": "boolean", "boolean_value": value}


def lifecycle_token(value: str) -> dict[str, Any]:
    return {"kind": "token", "token_value": value}


def lifecycle_predicate(
    states: list[str], facts: dict[str, dict[str, Any]]
) -> dict[str, Any]:
    state_order = {
        state: index
        for index, state in enumerate(
            (
                "undeclared",
                "declared",
                "prepared",
                "active",
                "degraded",
                "faulted",
                "disabling",
                "disabled",
            )
        )
    }
    return {
        "allowed_states": sorted(states, key=state_order.__getitem__),
        "fact_predicates": [
            {"lifecycle_fact_id": fact_id, "expected_value": facts[fact_id]}
            for fact_id in sorted(facts)
        ],
    }


def lifecycle_transition(
    primitive_id: str, eligibility: dict[str, Any], postcondition: dict[str, Any]
) -> dict[str, Any]:
    return {
        "lifecycle_primitive_id": primitive_id,
        "eligibility_predicate": eligibility,
        "postcondition_predicate": postcondition,
    }


def lite6_lifecycle_plans() -> list[dict[str, Any]]:
    boolean = lifecycle_boolean
    token = lifecycle_token
    prepared = lifecycle_predicate(
        ["prepared"],
        {
            "xarm.motion_enabled": boolean(True),
            "xarm.position_valid": boolean(True),
            "xarm.joint_servo_mode_selected": boolean(True),
            "xarm.vendor_control_ready": boolean(False),
            "xarm.vendor_stopped": boolean(False),
            "xarm.fault_present": boolean(False),
            "xarm.recovery_required": boolean(False),
            "xarm.warning_present": boolean(False),
            "xarm.vendor_feedback_class": token("mode_changed"),
        },
    )
    enabled_stopped = lifecycle_predicate(
        ["degraded"],
        {
            "xarm.motion_enabled": boolean(True),
            "xarm.position_valid": boolean(True),
            "xarm.vendor_stopped": boolean(True),
            "xarm.fault_present": boolean(False),
            "xarm.recovery_required": boolean(False),
            "xarm.warning_present": boolean(False),
            "xarm.vendor_feedback_class": token("stopped"),
        },
    )
    active = lifecycle_predicate(
        ["active"],
        {
            "xarm.motion_enabled": boolean(True),
            "xarm.position_valid": boolean(True),
            "xarm.joint_servo_mode_selected": boolean(True),
            "xarm.vendor_control_ready": boolean(True),
            "xarm.vendor_stopped": boolean(False),
            "xarm.fault_present": boolean(False),
            "xarm.recovery_required": boolean(False),
            "xarm.warning_present": boolean(False),
            "xarm.vendor_feedback_class": token("ready"),
        },
    )
    disabled = lifecycle_predicate(
        ["disabled"],
        {
            "xarm.motion_enabled": boolean(False),
            "xarm.vendor_control_ready": boolean(False),
            "xarm.vendor_stopped": boolean(True),
            "xarm.vendor_feedback_class": token("stopped"),
        },
    )
    stopped_enabled = lifecycle_predicate(
        ["degraded", "faulted"],
        {
            "xarm.motion_enabled": boolean(True),
            "xarm.vendor_control_ready": boolean(False),
            "xarm.vendor_stopped": boolean(True),
            "xarm.vendor_feedback_class": token("stopped"),
        },
    )
    recovered = lifecycle_predicate(
        ["disabled"],
        {
            "xarm.motion_enabled": boolean(False),
            "xarm.vendor_stopped": boolean(True),
            "xarm.fault_present": boolean(False),
            "xarm.recovery_required": boolean(False),
            "xarm.warning_present": boolean(False),
            "xarm.vendor_feedback_class": token("stopped"),
        },
    )
    timeout_ns = 30_000_000_000
    return [
        {
            "intent": "prepare",
            "requested_state": "prepared",
            "requested_postcondition": prepared,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "xarm.enable_motion",
                    lifecycle_predicate(
                        ["declared", "disabled"],
                        {
                            "xarm.motion_enabled": boolean(False),
                            "xarm.vendor_stopped": boolean(True),
                            "xarm.fault_present": boolean(False),
                            "xarm.recovery_required": boolean(False),
                            "xarm.warning_present": boolean(False),
                            "xarm.vendor_feedback_class": token("stopped"),
                        },
                    ),
                    enabled_stopped,
                ),
                lifecycle_transition(
                    "xarm.select_joint_servo_mode", enabled_stopped, prepared
                ),
            ],
        },
        {
            "intent": "enable",
            "requested_state": "active",
            "requested_postcondition": active,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "xarm.set_ready_state",
                    lifecycle_predicate(
                        prepared["allowed_states"],
                        {
                            **{
                                row["lifecycle_fact_id"]: row["expected_value"]
                                for row in prepared["fact_predicates"]
                            },
                            "xarm.ready_state_request_eligible": boolean(True),
                        },
                    ),
                    active,
                )
            ],
        },
        {
            "intent": "disable",
            "requested_state": "disabled",
            "requested_postcondition": disabled,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "xarm.set_stopped_state",
                    lifecycle_predicate(
                        ["prepared", "active", "degraded", "faulted"],
                        {
                            "xarm.motion_enabled": boolean(True),
                            "xarm.vendor_stopped": boolean(False),
                        },
                    ),
                    stopped_enabled,
                ),
                lifecycle_transition("xarm.disable_motion", stopped_enabled, disabled),
            ],
        },
        {
            "intent": "recover",
            "requested_state": "disabled",
            "recover_target_eligibility": lifecycle_predicate(
                ["disabled"],
                {
                    "xarm.motion_enabled": boolean(False),
                    "xarm.vendor_stopped": boolean(True),
                    "xarm.vendor_feedback_class": token("stopped"),
                },
            ),
            "requested_postcondition": recovered,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "xarm.clear_error",
                    lifecycle_predicate(
                        ["disabled"],
                        {
                            "xarm.motion_enabled": boolean(False),
                            "xarm.vendor_stopped": boolean(True),
                            "xarm.fault_present": boolean(True),
                            "xarm.vendor_feedback_class": token("stopped"),
                        },
                    ),
                    lifecycle_predicate(
                        ["disabled"],
                        {
                            "xarm.motion_enabled": boolean(False),
                            "xarm.vendor_stopped": boolean(True),
                            "xarm.fault_present": boolean(False),
                            "xarm.recovery_required": boolean(False),
                            "xarm.vendor_feedback_class": token("stopped"),
                        },
                    ),
                ),
                lifecycle_transition(
                    "xarm.clear_warning",
                    lifecycle_predicate(
                        ["disabled"],
                        {
                            "xarm.motion_enabled": boolean(False),
                            "xarm.vendor_stopped": boolean(True),
                            "xarm.fault_present": boolean(False),
                            "xarm.recovery_required": boolean(False),
                            "xarm.warning_present": boolean(True),
                            "xarm.vendor_feedback_class": token("stopped"),
                        },
                    ),
                    recovered,
                ),
            ],
        },
    ]


def lifecycle_product(
    provider_id: str, plans: list[dict[str, Any]]
) -> tuple[dict[str, Any], dict[str, Any]]:
    semantic_projection = {
        "api_version": LIFECYCLE_CATALOG_API_VERSION,
        "provider_manifest_id": provider_id,
        "plans": plans,
    }
    semantic_digest = sha256_digest(
        CONTENT_IDENTITY_PREFIX
        + b"brand-lifecycle-plan-catalog-semantic\0"
        + LIFECYCLE_CATALOG_API_VERSION.encode("ascii")
        + b"\0"
        + canonical_bytes(semantic_projection)
    )
    lifecycle_capabilities = tuple(
        f"component.lifecycle.{intent}"
        for intent in sorted({"observe", *(plan["intent"] for plan in plans)})
    )
    manifest = revisioned_document(
        {
            "api_version": "manyforge.provider_manifest.v1",
            "provider_manifest_id": provider_id,
            "offered_capabilities": [
                {
                    **capability_selector(capability_id),
                    "required_endpoint_role_ids": [],
                }
                for capability_id in lifecycle_capabilities
            ],
            "provider_contracts": [],
            "lifecycle_plan_catalog": {
                "catalog_api_version": LIFECYCLE_CATALOG_API_VERSION,
                "semantic_digest": semantic_digest,
            },
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )
    catalog_content = {
        **semantic_projection,
        "provider_manifest_revision": manifest["provider_manifest_revision"],
    }
    catalog_digest = sha256_digest(
        CONTENT_IDENTITY_PREFIX
        + b"brand-lifecycle-plan-catalog\0"
        + LIFECYCLE_CATALOG_API_VERSION.encode("ascii")
        + b"\0"
        + canonical_bytes(catalog_content)
    )
    return manifest, {**catalog_content, "catalog_digest": catalog_digest}


def lite6_lifecycle_product() -> tuple[dict[str, Any], dict[str, Any]]:
    return lifecycle_product(LITE6_LIFECYCLE_PROVIDER_ID, lite6_lifecycle_plans())


def lite6_gripper_lifecycle_predicate(
    state: str, enabled: bool, fault_present: bool
) -> dict[str, Any]:
    return lifecycle_predicate(
        [state],
        {
            "native.gripper.command_admission_enabled": lifecycle_boolean(enabled),
            "native.gripper.fault_present": lifecycle_boolean(fault_present),
        },
    )


def lite6_gripper_lifecycle_plans() -> list[dict[str, Any]]:
    timeout_ns = 30_000_000_000
    prepare_eligible = lite6_gripper_lifecycle_predicate("declared", False, False)
    prepare_eligible["allowed_states"].append("disabled")
    prepared = lite6_gripper_lifecycle_predicate("prepared", False, False)
    active = lite6_gripper_lifecycle_predicate("active", True, False)
    disabled = lite6_gripper_lifecycle_predicate("disabled", False, False)
    return [
        {
            "intent": "prepare",
            "requested_state": "prepared",
            "requested_postcondition": prepared,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition("native.gripper.prepare", prepare_eligible, prepared)
            ],
        },
        {
            "intent": "enable",
            "requested_state": "active",
            "requested_postcondition": active,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition("native.gripper.enable", prepared, active)
            ],
        },
        {
            "intent": "disable",
            "requested_state": "disabled",
            "requested_postcondition": disabled,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "native.gripper.disable",
                    lifecycle_predicate(
                        ["prepared", "active", "degraded", "faulted"], {}
                    ),
                    disabled,
                )
            ],
        },
        {
            "intent": "recover",
            "requested_state": "disabled",
            "recover_target_eligibility": lite6_gripper_lifecycle_predicate(
                "disabled", False, True
            ),
            "requested_postcondition": disabled,
            "timeout_duration_ns": timeout_ns,
            "transitions": [
                lifecycle_transition(
                    "native.gripper.clear_fault",
                    lite6_gripper_lifecycle_predicate("disabled", False, True),
                    disabled,
                )
            ],
        },
    ]


def lite6_gripper_lifecycle_product() -> tuple[dict[str, Any], dict[str, Any]]:
    return lifecycle_product(
        LITE6_GRIPPER_LIFECYCLE_PROVIDER_ID, lite6_gripper_lifecycle_plans()
    )


def native_control_lifecycle_plans() -> list[dict[str, Any]]:
    """In-process native readiness; no physical drive or ROS enable is implied."""

    def observed(state: str, enabled: bool) -> dict[str, Any]:
        return lifecycle_predicate(
            [state],
            {
                "native.runtime.present": lifecycle_boolean(True),
                "native.control.enabled": lifecycle_boolean(enabled),
            },
        )

    declared = lifecycle_predicate(
        ["declared", "disabled"],
        {
            "native.runtime.present": lifecycle_boolean(True),
            "native.control.enabled": lifecycle_boolean(False),
        },
    )
    prepared = observed("prepared", False)
    active = observed("active", True)
    disabled = observed("disabled", False)
    return [
        {
            "intent": "prepare",
            "requested_state": "prepared",
            "requested_postcondition": prepared,
            "timeout_duration_ns": 30_000_000_000,
            "transitions": [
                lifecycle_transition("native.control.prepare", declared, prepared)
            ],
        },
        {
            "intent": "enable",
            "requested_state": "active",
            "requested_postcondition": active,
            "timeout_duration_ns": 30_000_000_000,
            "transitions": [
                lifecycle_transition("native.control.enable", prepared, active)
            ],
        },
        {
            "intent": "disable",
            "requested_state": "disabled",
            "requested_postcondition": disabled,
            "timeout_duration_ns": 30_000_000_000,
            "transitions": [
                lifecycle_transition(
                    "native.control.disable",
                    lifecycle_predicate(
                        ["prepared", "active", "degraded", "faulted"],
                        {"native.runtime.present": lifecycle_boolean(True)},
                    ),
                    disabled,
                )
            ],
        },
    ]


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
            "lifecycle_plan_catalog": copy.deepcopy(
                lifecycle_manifest["lifecycle_plan_catalog"]
            ),
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def ros2_control_lifecycle_plans() -> list[dict[str, Any]]:
    ready = lifecycle_predicate(
        ["prepared", "active"],
        {
            "external.controller_configuration_ready": lifecycle_boolean(True),
            "external.fault_present": lifecycle_boolean(False),
            "external.transport_ready": lifecycle_boolean(True),
        },
    )
    active = lifecycle_predicate(
        ["active"],
        {
            "external.controller_configuration_ready": lifecycle_boolean(True),
            "external.fault_present": lifecycle_boolean(False),
            "external.transport_ready": lifecycle_boolean(True),
        },
    )
    disabled = lifecycle_predicate(
        ["disabled"],
        {
            "external.controller_configuration_ready": lifecycle_boolean(False),
            "external.fault_present": lifecycle_boolean(False),
            "external.transport_ready": lifecycle_boolean(True),
        },
    )
    timeout_ns = 30_000_000_000
    return [
        {
            "intent": "prepare",
            "requested_state": "prepared",
            "requested_postcondition": ready,
            "timeout_duration_ns": timeout_ns,
            "transitions": [],
        },
        {
            "intent": "enable",
            "requested_state": "active",
            "requested_postcondition": active,
            "timeout_duration_ns": timeout_ns,
            "transitions": [],
        },
        {
            "intent": "disable",
            "requested_state": "disabled",
            "requested_postcondition": disabled,
            "timeout_duration_ns": timeout_ns,
            "transitions": [],
        },
        {
            "intent": "recover",
            "requested_state": "disabled",
            "recover_target_eligibility": disabled,
            "requested_postcondition": disabled,
            "timeout_duration_ns": timeout_ns,
            "transitions": [],
        },
    ]


def ros2_control_provider_manifest(
    control_capabilities: tuple[str, ...],
    planner_capabilities: tuple[str, ...],
) -> dict[str, Any]:
    plans = ros2_control_lifecycle_plans()
    semantic_projection = {
        "api_version": LIFECYCLE_CATALOG_API_VERSION,
        "provider_manifest_id": ROS2_CONTROL_PROVIDER_ID,
        "plans": plans,
    }
    semantic_digest = sha256_digest(
        CONTENT_IDENTITY_PREFIX
        + b"brand-lifecycle-plan-catalog-semantic\0"
        + LIFECYCLE_CATALOG_API_VERSION.encode("ascii")
        + b"\0"
        + canonical_bytes(semantic_projection)
    )
    lifecycle_capabilities = tuple(
        f"component.lifecycle.{intent}"
        for intent in ("disable", "enable", "observe", "prepare", "recover")
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
            "provider_manifest_id": ROS2_CONTROL_PROVIDER_ID,
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
                ],
                key=lambda row: row["provider_contract_ref"][
                    "provider_contract_role_id"
                ],
            ),
            "lifecycle_plan_catalog": {
                "catalog_api_version": LIFECYCLE_CATALOG_API_VERSION,
                "semantic_digest": semantic_digest,
            },
        },
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def lite6_lifecycle_identity_header(
    manifest: dict[str, Any], catalog: dict[str, Any]
) -> bytes:
    semantic_digest = manifest["lifecycle_plan_catalog"]["semantic_digest"]
    return (
        "// Generated by generate_maintained_deployment_occurrences.py; do not edit.\n"
        "#pragma once\n\n"
        "#include <string_view>\n\n"
        "namespace manyforge::xarm_control::generated {\n"
        f'inline constexpr std::string_view kLite6LifecycleCatalogSemanticDigest = "{semantic_digest}";\n'
        f'inline constexpr std::string_view kLite6LifecycleProviderManifestRevision = "{manifest["provider_manifest_revision"]}";\n'
        f'inline constexpr std::string_view kLite6LifecycleCatalogDigest = "{catalog["catalog_digest"]}";\n'
        "} // namespace manyforge::xarm_control::generated\n"
    ).encode("utf-8")


def cell_controlled_stop_deadline_budget() -> dict[str, Any]:
    """One outer transaction envelope shared by every cell participant."""
    return {
        "outer_duration_ns": 3_000_000_000,
        "kernel_completion_reserve_ns": 200_000_000,
        "phases": [
            {"phase_id": "command", "deadline_offset_ns": 300_000_000},
            {"phase_id": "terminal", "deadline_offset_ns": 1_750_000_000},
            {"phase_id": "hard_command", "deadline_offset_ns": 1_900_000_000},
            {"phase_id": "hard_terminal", "deadline_offset_ns": 2_700_000_000},
        ],
    }


def controller_goal_cancel_stop_policy() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.stop_evidence_policy.v2",
        "stop_evidence_policy_id": "manyforge.native_controller_goal_terminal",
        "disposition": "stoppable",
        "containment_granularity": "resource",
        "deadline_budget": cell_controlled_stop_deadline_budget(),
        "escalation_stages": [
            {
                "stop_escalation_stage_id": "nominal_controller_goal_cancel",
                "stage_kind": "nominal",
                "containment_granularity": "resource",
                "containment_command": {
                    "command_phase_id": "command",
                    "command": {"command_kind": "controller_goal_cancel"},
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "goal_terminal",
                        "phase_id": "terminal",
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
                    "command_phase_id": "hard_command",
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
                        "phase_id": "hard_terminal",
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
        "deadline_budget": cell_controlled_stop_deadline_budget(),
        "escalation_stages": [
            {
                "stop_escalation_stage_id": "motion_controlled_stop",
                "stage_kind": "nominal",
                "containment_granularity": "resource",
                "containment_command": {
                    "command_phase_id": "command",
                    "command": {
                        "command_kind": "motion_controlled_stop",
                        "provider_contract_ref": controlled_ref,
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "controlled_rest",
                        "phase_id": "terminal",
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
                    "command_phase_id": "hard_command",
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
                        "phase_id": "hard_terminal",
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


def native_runtime_profile() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.runtime_profile.v1",
        "runtime_profile_id": "manyforge.native_execution_runtime",
        "configuration_schema_ref": {
            "schema_id": NATIVE_RUNTIME_CONFIGURATION_SCHEMA_ID,
            "schema_revision": NATIVE_RUNTIME_CONFIGURATION_SCHEMA_REVISION,
        },
        "process_contract": {"lifecycle_control": "manager_managed"},
        "supervision_contract": {"status_endpoint_role_id": "native_execution_status"},
        "offered_runtime_role_ids": ["native_execution"],
        "source_endpoint_role_ids": [
            "logical_scene_services",
            "native_execution_status",
            "planner_scene_projection",
        ],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/runtime_profile",
        api_version="manyforge.runtime_profile.v1",
        revision_field="runtime_profile_revision",
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
        (runtime_kind, target): control_contract(target, runtime_kind)
        for runtime_kind in ("native", "ros2_control")
        for target in ("arm", "gripper")
    }
    for (runtime_kind, target), document in contracts.items():
        key_prefix = "" if runtime_kind == "native" else "ros2_control:"
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
        topology = controller_topology(target, document, runtime_kind)
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
        "arm", contracts[("ros2_control", "arm")], "ros2_control", chained=True
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
    lifecycle_manifest, lifecycle_catalog = lite6_lifecycle_product()
    gripper_lifecycle_manifest, _gripper_lifecycle_catalog = (
        lite6_gripper_lifecycle_product()
    )
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
        lifecycle_manifest,
        native_arm_capabilities,
        include_motion_controlled_stop=True,
    )
    native_gripper_manifest = native_lifecycle_provider_manifest(
        gripper_lifecycle_manifest,
        native_gripper_capabilities,
        include_motion_controlled_stop=False,
    )
    native_control_lifecycle_manifest, _ = lifecycle_product(
        "manyforge.provider.native_control", native_control_lifecycle_plans()
    )
    native_control_manifest = native_lifecycle_provider_manifest(
        native_control_lifecycle_manifest,
        control_capabilities,
        include_motion_controlled_stop=True,
    )
    native_control_catalog_content = {
        "api_version": LIFECYCLE_CATALOG_API_VERSION,
        "provider_manifest_id": native_control_manifest["provider_manifest_id"],
        "provider_manifest_revision": native_control_manifest[
            "provider_manifest_revision"
        ],
        "plans": native_control_lifecycle_plans(),
    }
    native_control_lifecycle_catalog = {
        **native_control_catalog_content,
        "catalog_digest": sha256_digest(
            CONTENT_IDENTITY_PREFIX
            + b"brand-lifecycle-plan-catalog\0"
            + LIFECYCLE_CATALOG_API_VERSION.encode("ascii")
            + b"\0"
            + canonical_bytes(native_control_catalog_content)
        ),
    }
    manifests = {
        "control": native_control_manifest,
        "planning": provider_manifest(
            "manyforge.provider.native_planning",
            planner_capabilities,
            ["planner_scene_projection"],
        ),
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

    runtime = native_runtime_profile()
    sources["runtime"] = generated_catalog_document(
        root=root,
        relative_path=(
            "manyforge_deployment/resources/maintained/catalog/"
            f"runtime_profile/{runtime['runtime_profile_id']}.json"
        ),
        kind="runtime_profile",
        record_id=runtime["runtime_profile_id"],
        record_revision=runtime["runtime_profile_revision"],
        schema_id="manyforge.runtime_profile.v1",
        schema_revision=schemas["runtime_profile"],
        value=runtime,
        canonical_output=True,
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
    fixed_scale_profiles = {
        profile["runtime_restoration_profile_id"]
        for profile in description["runtime_restoration_profiles"]
        if "ros2_control_runtime" in profile
    }
    fixed_scale_families = {
        deployment["model_family_id"]
        for deployment in description["deployments"]
        if deployment["runtime_restoration_profile_id"] in fixed_scale_profiles
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
    lifecycle_outputs = [
        (
            root
            / "manyforge_roboplan/resources/lifecycle/native_control.lifecycle_plan_catalog.json",
            canonical_bytes(native_control_lifecycle_catalog),
            "build-owned generic native lifecycle plan catalog",
        ),
        (
            root
            / "manyforge_xarm_control/resources/lifecycle/lite6_arm_provider.lifecycle_plan_catalog.json",
            canonical_bytes(lifecycle_catalog),
            "build-owned Lite6 lifecycle plan catalog",
        ),
        (
            root / "manyforge_xarm_control/resources/lifecycle/lite6_arm_provider.json",
            canonical_bytes(lifecycle_manifest),
            "build-owned Lite6 lifecycle provider manifest",
        ),
        (
            root
            / "manyforge_xarm_control/include/manyforge_xarm_control/generated/lite6_brand_lifecycle_product_identity.hpp",
            lite6_lifecycle_identity_header(lifecycle_manifest, lifecycle_catalog),
            "build-owned Lite6 lifecycle product identity",
        ),
    ]
    return sources, component_references, lifecycle_outputs


def source_joint_names(family: dict[str, Any], role: str) -> list[str]:
    for row in family["semantics"]["resource_role_bindings"]:
        if row["resource_role_id"] == role:
            return list(row["source_joint_names"])
    return []


def control_binding_id(occurrence: str, target: str, capability_id: str) -> str:
    return f"{occurrence}.{target}.{capability_id.replace('.', '-')}"


def domain_id(occurrence: str, target: str) -> str:
    return f"{occurrence}.{target}.control"


def graph_and_applicability(
    deployment: dict[str, Any],
    family: dict[str, Any],
    runtime_restoration_profile: dict[str, Any],
    refs: dict[str, dict[str, str]],
) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    occurrence = deployment["component_occurrence_id"]
    family_id = family["model_family_id"]
    ros2_control = "ros2_control_runtime" in runtime_restoration_profile
    runtime_configuration = runtime_restoration_profile[
        "ros2_control_runtime" if ros2_control else "native_runtime"
    ]
    key_prefix = "ros2_control:" if ros2_control else ""
    component_ref = refs[
        f"component:{'fixed_scale:' if ros2_control else ''}{family_id}"
    ]
    model_ref = refs[f"model:{family_id}"]
    runtime_ref = refs[f"runtime{':ros2_control' if ros2_control else ''}"]
    arm_stop_ref = refs["stop:arm"]
    goal_cancel_stop_ref = refs["stop:goal_cancel"]
    control_ref = refs["provider:ros2_control" if ros2_control else "provider:control"]
    planning_ref = refs[
        "provider:ros2_control" if ros2_control else "provider:planning"
    ]
    logical_scene_ref = refs["provider:logical_scene"]
    runtime_unit_id = (
        f"{occurrence}.ros2_control_execution"
        if ros2_control
        else "robot_cell.native_execution"
    )
    physical_provider_instance_id = (
        f"{occurrence}.ros2_control" if ros2_control else f"{occurrence}.native_control"
    )
    model_instance_id = f"{occurrence}.model"

    arm_resources = [
        {
            "resource_role_id": "arm_joints",
            "resource_id": f"{occurrence}.arm.coordinate.{index:06d}",
        }
        for index, _ in enumerate(source_joint_names(family, "arm_joints"))
    ]
    finger_joints = source_joint_names(family, "fingers")
    gripper_resources = [
        {
            "resource_role_id": "fingers",
            "resource_id": f"{occurrence}.gripper.coordinate.{index:06d}",
        }
        for index, _ in enumerate(finger_joints)
    ] or [
        {
            "resource_role_id": "fingers",
            "resource_id": f"{occurrence}.gripper.logical_position",
        }
    ]

    bindings: list[dict[str, Any]] = []
    for target, capabilities, resources in (
        (
            "arm",
            ARM_CAPABILITIES if ros2_control else NATIVE_ARM_CAPABILITIES,
            arm_resources,
        ),
        ("gripper", GRIPPER_CAPABILITIES, gripper_resources),
    ):
        stop_ref = arm_stop_ref if target == "arm" else goal_cancel_stop_ref
        target_control_ref = control_ref
        target_provider_instance_id = physical_provider_instance_id
        contract = refs[f"contract:{key_prefix}{target}"]
        topology = refs[f"topology:{key_prefix}{target}"]
        for capability_id in capabilities:
            if CAPABILITIES[capability_id][2] != "control_binding":
                continue
            binding = {
                "control_binding_id": control_binding_id(
                    occurrence, target, capability_id
                ),
                "control_binding_revision": ZERO_DIGEST,
                "controllable_component_id": occurrence,
                "command_target_id": target,
                **capability_selector(capability_id),
                "control_domain_id": domain_id(occurrence, target),
                "resource_bindings": copy.deepcopy(resources),
                "control_interface_contract_id": contract["record_id"],
                "control_interface_revision": contract["record_revision"],
                "controller_topology_id": topology["record_id"],
                "topology_revision": topology["record_revision"],
                "legal_control_route_ids": [f"{target}_position"],
                "route_endpoint_projections": [
                    {
                        "control_route_id": f"{target}_position",
                        "command_endpoint_role_ids": ["command"],
                        "state_endpoint_role_ids": ["state"],
                    }
                ],
                "provider_instance_id": target_provider_instance_id,
                "provider_runtime_unit_id": runtime_unit_id,
                "provider_manifest_id": target_control_ref["record_id"],
                "provider_manifest_revision": target_control_ref["record_revision"],
                "state_authority_id": (
                    f"{occurrence}.{target}.commanded_sim_state"
                    if ros2_control
                    else f"{occurrence}.{target}.predicted_state"
                ),
                "state_authority_class": ("commanded" if ros2_control else "predicted"),
                "stop_evidence_policy_id": stop_ref["record_id"],
                "stop_evidence_policy_revision": stop_ref["record_revision"],
                "containment_granularity": "resource",
            }
            if (
                not ros2_control
                and target == "arm"
                and capability_id
                in {
                    "execution.trajectory.execute",
                    "execution.runtime_scale.operation_scoped",
                }
            ):
                binding["runtime_scale_route_qualifications"] = [
                    {
                        "control_route_id": "arm_position",
                        "scaling_domain_id": f"{occurrence}.arm.runtime_scale",
                        "command_endpoint_role_id": "command",
                        "applied_state_endpoint_role_id": "state",
                        "acknowledgement_kind": "applied_state",
                        "maximum_slew_per_second": 4.0,
                        "freshness_budget_ns": 100_000_000,
                    }
                ]
            bindings.append(binding)
    bindings.sort(key=lambda row: row["control_binding_id"])

    graph = {
        "runtime_units": [
            {
                "runtime_unit_id": runtime_unit_id,
                "runtime_profile_id": runtime_ref["record_id"],
                "runtime_profile_revision": runtime_ref["record_revision"],
                "dependency_runtime_unit_ids": [],
                "configuration": copy.deepcopy(runtime_configuration),
            }
        ],
        "model_instances": [
            {
                "model_instance_id": model_instance_id,
                "model_asset_id": model_ref["record_id"],
                "model_revision": model_ref["record_revision"],
            }
        ],
        "controllable_components": [
            {
                "controllable_component_id": occurrence,
                "component_type_id": component_ref["record_id"],
                "component_type_revision": component_ref["record_revision"],
                "model_role_bindings": [
                    {
                        "model_role_id": "arm_model",
                        "model_instance_id": model_instance_id,
                    }
                ],
                "runtime_unit_ids": [runtime_unit_id],
                "root_pose_in_cell": copy.deepcopy(
                    deployment.get(
                        "root_pose_in_cell",
                        {
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        },
                    )
                ),
            }
        ],
        "control_domains": [
            {
                "control_domain_id": domain_id(occurrence, target),
                "control_domain_revision": ZERO_DIGEST,
                "runtime_unit_ids": [runtime_unit_id],
                "control_coordinator_endpoint_role": (
                    "ros2_control_execution_status"
                    if ros2_control
                    else "native_execution_status"
                ),
                "controller_topology_id": refs[f"topology:{key_prefix}{target}"][
                    "record_id"
                ],
                "topology_revision": refs[f"topology:{key_prefix}{target}"][
                    "record_revision"
                ],
                "containment_granularity": "resource",
            }
            for target in ("arm", "gripper")
        ],
        "control_bindings": bindings,
        "component_attachments": [],
    }

    desired: list[dict[str, Any]] = []
    for target in ("arm", "gripper"):
        topology = refs[f"topology:{key_prefix}{target}"]
        selected_bindings = [
            binding for binding in bindings if binding["command_target_id"] == target
        ]
        desired.append(
            {
                "control_domain_id": domain_id(occurrence, target),
                "control_domain_revision": ZERO_DIGEST,
                "controller_topology_id": topology["record_id"],
                "topology_revision": topology["record_revision"],
                "activation_set_id": f"{target}_direct",
                "binding_routes": [
                    {
                        "control_binding_id": binding["control_binding_id"],
                        "control_binding_revision": ZERO_DIGEST,
                        "control_route_id": f"{target}_position",
                    }
                    for binding in selected_bindings
                ],
            }
        )

    provider_bindings: list[dict[str, Any]] = []
    anchors = {
        "arm": control_binding_id(occurrence, "arm", "execution.trajectory.execute"),
        "gripper": control_binding_id(occurrence, "gripper", "tool.gripper.command"),
    }
    for target, capabilities in (
        ("arm", ARM_CAPABILITIES),
        ("gripper", GRIPPER_CAPABILITIES),
    ):
        for capability_id in capabilities:
            if CAPABILITIES[capability_id][2] != "capability_provider_binding":
                continue
            provider_bindings.append(
                {
                    "capability_provider_binding_id": (
                        f"{occurrence}.{target}.{capability_id.replace('.', '-')}.provider"
                    ),
                    "capability_provider_binding_revision": ZERO_DIGEST,
                    **capability_selector(capability_id),
                    "scope": "target_binding",
                    "target": {
                        "controllable_component_id": occurrence,
                        "component_type_revision": component_ref["record_revision"],
                        "command_target_id": target,
                        "control_binding_id": anchors[target],
                        "control_binding_revision": ZERO_DIGEST,
                    },
                    "provider_instance_id": (
                        physical_provider_instance_id
                        if ros2_control
                        else f"{occurrence}.native_planning"
                    ),
                    "provider_manifest_id": planning_ref["record_id"],
                    "provider_manifest_revision": planning_ref["record_revision"],
                    "endpoint_bindings": [
                        {
                            "endpoint_role_id": "planner_scene_projection",
                            "source_kind": "runtime_unit",
                            "source_id": runtime_unit_id,
                            "source_endpoint_role_id": "planner_scene_projection",
                        }
                    ],
                }
            )
    for capability_id in CELL_CAPABILITIES:
        provider_bindings.append(
            {
                "capability_provider_binding_id": (
                    f"{deployment['cell_id']}.{capability_id.replace('.', '-')}.provider"
                ),
                "capability_provider_binding_revision": ZERO_DIGEST,
                **capability_selector(capability_id),
                "scope": "cell_global",
                "target": {"cell_id": deployment["cell_id"]},
                "provider_instance_id": f"{deployment['cell_id']}.logical_scene",
                "provider_manifest_id": logical_scene_ref["record_id"],
                "provider_manifest_revision": logical_scene_ref["record_revision"],
                "endpoint_bindings": [
                    {
                        "endpoint_role_id": "logical_scene_services",
                        "source_kind": "runtime_unit",
                        "source_id": runtime_unit_id,
                        "source_endpoint_role_id": "logical_scene_services",
                    }
                ],
            }
        )
    provider_bindings.sort(key=lambda row: row["capability_provider_binding_id"])
    return graph, desired, provider_bindings


def operator_stop_contract_parameters(
    family: dict[str, Any],
    runtime_restoration_profile: dict[str, Any],
    binding: dict[str, Any],
) -> dict[str, Any]:
    profile = runtime_restoration_profile["stop_profiles"]["operator_stop_profile"]
    if (
        profile["profile_id"] != "operator_soft_on_path"
        or profile["stop_mode"] != "soft_on_path"
        or profile.get("require_backend_confirmation", True) is not True
    ):
        raise GenerationError(
            "maintained ordinary arm Stop must select operator_soft_on_path "
            "with backend confirmation"
        )
    source_joints = source_joint_names(family, "arm_joints")
    resources = binding["resource_bindings"]
    acceleration = profile["per_joint_max_deceleration_rad_s2"]
    if family["model_family_id"] == LITE6_NATIVE_MODEL_FAMILY_ID:
        expected_acceleration = {joint: 4.0 for joint in source_joints}
        fault_profile = runtime_restoration_profile["stop_profiles"][
            "default_fault_stop_profile"
        ]
        if (
            profile.get("deceleration_time_s") != 1.0
            or profile.get("allow_natural_completion_on_short_path") is not True
            or "recoverable_substitution_mode" in profile
            or acceleration != expected_acceleration
            or fault_profile.get("profile_id") != "fault_spring_back_hold"
            or fault_profile.get("stop_mode") != "spring_back_hold"
            or fault_profile.get("deceleration_time_s") != 1.0
            or fault_profile.get("per_joint_max_deceleration_rad_s2")
            != expected_acceleration
            or fault_profile.get("require_backend_confirmation", True) is not True
        ):
            raise GenerationError(
                "Lite6 retained Stop profiles differ from the qualified "
                "operator_soft_on_path/fault_spring_back_hold contract"
            )
    if len(source_joints) != len(resources) or any(
        joint not in acceleration for joint in source_joints
    ):
        raise GenerationError(
            "operator Stop acceleration limits do not cover the exact arm scope"
        )
    resource_limits = [
        {
            "resource_id": resource["resource_id"],
            "max_deceleration": {
                "coordinate_kind": "angular_rad_s2",
                "value_rad_s2": acceleration[joint],
            },
            "settle_velocity_threshold": {
                "coordinate_kind": "angular_rad_s",
                "value_rad_s": 0.01,
            },
        }
        for joint, resource in zip(source_joints, resources, strict=True)
    ]
    resource_limits.sort(key=lambda row: row["resource_id"])
    result = {
        "stop_mode": "soft_on_path",
        # Explicitly preserve the old maintained examples' Stop-only shaping.
        # User Deployments without this choice require migration/review.
        "jerk_limits_enabled": True,
        "deceleration_time_s": profile["deceleration_time_s"],
        "per_resource_limits": resource_limits,
        "allow_natural_completion_on_short_path": profile[
            "allow_natural_completion_on_short_path"
        ],
    }
    if "recoverable_substitution_mode" in profile:
        result["recoverable_substitution_mode"] = profile[
            "recoverable_substitution_mode"
        ]
    return result


def stop_policy_projection(
    graph: dict[str, Any],
    family: dict[str, Any],
    runtime_restoration_profile: dict[str, Any],
) -> dict[str, Any]:
    domains = {row["control_domain_id"]: row for row in graph["control_domains"]}
    stop_stage_parameters: list[dict[str, Any]] = []
    for binding in graph["control_bindings"]:
        domain = domains[binding["control_domain_id"]]
        for route_id in binding["legal_control_route_ids"]:
            if binding["command_target_id"] == "arm":
                stop_stage_parameters.append(
                    {
                        "control_domain_id": binding["control_domain_id"],
                        "control_domain_revision": domain["control_domain_revision"],
                        "control_binding_id": binding["control_binding_id"],
                        "control_binding_revision": binding["control_binding_revision"],
                        "controller_topology_id": binding["controller_topology_id"],
                        "topology_revision": binding["topology_revision"],
                        "control_route_id": route_id,
                        "stop_evidence_policy_id": binding["stop_evidence_policy_id"],
                        "stop_evidence_policy_revision": binding[
                            "stop_evidence_policy_revision"
                        ],
                        "stop_escalation_stage_id": "motion_controlled_stop",
                        "provider_contract_ref": (
                            motion_controlled_stop_provider_contract_ref()
                        ),
                        "default_contract_parameters": (
                            operator_stop_contract_parameters(
                                family, runtime_restoration_profile, binding
                            )
                        ),
                    }
                )
            stop_stage_parameters.append(
                {
                    "control_domain_id": binding["control_domain_id"],
                    "control_domain_revision": domain["control_domain_revision"],
                    "control_binding_id": binding["control_binding_id"],
                    "control_binding_revision": binding["control_binding_revision"],
                    "controller_topology_id": binding["controller_topology_id"],
                    "topology_revision": binding["topology_revision"],
                    "control_route_id": route_id,
                    "stop_evidence_policy_id": binding["stop_evidence_policy_id"],
                    "stop_evidence_policy_revision": binding[
                        "stop_evidence_policy_revision"
                    ],
                    "stop_escalation_stage_id": "software_hard",
                    "provider_contract_ref": software_hard_provider_contract_ref(),
                    "default_contract_parameters": {},
                }
            )
    stop_stage_parameters.sort(
        key=lambda row: (
            row["control_domain_id"],
            row["control_binding_id"],
            row["control_route_id"],
            row["stop_escalation_stage_id"],
        )
    )
    return {
        "retained_policy_records": [],
        "stop_stage_parameters": stop_stage_parameters,
        # Explicit new example settings, not a reinterpretation of the old
        # catalog's outer envelope. ROS/controller-backed examples retain a
        # larger settling allowance; provider qualification is still required.
        "stop_timing_parameters": [
            {
                "control_binding_id": binding["control_binding_id"],
                "control_binding_revision": binding["control_binding_revision"],
                "timing": {
                    "command_latency_allowance_ns": 250_000_000,
                    "settling_allowance_ns": (
                        1_500_000_000
                        if "ros2_control_runtime" in runtime_restoration_profile
                        else 750_000_000
                    ),
                    "operational_timeout_ns": 3_000_000_000,
                },
            }
            for binding in sorted(
                graph["control_bindings"], key=lambda row: row["control_binding_id"]
            )
        ],
        "commissioned_stop_interfaces": [],
        "interrupt_sources": [],
        "interrupt_policies": [],
        "interrupt_config": {
            "queue_depth": 8,
            "cancel_validation_timeout_ns": 1_000,
            "motion_scope_arm_timeout_ns": 1_000,
            "supervisory_poll_rate_hz": 10,
        },
    }


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


def assistant_projection(
    profile: dict[str, Any],
    profiles: dict[str, Any],
    refs: dict[str, dict[str, str]],
) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    model_aliases: dict[str, Any] = {}
    external_resources: list[dict[str, Any]] = []
    model_provider = refs["provider:model_endpoint"]
    bridge_provider = refs["provider:assistant_bridge"]
    for endpoint in profile["model_endpoints"]:
        alias = endpoint["model_endpoint_alias"]
        model_aliases[alias] = {
            "external_resource_alias": alias,
            "model_id": endpoint["model_id"],
            "request_contract": copy.deepcopy(endpoint["request_contract"]),
        }
        external_resources.append(
            {
                "external_resource_alias": alias,
                "resource_kind": "model_endpoint",
                "provider_instance_id": f"{profile['profile_id']}.{alias}.provider",
                "provider_manifest_id": model_provider["record_id"],
                "provider_manifest_revision": model_provider["record_revision"],
                "endpoint_role_id": "model_inference",
                "site_overlay_value_ref": endpoint["site_overlay_value_ref"],
            }
        )

    provider_aliases: dict[str, Any] = {}
    provider = profile["provider_alias"]
    if provider is not None:
        provider_aliases[provider["provider_alias_id"]] = {
            "provider_kind": provider["provider_kind"],
            "bridge_external_resource_alias": provider[
                "bridge_external_resource_alias"
            ],
        }
        external_resources.append(
            {
                "external_resource_alias": provider["bridge_external_resource_alias"],
                "resource_kind": "service_endpoint",
                "provider_instance_id": f"{profile['profile_id']}.bridge.provider",
                "provider_manifest_id": bridge_provider["record_id"],
                "provider_manifest_revision": bridge_provider["record_revision"],
                "endpoint_role_id": "assistant_request",
                "site_overlay_value_ref": provider["site_overlay_value_ref"],
            }
        )

    modes: dict[str, Any] = {}
    mode_profile_id = profile["mode_profile_id"]
    if mode_profile_id is not None:
        if provider is None or "assistant_model" not in model_aliases:
            raise GenerationError(
                f"assistant profile {profile['profile_id']} has incomplete mode aliases"
            )
        for mode_id, source in profiles["mode_profiles"][mode_profile_id].items():
            mode = copy.deepcopy(source)
            mode["provider_alias"] = provider["provider_alias_id"]
            mode["model_endpoint_alias"] = "assistant_model"
            modes[mode_id] = mode

    external_resources.sort(key=lambda row: row["external_resource_alias"])
    return (
        {
            "audit_policy": copy.deepcopy(profile["audit_policy"]),
            "prompt_templates": copy.deepcopy(profile["prompt_templates"]),
            "model_endpoint_aliases": model_aliases,
            "provider_aliases": provider_aliases,
            "modes": modes,
        },
        external_resources,
    )


def deployment_candidate(
    deployment: dict[str, Any],
    family: dict[str, Any],
    profiles_by_id: dict[str, dict[str, Any]],
    runtime_profiles_by_id: dict[str, dict[str, Any]],
    profiles: dict[str, Any],
    refs: dict[str, dict[str, str]],
    overlay_ref: dict[str, str],
) -> dict[str, Any]:
    runtime_restoration_profile = runtime_profiles_by_id[
        deployment["runtime_restoration_profile_id"]
    ]
    ros2_control = "ros2_control_runtime" in runtime_restoration_profile
    key_prefix = "ros2_control:" if ros2_control else ""
    graph, desired, provider_bindings = graph_and_applicability(
        deployment,
        family,
        runtime_restoration_profile,
        refs,
    )
    family_id = family["model_family_id"]
    catalog_keys = {
        *(
            f"capability:{capability_id}"
            for capability_id in (
                ARM_CAPABILITIES if ros2_control else NATIVE_ARM_CAPABILITIES
            )
        ),
        *(f"capability:{capability_id}" for capability_id in GRIPPER_CAPABILITIES),
        *(f"capability:{capability_id}" for capability_id in CELL_CAPABILITIES),
        *(
            f"capability:component.lifecycle.{intent}"
            for intent in (
                LIFECYCLE_INTENTS
                if ros2_control
                else ("disable", "enable", "observe", "prepare")
            )
        ),
        "command_schema",
        f"component:{'fixed_scale:' if ros2_control else ''}{family_id}",
        f"contract:{key_prefix}arm",
        f"contract:{key_prefix}gripper",
        f"model:{family_id}",
        "provider:logical_scene",
        ("provider:ros2_control" if ros2_control else "provider:control"),
        *([] if ros2_control else ["provider:planning"]),
        ("runtime:ros2_control" if ros2_control else "runtime"),
        "stop:arm",
        "stop:goal_cancel",
        f"topology:{key_prefix}arm",
        f"topology:{key_prefix}gripper",
    }
    profile_id = deployment["assistant_profile_id"]
    if profile_id is not None:
        profile = profiles_by_id[profile_id]
        if profile["model_endpoints"]:
            catalog_keys.update(
                {
                    "capability:assistant.model.invoke",
                    "provider:model_endpoint",
                }
            )
        if profile["provider_alias"] is not None:
            catalog_keys.update(
                {
                    "capability:assistant.bridge.request",
                    "provider:assistant_bridge",
                }
            )
    records = [refs[key] for key in catalog_keys]
    records.sort(
        key=lambda row: (
            row["record_kind"],
            row["record_id"],
            row["record_revision"],
        )
    )
    value: dict[str, Any] = {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": deployment["deployment_artifact_id"],
        "deployment_revision": ZERO_DIGEST,
        "cell_id": deployment["cell_id"],
        "name": deployment["name"],
        "description": deployment["description"],
        "catalogs": {"records": records},
        "graph": graph,
        "desired_control_configurations": desired,
        "capability_provider_bindings": provider_bindings,
        "policies": stop_policy_projection(
            graph,
            family,
            runtime_restoration_profile,
        ),
        "compatibility": copy.deepcopy(deployment["compatibility"]),
    }
    if profile_id is not None:
        assistant, resources = assistant_projection(
            profiles_by_id[profile_id], profiles, refs
        )
        value["site_overlay_ref"] = copy.deepcopy(overlay_ref)
        value["external_resources"] = resources
        value["assistant"] = assistant
    return value


def refresh_cross_revisions(path: Path) -> None:
    value = read_json(path, "first-pass deployment")
    domains = {
        row["control_domain_id"]: row for row in value["graph"]["control_domains"]
    }
    bindings = {
        row["control_binding_id"]: row for row in value["graph"]["control_bindings"]
    }
    for configuration in value["desired_control_configurations"]:
        configuration["control_domain_revision"] = domains[
            configuration["control_domain_id"]
        ]["control_domain_revision"]
        for route in configuration["binding_routes"]:
            route["control_binding_revision"] = bindings[route["control_binding_id"]][
                "control_binding_revision"
            ]
    for provider in value["capability_provider_bindings"]:
        target = provider["target"]
        if provider["scope"] == "target_binding":
            target["control_binding_revision"] = bindings[target["control_binding_id"]][
                "control_binding_revision"
            ]
    for parameters in value["policies"]["stop_stage_parameters"]:
        binding = bindings[parameters["control_binding_id"]]
        domain = domains[parameters["control_domain_id"]]
        parameters["control_binding_revision"] = binding["control_binding_revision"]
        parameters["control_domain_revision"] = domain["control_domain_revision"]
    for selected in value["policies"].get("stop_timing_parameters", []):
        selected["control_binding_revision"] = bindings[selected["control_binding_id"]][
            "control_binding_revision"
        ]
    replace_file(path, pretty_bytes(value))


def run_revision_tool(tool: Path, mode: str, source: Path) -> None:
    result = subprocess.run(
        [str(tool), mode, "--input", str(source)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise GenerationError(
            f"deployment revision tool failed for {source}: {result.stderr.strip()}"
        )


def sealed_deployment_bytes(
    tool: Path, candidate: dict[str, Any], temporary_root: Path
) -> bytes:
    source = temporary_root / "candidate.deployment.json"
    source.write_bytes(pretty_bytes(candidate))
    run_revision_tool(tool, "--update", source)
    refresh_cross_revisions(source)
    run_revision_tool(tool, "--update", source)
    run_revision_tool(tool, "--check", source)
    return source.read_bytes()


def _program_draft(program: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(program)
    tree = result.pop("tree")
    result.pop("program_revision")
    return {
        "schema_version": "manyforge.composer_authoring_forest.v1",
        **result,
        "forest": [tree],
        "positions": {},
    }


def _role_assignment(assignment: dict[str, Any]) -> dict[str, Any]:
    return {
        "component_roles": [
            {
                "component_role_id": role,
                "controllable_component_id": component,
            }
            for role, component in sorted(assignment["component_roles"].items())
        ],
        "target_roles": [
            {
                "target_role_id": role,
                "command_target_id": target["command_target_id"],
            }
            for role, target in sorted(assignment["target_roles"].items())
        ],
    }


def _project_reference(
    root: Path, project_root: Path, relative_source: str
) -> dict[str, str]:
    target = (root / relative_source).resolve()
    try:
        target.relative_to(root)
    except ValueError as error:
        raise GenerationError(
            f"maintained source escapes the workspace: {relative_source}"
        ) from error
    return {"project_path": os.path.relpath(target, project_root).replace(os.sep, "/")}


def _deployment_intent(
    *,
    root: Path,
    project_root: Path,
    deployment: dict[str, Any],
    family: dict[str, Any],
    runtime_profile: dict[str, Any],
) -> dict[str, Any]:
    """Project the maintained source description into editable intent.

    The generated Deployment graph remains an offline qualification product.
    Scenario projects retain only source references and the settings a user can
    meaningfully author; preparation derives graph/provider/binding identities.
    """

    source = family["source"]
    accepted_prefix = source["accepted_package_uri_prefix"]
    if not accepted_prefix.startswith("package://"):
        raise GenerationError("maintained model source has no package URI prefix")
    package_reference = accepted_prefix[len("package://") :]
    package_name, _separator, package_suffix = package_reference.partition("/")
    if not package_name:
        raise GenerationError("maintained model source has an empty package name")
    resource_root = (root / source["resource_root"]).resolve()
    try:
        resource_root.relative_to(root)
    except ValueError as error:
        raise GenerationError(
            "maintained model resource root escapes the workspace"
        ) from error
    suffix_parts = tuple(part for part in package_suffix.split("/") if part)
    if suffix_parts and resource_root.parts[-len(suffix_parts) :] != suffix_parts:
        raise GenerationError(
            "maintained model resource root does not match its package URI prefix"
        )
    package_root = resource_root
    for _unused in suffix_parts:
        package_root = package_root.parent
    if package_root.name != package_name:
        raise GenerationError(
            "maintained model resource root does not identify its declared package"
        )
    if not package_root.is_dir():
        raise GenerationError(
            f"maintained package root is unavailable: {package_root}"
        )

    runtime_kind = (
        "ros2_control" if "ros2_control_runtime" in runtime_profile else "native"
    )
    runtime_key = f"{runtime_kind}_runtime"
    runtime_configuration = copy.deepcopy(runtime_profile[runtime_key])
    motion_limits = runtime_configuration.pop("motion_target_limits", [])
    target_settings: dict[str, dict[str, Any]] = {}
    for limit in motion_limits:
        target = limit["command_target_id"]
        selected = target_settings.setdefault(target, {})
        selected["maximum_tcp_linear_speed_mps"] = limit[
            "maximum_tcp_linear_speed_mps"
        ]
        if "ordinary_motion_jerk" in limit:
            selected["ordinary_motion_jerk"] = copy.deepcopy(
                limit["ordinary_motion_jerk"]
            )

    operator_stop = runtime_profile["stop_profiles"]["operator_stop_profile"]
    source_joints = source_joint_names(family, "arm_joints")
    decelerations = operator_stop["per_joint_max_deceleration_rad_s2"]
    if not source_joints or not set(source_joints).issubset(decelerations):
        raise GenerationError(
            "maintained operator Stop limits do not cover the model arm joints"
        )
    target_settings.setdefault("arm", {})["controlled_stop"] = {
        "stop_mode": operator_stop["stop_mode"],
        "allow_natural_completion_on_short_path": operator_stop[
            "allow_natural_completion_on_short_path"
        ],
        "jerk_limits_enabled": True,
        "deceleration_time_s": operator_stop["deceleration_time_s"],
        "per_joint_limits": [
            {
                "source_joint_name": joint,
                "max_deceleration_rad_s2": decelerations[joint],
                "settle_velocity_threshold_rad_s": 0.01,
            }
            for joint in source_joints
        ],
        "timing": {
            "command_latency_allowance_ns": 250_000_000,
            "settling_allowance_ns": (
                1_500_000_000 if runtime_kind == "ros2_control" else 750_000_000
            ),
            "operational_timeout_ns": 3_000_000_000,
        },
    }

    model_source: dict[str, Any] = {
        "urdf": _project_reference(root, project_root, source["urdf_path"]),
        "package_roots": {
            package_name: {
                "project_path": os.path.relpath(package_root, project_root).replace(
                    os.sep, "/"
                )
            }
        },
    }
    if source.get("srdf_path"):
        model_source["srdf"] = _project_reference(
            root, project_root, source["srdf_path"]
        )

    root_pose = deployment.get("root_pose_in_cell")
    if root_pose is None:
        root_pose = {
            "position_m": [0.0, 0.0, 0.0],
            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
        }
    result: dict[str, Any] = {
        "api_version": DEPLOYMENT_INTENT_API,
        "deployment_artifact_id": deployment["deployment_artifact_id"],
        "name": deployment["name"],
        "description": deployment["description"],
        "components": [
            {
                "component_id": deployment["component_occurrence_id"],
                "component_profile_id": deployment[
                    "runtime_restoration_profile_id"
                ],
                "model_source": model_source,
                "root_pose_in_cell": copy.deepcopy(root_pose),
                "target_settings": target_settings,
                "configuration": {
                    "runtime_kind": runtime_kind,
                    "runtime_configuration": runtime_configuration,
                    "default_fault_stop_profile": copy.deepcopy(
                        runtime_profile["stop_profiles"][
                            "default_fault_stop_profile"
                        ]
                    ),
                },
            }
        ],
        "attachments": [],
        "policies": {
            "interrupt_config": {
                "queue_depth": 8,
                "supervisory_poll_rate_hz": 10,
                "cancel_validation_timeout_ns": 1_000,
                "motion_scope_arm_timeout_ns": 1_000,
            }
        },
        "assistant_profile_id": deployment["assistant_profile_id"],
        "compatibility": copy.deepcopy(deployment["compatibility"]),
    }
    return result


def _scenario_project_outputs(
    *,
    project_root: Path,
    scenario_id: str,
    name: str,
    description: str,
    program: dict[str, Any],
    deployment_intent: dict[str, Any],
    assignment: dict[str, Any],
    manyforge_version: str,
) -> list[tuple[Path, bytes, str]]:
    program_draft = _program_draft(program)
    role_assignment = _role_assignment(assignment)
    content = {
        "program_draft": program_draft,
        "deployment_draft": deployment_intent,
        "program_role_assignment": role_assignment,
    }
    saved_status = {
        "manyforge_version": manyforge_version,
        "last_known_status": "ready",
        "checked_content_digest": raw_sha256_digest(canonical_json_bytes(content)),
    }
    members = {
        "deployment.yaml": pretty_bytes(deployment_intent),
        "program.yaml": pretty_bytes(program_draft),
        "assignment.yaml": pretty_bytes(role_assignment),
    }
    source_entries: list[dict[str, str]] = []
    source_closure = capture_deployment_model_sources(
        deployment_intent, project_root=project_root
    )
    for component in source_closure["components"]:
        component_id = component["component_id"]
        for entrypoint, artifact in sorted(component["entrypoints"].items()):
            source_entries.append(
                {
                    "source_id": f"component:{component_id}:{entrypoint}",
                    "content_digest": artifact["content_digest"],
                }
            )
        for artifact in component["resources"]:
            source_entries.append(
                {
                    "source_id": (
                        f"component:{component_id}:resource:"
                        f"{artifact['logical_uri']}"
                    ),
                    "content_digest": artifact["content_digest"],
                }
            )
    source_entries.sort(key=lambda row: row["source_id"])
    manifest = {
        "api_version": SCENARIO_PROJECT_API,
        "scenario_id": scenario_id,
        "name": name,
        "description": description,
        "content": {
            "deployment_present": True,
            "program_present": True,
            "assignment_present": True,
        },
        "members": {
            filename: raw_sha256_digest(payload)
            for filename, payload in sorted(members.items())
        },
        "source_snapshot": {
            "schema_version": SCENARIO_SOURCE_SNAPSHOT_API,
            "entries": source_entries,
        },
        "saved_status": saved_status,
    }
    outputs = [
        (
            project_root / filename,
            payload,
            f"maintained Scenario {scenario_id} member {filename}",
        )
        for filename, payload in sorted(members.items())
    ]
    outputs.append(
        (
            project_root / "scenario.yaml",
            pretty_bytes(manifest),
            f"maintained Scenario {scenario_id} manifest",
        )
    )
    return outputs


def scenario_documents(
    root: Path,
    description: dict[str, Any],
    sealed: dict[str, dict[str, Any]],
) -> list[tuple[Path, bytes, str]]:
    version = (root / "VERSION").read_text(encoding="utf-8").strip()
    outputs: list[tuple[Path, bytes, str]] = []
    families = {
        row["model_family_id"]: row for row in description["model_families"]
    }
    runtime_profiles = {
        row["runtime_restoration_profile_id"]: row
        for row in description["runtime_restoration_profiles"]
    }
    for deployment in description["deployments"]:
        # Keep the compiled Deployment output for offline qualification only.
        # The Scenario project intentionally stores the editable source intent.
        if deployment["deployment_source"] not in sealed:
            raise GenerationError("maintained Deployment was not sealed")
        for assignment in deployment["program_role_assignments"]:
            program = read_yaml(
                root / assignment["program_source"], "maintained Program"
            )
            project_root = root / "examples" / f"{assignment['scenario_id']}.scenario"
            intent = _deployment_intent(
                root=root,
                project_root=project_root,
                deployment=deployment,
                family=families[deployment["model_family_id"]],
                runtime_profile=runtime_profiles[
                    deployment["runtime_restoration_profile_id"]
                ],
            )
            outputs.extend(
                _scenario_project_outputs(
                    project_root=project_root,
                    scenario_id=assignment["scenario_id"],
                    name=deployment["name"],
                    description=deployment["description"],
                    program=program,
                    deployment_intent=intent,
                    assignment=assignment,
                    manyforge_version=version,
                )
            )
    return outputs


def historical_capability_closure() -> dict[str, Any]:
    active_ids = sorted(
        set(ARM_CAPABILITIES + GRIPPER_CAPABILITIES + CELL_CAPABILITIES)
    )
    if len(active_ids) != 36:
        raise GenerationError(
            "historical capability closure must contain exactly 36 active IDs"
        )
    rows: list[dict[str, Any]] = []
    for capability_id in active_ids:
        scope = CAPABILITIES[capability_id][1]
        binding_kind = CAPABILITIES[capability_id][2]
        if scope == "cell_global":
            owner_boundary = "logical_scene"
            applicability = ["cell_global"]
        elif binding_kind == "control_binding":
            owner_boundary = "native_control"
            applicability = [
                target
                for target, capabilities in (
                    ("arm", ARM_CAPABILITIES),
                    ("gripper", GRIPPER_CAPABILITIES),
                )
                if capability_id in capabilities
            ]
        else:
            owner_boundary = "native_planning"
            applicability = [
                target
                for target, capabilities in (
                    ("arm", ARM_CAPABILITIES),
                    ("gripper", GRIPPER_CAPABILITIES),
                )
                if capability_id in capabilities
            ]
        if not applicability:
            raise GenerationError(
                f"historical capability {capability_id} has no exact applicability"
            )
        acceptance_steps = ["N6-a3", "N7"]
        if capability_id in I8_REQUALIFIED_CAPABILITIES:
            acceptance_steps.append("I8/P5")
        rows.append(
            {
                "historical_capability_id": LEGACY_CAPABILITY_REPLACEMENTS.get(
                    capability_id, capability_id
                ),
                "active_capability_id": capability_id,
                "capability_definition_revision": CAPABILITIES[capability_id][0],
                "scope": scope,
                "binding_kind": binding_kind,
                "applicability": applicability,
                "owner_boundary": owner_boundary,
                "acceptance_steps": acceptance_steps,
                "runtime_claim_gate": (
                    "I8/P5" if capability_id == "execution.stop.soft" else "N7"
                ),
                "disposition": "retained_binding_only_until_runtime_claim_gate",
            }
        )
    return {
        "api_version": "manyforge.maintained_historical_capability_closure.v1",
        "historical_source_commit": "c4b075fa6dfb69e463c61f3efa8cfce8de681e8a",
        "capabilities": rows,
    }


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
    if args.revision_tool.is_symlink() or not args.revision_tool.is_file():
        raise GenerationError("revision tool must select one plain executable")

    source_rows, component_refs, lifecycle_outputs = catalog_sources(root, description)
    del component_refs
    refs = {key: value[2] for key, value in source_rows.items()}
    outputs: list[tuple[Path, bytes, str]] = []
    outputs.extend(lifecycle_outputs)
    for key, (path, payload, unused_ref) in source_rows.items():
        del unused_ref
        if key.startswith("model:") or key == "command_schema":
            continue
        outputs.append((path, payload, "maintained catalog source"))

    overlay_path, overlay_payload, overlay_ref = site_overlay(root, profiles)
    outputs.append((overlay_path, overlay_payload, "maintained site overlay"))

    family_by_id = {
        family["model_family_id"]: family for family in description["model_families"]
    }
    profiles_by_id = {
        profile["profile_id"]: profile for profile in profiles["profiles"]
    }
    runtime_profiles_by_id = {
        profile["runtime_restoration_profile_id"]: profile
        for profile in description["runtime_restoration_profiles"]
    }
    sealed_values: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="manyforge-n6-deployments-") as temporary:
        temporary_root = Path(temporary)
        for deployment in description["deployments"]:
            candidate = deployment_candidate(
                deployment,
                family_by_id[deployment["model_family_id"]],
                profiles_by_id,
                runtime_profiles_by_id,
                profiles,
                refs,
                overlay_ref,
            )
            expected = sealed_deployment_bytes(
                args.revision_tool.resolve(), candidate, temporary_root
            )
            target = root / deployment["deployment_source"]
            outputs.append((target, expected, "direct deployment source"))
            sealed_values[deployment["deployment_source"]] = json.loads(expected)

    outputs.extend(scenario_documents(root, description, sealed_values))
    capability_closure_path = (
        root
        / "manyforge_deployment/resources/maintained/maintained_historical_capability_closure.v1.json"
    )
    outputs.append(
        (
            capability_closure_path,
            pretty_bytes(historical_capability_closure()),
            "maintained historical capability closure",
        )
    )
    for path, payload, label in outputs:
        require_or_update(path, payload, args.update, label)
    for deployment in description["deployments"]:
        for assignment in deployment["program_role_assignments"]:
            require_obsolete_output_absent(
                root / "examples" / f"{assignment['scenario_id']}.scenario.yaml",
                update=args.update,
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--update", action="store_true")
    parser.add_argument("--description", type=Path, required=True)
    parser.add_argument("--assistant-profiles", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    parser.add_argument("--revision-tool", type=Path, required=True)
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
