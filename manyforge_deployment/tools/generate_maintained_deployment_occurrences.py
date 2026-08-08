#!/usr/bin/env python3
"""Generate and verify the seven direct N6 deployment occurrences.

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

from check_maintained_deployment_sources import (
    SourceDescriptionError,
    validate as validate_maintained_description,
)


ZERO_DIGEST: Final = "sha256:" + "0" * 64
DESCRIPTION_API: Final = "manyforge.maintained_deployment_sources.v1"
ASSISTANT_API: Final = "manyforge.maintained_assistant_profiles.v1"
ROLE_ASSIGNMENTS_API: Final = "manyforge.maintained_program_role_assignments.v1"
CONTENT_IDENTITY_PREFIX: Final = b"manyforge.content-identity.v1\0"

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
    "execution.feedback.phase_events": (
        "sha256:37a4b6d145a96b0336ad03680c3222a63da28168764663cee7f5185fcbd467da",
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
}

ARM_CAPABILITIES: Final = tuple(
    sorted(
        {
            "execution.feedback.phase_events",
            "execution.trajectory.execute",
            "model.kinematics.fk",
            "planning.motion.cartesian_path",
            "planning.motion.joint_goal",
            "planning.motion.pose_goal",
            "planning.profile.balanced",
            "planning.profile.first_feasible",
            "planning.trajectory.time_parameterized",
            "state.observed.current",
            "validation.trajectory.non_collision",
        }
    )
)
GRIPPER_CAPABILITIES: Final = tuple(
    sorted(
        {
            "model.kinematics.fk",
            "tool.gripper.command",
            "tool.gripper.feedback",
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
    "stop_evidence_policy": "manyforge_deployment/resources/closed_schema/catalog/manyforge.stop_evidence_policy.v1.yaml",
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
) -> tuple[Path, bytes, dict[str, str]]:
    payload = pretty_bytes(value)
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


def component_type_document(family: dict[str, Any]) -> dict[str, Any]:
    family_id = family["model_family_id"]
    semantics = family["semantics"]
    group_roles = {
        row["model_group_role_id"]
        for row in semantics["model_group_role_bindings"]
    }
    resource_roles = {
        row["resource_role_id"]
        for row in semantics["resource_role_bindings"]
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
    value = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": f"{family_id}.component",
        "tags": ["articulated-arm", "gripper"],
        "model_roles": [
            {"model_role_id": "arm_model", "cardinality": "exactly_one"}
        ],
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
                    for capability_id in ARM_CAPABILITIES
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


def control_contract(target: str) -> dict[str, Any]:
    role = "arm_joints" if target == "arm" else "fingers"
    value = {
        "api_version": "manyforge.control_interface_contract.v1",
        "control_interface_contract_id": f"manyforge.native_{target}_position",
        "command_interfaces": [
            {"resource_role_id": role, "interface_name": "position"}
        ],
        "state_interfaces": [
            {"resource_role_id": role, "interface_name": "position"}
        ],
        "ordering": [role],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/control_interface_contract",
        api_version="manyforge.control_interface_contract.v1",
        revision_field="control_interface_revision",
    )


def controller_topology(target: str, contract: dict[str, Any]) -> dict[str, Any]:
    activation = f"{target}_direct"
    route = f"{target}_position"
    value = {
        "api_version": "manyforge.controller_topology.v1",
        "controller_topology_id": f"manyforge.native_{target}_direct",
        "controllers": [
            {
                "controller_role_id": "native_execution",
                "control_interface_claims": [
                    {
                        "control_interface_contract_id": contract[
                            "control_interface_contract_id"
                        ],
                        "control_interface_revision": contract[
                            "control_interface_revision"
                        ],
                        "claimed_directions": ["command", "state"],
                    }
                ],
                "exported_reference_interfaces": [],
                "exported_state_interfaces": [],
            }
        ],
        "chain_edges": [],
        "activation_sets": [
            {
                "activation_set_id": activation,
                "active_controller_role_ids": ["native_execution"],
                "active_chain_edges": [],
            }
        ],
        "legal_routes": [
            {
                "control_route_id": route,
                "control_interface_contract_id": contract[
                    "control_interface_contract_id"
                ],
                "control_interface_revision": contract[
                    "control_interface_revision"
                ],
                "ordered_controller_role_ids": ["native_execution"],
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


def provider_manifest(
    provider_id: str, capability_ids: tuple[str, ...], endpoint_roles: list[str]
) -> dict[str, Any]:
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
        "provider_contracts": [],
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/provider_manifest",
        api_version="manyforge.provider_manifest.v1",
        revision_field="provider_manifest_revision",
    )


def stop_policy() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.stop_evidence_policy.v1",
        "stop_evidence_policy_id": "manyforge.native_controller_goal_terminal",
        "disposition": "stoppable",
        "containment_granularity": "resource",
        "containment_command": {
            "command_phase_id": "command",
            "command": {"command_kind": "controller_goal_cancel"},
        },
        "deadline_budget": {
            "outer_duration_ns": 2_000_000_000,
            "kernel_completion_reserve_ns": 100_000_000,
            "phases": [
                {"phase_id": "command", "deadline_offset_ns": 500_000_000},
                {"phase_id": "terminal", "deadline_offset_ns": 1_900_000_000},
            ],
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
    }
    return revisioned_document(
        value,
        domain="catalog-record-revision/stop_evidence_policy",
        api_version="manyforge.stop_evidence_policy.v1",
        revision_field="stop_evidence_policy_revision",
    )


def native_runtime_profile() -> dict[str, Any]:
    value = {
        "api_version": "manyforge.runtime_profile.v1",
        "runtime_profile_id": "manyforge.native_execution_runtime",
        "configuration_schema_ref": {
            "schema_id": "manyforge.value_schema.runtime.empty.v1",
            "schema_revision": "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785",
        },
        "process_contract": {"lifecycle_control": "manager_managed"},
        "supervision_contract": {
            "status_endpoint_role_id": "native_execution_status"
        },
        "offered_runtime_role_ids": ["native_execution"],
        "source_endpoint_role_ids": [
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


def catalog_sources(
    root: Path,
    description: dict[str, Any],
) -> tuple[
    dict[str, tuple[Path, bytes, dict[str, str]]],
    dict[str, dict[str, str]],
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

    contracts = {target: control_contract(target) for target in ("arm", "gripper")}
    for target, document in contracts.items():
        sources[f"contract:{target}"] = generated_catalog_document(
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
        sources[f"topology:{target}"] = generated_catalog_document(
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

    control_capabilities = tuple(
        sorted(
            capability_id
            for capability_id in set(ARM_CAPABILITIES + GRIPPER_CAPABILITIES)
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
    manifests = {
        "control": provider_manifest(
            "manyforge.provider.native_control",
            control_capabilities,
            ["command", "state"],
        ),
        "planning": provider_manifest(
            "manyforge.provider.native_planning",
            planner_capabilities,
            ["planner_scene_projection"],
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
        )

    policy = stop_policy()
    sources["stop"] = generated_catalog_document(
        root=root,
        relative_path=(
            "manyforge_deployment/resources/maintained/catalog/"
            f"stop_evidence_policy/{policy['stop_evidence_policy_id']}.json"
        ),
        kind="stop_evidence_policy",
        record_id=policy["stop_evidence_policy_id"],
        record_revision=policy["stop_evidence_policy_revision"],
        schema_id="manyforge.stop_evidence_policy.v1",
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
    )

    component_references: dict[str, dict[str, str]] = {}
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

    command_path = root / "manyforge_deployment/resources/catalog/gripper.command_position.v1.yaml"
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
    return sources, component_references


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
    refs: dict[str, dict[str, str]],
) -> tuple[dict[str, Any], list[dict[str, Any]], list[dict[str, Any]]]:
    occurrence = deployment["component_occurrence_id"]
    family_id = family["model_family_id"]
    component_ref = refs[f"component:{family_id}"]
    model_ref = refs[f"model:{family_id}"]
    runtime_ref = refs["runtime"]
    stop_ref = refs["stop"]
    control_ref = refs["provider:control"]
    planning_ref = refs["provider:planning"]
    runtime_unit_id = f"{occurrence}.native_execution"
    model_instance_id = f"{occurrence}.model"

    arm_resources = [
        {
            "resource_role_id": "arm_joints",
            "resource_id": f"{occurrence}.arm.{joint}",
        }
        for joint in source_joint_names(family, "arm_joints")
    ]
    finger_joints = source_joint_names(family, "fingers")
    gripper_resources = [
        {
            "resource_role_id": "fingers",
            "resource_id": f"{occurrence}.gripper.{joint}",
        }
        for joint in finger_joints
    ] or [
        {
            "resource_role_id": "fingers",
            "resource_id": f"{occurrence}.gripper.logical_position",
        }
    ]

    bindings: list[dict[str, Any]] = []
    for target, capabilities, resources in (
        ("arm", ARM_CAPABILITIES, arm_resources),
        ("gripper", GRIPPER_CAPABILITIES, gripper_resources),
    ):
        contract = refs[f"contract:{target}"]
        topology = refs[f"topology:{target}"]
        for capability_id in capabilities:
            if CAPABILITIES[capability_id][2] != "control_binding":
                continue
            bindings.append(
                {
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
                    "provider_instance_id": f"{occurrence}.native_control",
                    "provider_manifest_id": control_ref["record_id"],
                    "provider_manifest_revision": control_ref["record_revision"],
                    "state_authority_id": f"{occurrence}.{target}.predicted_state",
                    "state_authority_class": "predicted",
                    "stop_evidence_policy_id": stop_ref["record_id"],
                    "stop_evidence_policy_revision": stop_ref["record_revision"],
                    "containment_granularity": "resource",
                }
            )
    bindings.sort(key=lambda row: row["control_binding_id"])

    graph = {
        "runtime_units": [
            {
                "runtime_unit_id": runtime_unit_id,
                "runtime_profile_id": runtime_ref["record_id"],
                "runtime_profile_revision": runtime_ref["record_revision"],
                "dependency_runtime_unit_ids": [],
                "configuration": {},
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
                "root_pose_in_cell": {
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
            }
        ],
        "control_domains": [
            {
                "control_domain_id": domain_id(occurrence, target),
                "control_domain_revision": ZERO_DIGEST,
                "runtime_unit_ids": [runtime_unit_id],
                "control_coordinator_endpoint_role": "native_execution_status",
                "controller_topology_id": refs[f"topology:{target}"]["record_id"],
                "topology_revision": refs[f"topology:{target}"]["record_revision"],
                "containment_granularity": "resource",
            }
            for target in ("arm", "gripper")
        ],
        "control_bindings": bindings,
        "component_attachments": [],
    }

    desired: list[dict[str, Any]] = []
    for target in ("arm", "gripper"):
        topology = refs[f"topology:{target}"]
        selected_bindings = [
            binding
            for binding in bindings
            if binding["command_target_id"] == target
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
        "arm": control_binding_id(
            occurrence, "arm", "execution.trajectory.execute"
        ),
        "gripper": control_binding_id(
            occurrence, "gripper", "tool.gripper.command"
        ),
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
                    "provider_instance_id": f"{occurrence}.native_planning",
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
    provider_bindings.sort(key=lambda row: row["capability_provider_binding_id"])
    return graph, desired, provider_bindings


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
                "external_resource_alias": provider[
                    "bridge_external_resource_alias"
                ],
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
    profiles: dict[str, Any],
    refs: dict[str, dict[str, str]],
    overlay_ref: dict[str, str],
) -> dict[str, Any]:
    graph, desired, provider_bindings = graph_and_applicability(
        deployment, family, refs
    )
    family_id = family["model_family_id"]
    catalog_keys = {
        *(f"capability:{capability_id}" for capability_id in ARM_CAPABILITIES),
        *(f"capability:{capability_id}" for capability_id in GRIPPER_CAPABILITIES),
        "command_schema",
        f"component:{family_id}",
        "contract:arm",
        "contract:gripper",
        f"model:{family_id}",
        "provider:control",
        "provider:planning",
        "runtime",
        "stop",
        "topology:arm",
        "topology:gripper",
    }
    profile_id = deployment["assistant_profile_id"]
    if profile_id is not None:
        catalog_keys.update(
            {
                "capability:assistant.bridge.request",
                "capability:assistant.model.invoke",
                "provider:assistant_bridge",
                "provider:model_endpoint",
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
            route["control_binding_revision"] = bindings[
                route["control_binding_id"]
            ]["control_binding_revision"]
    for provider in value["capability_provider_bindings"]:
        target = provider["target"]
        target["control_binding_revision"] = bindings[target["control_binding_id"]][
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


def role_assignments(
    root: Path,
    description: dict[str, Any],
    sealed: dict[str, dict[str, Any]],
) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    for deployment in description["deployments"]:
        deployment_source = deployment["deployment_source"]
        deployment_value = sealed[deployment_source]
        for assignment in deployment["program_role_assignments"]:
            program = read_yaml(root / assignment["program_source"], "maintained Program")
            rows.append(
                {
                    "deployment_artifact_id": deployment["deployment_artifact_id"],
                    "deployment_revision": deployment_value["deployment_revision"],
                    "deployment_source": deployment_source,
                    "program_id": program["program_id"],
                    "program_revision": program["program_revision"],
                    "program_source": assignment["program_source"],
                    "component_roles": copy.deepcopy(assignment["component_roles"]),
                    "target_roles": copy.deepcopy(assignment["target_roles"]),
                }
            )
    rows.sort(key=lambda row: (row["deployment_source"], row["program_source"]))
    return {"api_version": ROLE_ASSIGNMENTS_API, "assignments": rows}


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

    source_rows, component_refs = catalog_sources(root, description)
    del component_refs
    refs = {key: value[2] for key, value in source_rows.items()}
    outputs: list[tuple[Path, bytes, str]] = []
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
    sealed_values: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="manyforge-n6-deployments-") as temporary:
        temporary_root = Path(temporary)
        for deployment in description["deployments"]:
            candidate = deployment_candidate(
                deployment,
                family_by_id[deployment["model_family_id"]],
                profiles_by_id,
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

    assignment_value = role_assignments(root, description, sealed_values)
    assignment_path = (
        root
        / "manyforge_deployment/resources/maintained/maintained_program_role_assignments.v1.json"
    )
    outputs.append(
        (
            assignment_path,
            pretty_bytes(assignment_value),
            "maintained Program role assignments",
        )
    )
    for path, payload, label in outputs:
        require_or_update(path, payload, args.update, label)


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
        print(f"generate_maintained_deployment_occurrences: {error}", file=os.sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
