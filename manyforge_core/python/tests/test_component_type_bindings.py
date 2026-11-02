"""Parity tests for the immutable C0.2i ComponentType boundary."""

from __future__ import annotations

import hashlib
import json
import typing

import pytest

core = pytest.importorskip("manyforge_core_py")

CAPABILITY_SCHEMA_REVISION = (
    "sha256:2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0"
)
COMPONENT_TYPE_SCHEMA_REVISION = (
    "sha256:d971a7022a7977625256649896f8253020686a7b543c30d557162ebee8f69dde"
)


def _canonical_json(value: object) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _content_revision(domain: str, api_version: str, identity: object) -> str:
    preimage = (
        b"manyforge.content-identity.v1"
        + b"\x00"
        + domain.encode()
        + b"\x00"
        + api_version.encode()
        + b"\x00"
        + _canonical_json(identity).encode()
    )
    return f"sha256:{hashlib.sha256(preimage).hexdigest()}"


def _qualified_capability(capability_id: str):
    descriptor = core.find_builtin_capability_definition(capability_id)
    assert descriptor is not None
    document = _canonical_json(
        {
            "api_version": "manyforge.capability_definition.v1",
            "capability_id": descriptor.capability_id,
            "capability_definition_revision": (
                descriptor.capability_definition_revision
            ),
            "scope": core.capability_scope_to_string(descriptor.scope),
            "provider_binding_kind": (
                core.capability_provider_binding_kind_to_string(
                    descriptor.provider_binding_kind
                )
            ),
        }
    )
    store_key = core.ContentAddressedStoreKey(f"sha256/capability/{capability_id}")
    reference = core.CapabilityDefinitionCatalogRecordRefDTO(
        record_id=core.CapabilityId(capability_id),
        record_revision=core.CapabilityDefinitionRevision(
            descriptor.capability_definition_revision
        ),
        schema_id=core.SchemaId("manyforge.capability_definition.v1"),
        schema_revision=core.SchemaRevision(CAPABILITY_SCHEMA_REVISION),
        content_digest=core.ContentDigest(
            f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
        ),
        content_addressed_store_key=store_key,
    )
    return core.resolve_and_qualify_capability_definition(
        reference, store_key, document
    )


def _component_type_document(capabilities) -> str:
    identity = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": "vacuum_io",
        "tags": [],
        "model_roles": [],
        "model_mounts": [],
        "command_targets": [
            {
                "command_target_id": "vacuum",
                "model_group_roles": [],
                "resource_roles": [
                    {
                        "resource_role_id": "vacuum",
                        "source_kind": "component_local",
                    }
                ],
                "frame_roles": [],
                "collision_body_roles": [],
                "capabilities": [
                    {
                        "capability_id": capability.value.capability_id.value,
                        "capability_definition_revision": (
                            capability.value.capability_definition_revision.value
                        ),
                    }
                    for capability in capabilities
                ],
            }
        ],
        "attachment_interfaces": [],
    }
    document = dict(identity)
    document["component_type_revision"] = _content_revision(
        "catalog-record-revision/component_type",
        "manyforge.component_type.v1",
        identity,
    )
    return _canonical_json(document)


def _component_type_reference(document: str):
    decoded = json.loads(document)
    store_key = core.ContentAddressedStoreKey("sha256/component-type/vacuum-io")
    return (
        core.ComponentTypeCatalogRecordRefDTO(
            record_id=core.ComponentTypeId(decoded["component_type_id"]),
            record_revision=core.ComponentTypeRevision(
                decoded["component_type_revision"]
            ),
            schema_id=core.SchemaId("manyforge.component_type.v1"),
            schema_revision=core.SchemaRevision(COMPONENT_TYPE_SCHEMA_REVISION),
            content_digest=core.ContentDigest(
                f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
            ),
            content_addressed_store_key=store_key,
        ),
        store_key,
    )


def test_component_type_dto_surface_is_typed_read_only_and_uses_closed_union():
    endpoint_frame = core.ComponentTypeFrameRoleDTO(
        frame_role_id=core.FrameRoleId("tool.endpoint.grasp"),
        model_role_id=core.ModelRoleId("tool"),
        asset_frame_role_id=core.FrameRoleId("gripper_tcp"),
        contact_body_role_ids=[core.CollisionBodyRoleId("fingers")],
    )
    assert endpoint_frame.contact_body_role_ids == [
        core.CollisionBodyRoleId("fingers")
    ]
    legacy_frame = core.ComponentTypeFrameRoleDTO(
        frame_role_id=core.FrameRoleId("tcp"),
        model_role_id=core.ModelRoleId("arm"),
        asset_frame_role_id=core.FrameRoleId("tcp"),
    )
    assert legacy_frame.contact_body_role_ids == []

    pose = core.PlacementPoseDTO(
        position_m=[0.0, 0.0, 0.1],
        orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    parent = core.ComponentTypeModelFrameRefDTO(
        model_role_id=core.ModelRoleId("arm"),
        frame_role_id=core.FrameRoleId("tool"),
    )
    child = core.ComponentTypeModelFrameRefDTO(
        model_role_id=core.ModelRoleId("gripper"),
        frame_role_id=core.FrameRoleId("base"),
    )
    model_resource = core.ComponentTypeModelResourceRoleDTO(
        resource_role_id=core.ResourceRoleId("arm_joints"),
        model_role_id=core.ModelRoleId("arm"),
        asset_resource_role_id=core.ResourceRoleId("joints"),
    )
    local_resource = core.ComponentTypeLocalResourceRoleDTO(
        resource_role_id=core.ResourceRoleId("vacuum"),
        coordinate_kind=core.CoordinateKind.kLinear,
    )
    assert local_resource.coordinate_kind == core.CoordinateKind.kLinear

    assert set(typing.get_args(core.ComponentTypeResourceRoleDTO)) == {
        core.ComponentTypeModelResourceRoleDTO,
        core.ComponentTypeLocalResourceRoleDTO,
    }
    assert (
        core.component_type_resource_source_kind(model_resource)
        == core.ComponentTypeResourceSourceKind.kModelRole
    )
    assert (
        core.component_type_resource_source_kind(local_resource)
        == core.ComponentTypeResourceSourceKind.kComponentLocal
    )
    # The returned strong ID must own its value after the temporary variant
    # created by pybind's union conversion has been destroyed.
    returned_role_id = core.component_type_resource_role_id(model_resource)
    assert returned_role_id == core.ResourceRoleId("arm_joints")
    assert returned_role_id.value == "arm_joints"

    target = core.ComponentTypeCommandTargetDTO(
        command_target_id=core.CommandTargetId("arm"),
        model_group_roles=[
            core.ComponentTypeModelGroupRoleDTO(
                model_group_role_id=core.ModelGroupRoleId("motion"),
                model_role_id=core.ModelRoleId("arm"),
                asset_model_group_role_id=core.ModelGroupRoleId("manipulator"),
            )
        ],
        resource_roles=[model_resource, local_resource],
        frame_roles=[],
        collision_body_roles=[],
        capabilities=[
            core.ComponentTypeCapabilityDTO(
                capability_id=core.CapabilityId("planning.motion.pose_goal"),
                capability_definition_revision=core.CapabilityDefinitionRevision(
                    "sha256:" + "1" * 64
                ),
            )
        ],
    )
    attachment_interface = core.ComponentTypeAttachmentInterfaceDTO(
        attachment_interface_id=core.AttachmentInterfaceId("tool_mount"),
        model_role_id=core.ModelRoleId("gripper"),
        frame_role_id=core.FrameRoleId("tool"),
        accepts_tags=["tool"],
    )
    component_type = core.ComponentTypeDTO(
        component_type_id=core.ComponentTypeId("arm_with_gripper"),
        component_type_revision=core.ComponentTypeRevision("sha256:" + "2" * 64),
        tags=["robot"],
        model_roles=[
            core.ComponentTypeModelRoleDTO(
                model_role_id=core.ModelRoleId("arm"),
                cardinality=(core.ComponentTypeModelRoleCardinality.kExactlyOne),
            ),
            core.ComponentTypeModelRoleDTO(
                model_role_id=core.ModelRoleId("gripper"),
                cardinality=(core.ComponentTypeModelRoleCardinality.kExactlyOne),
            ),
        ],
        component_root=parent,
        model_mounts=[
            core.ComponentTypeModelMountDTO(
                parent=parent,
                child=child,
                child_frame_pose_in_parent_frame=pose,
            )
        ],
        command_targets=[target],
        attachment_interfaces=[attachment_interface],
    )

    assert component_type.model_mounts[0].child == child
    assert list(component_type.command_targets[0].resource_roles) == [
        model_resource,
        local_resource,
    ]
    assert component_type.attachment_interfaces[0].max_attachments == 1
    with pytest.raises(AttributeError):
        component_type.tags = []


def test_component_type_constants_and_string_surfaces_match_v1():
    assert core.COMPONENT_TYPE_API_VERSION == "manyforge.component_type.v1"
    assert core.MAX_COMPONENT_TYPE_TAGS == 256
    assert core.MAX_COMPONENT_TYPE_MODEL_ROLES == 256
    assert core.MAX_COMPONENT_TYPE_MODEL_MOUNTS == 255
    assert core.MAX_COMPONENT_TYPE_COMMAND_TARGETS == 256
    assert core.MAX_COMPONENT_TYPE_ATTACHMENT_INTERFACES == 256
    assert core.MAX_COMPONENT_TYPE_TARGET_ROLE_ALIASES == 4096
    assert core.MAX_COMPONENT_TYPE_TARGET_CAPABILITIES == 256
    assert core.MAX_COMPONENT_TYPE_CAPABILITY_DEPENDENCIES == 256
    assert core.MAX_COMPONENT_TYPE_ATTACHMENTS_PER_INTERFACE == 4096
    assert (
        core.component_type_model_role_cardinality_to_string(
            core.ComponentTypeModelRoleCardinality.kExactlyOne
        )
        == "exactly_one"
    )
    assert (
        core.component_type_resource_source_kind_to_string(
            core.ComponentTypeResourceSourceKind.kComponentLocal
        )
        == "component_local"
    )


def test_component_type_is_exactly_qualified_from_qualified_capabilities():
    capabilities = [
        _qualified_capability("planning.motion.pose_goal"),
        _qualified_capability("state.observed.current"),
    ]
    document = _component_type_document(capabilities)
    reference, store_key = _component_type_reference(document)

    qualified = core.resolve_and_qualify_component_type(
        reference, store_key, document, capabilities
    )

    assert isinstance(qualified, core.QualifiedComponentType)
    assert qualified.value.component_type_id == core.ComponentTypeId("vacuum_io")
    assert qualified.value.component_type_revision == (reference.record_revision)
    assert qualified.value.component_root is None
    local_resource = qualified.value.command_targets[0].resource_roles[0]
    assert isinstance(local_resource, core.ComponentTypeLocalResourceRoleDTO)
    assert core.component_type_resource_role_id(local_resource).value == "vacuum"
    assert json.loads(qualified.normalized_canonical_json) == json.loads(document)

    with pytest.raises(TypeError):
        core.QualifiedComponentType()
    with pytest.raises(AttributeError):
        qualified.value = qualified.value
    with pytest.raises(TypeError):
        core.resolve_and_qualify_component_type(
            reference,
            store_key,
            document,
            [capability.value for capability in capabilities],
        )


def test_component_type_qualification_rejects_unresolved_capability_closure():
    capability = _qualified_capability("state.observed.current")
    document = _component_type_document([capability])
    reference, store_key = _component_type_reference(document)

    with pytest.raises(
        core.ComponentTypeCodecError,
        match="missing qualified capability definition",
    ):
        core.resolve_and_qualify_component_type(reference, store_key, document, [])

    with pytest.raises(ValueError, match="exceeds.*dependency bound"):
        core.resolve_and_qualify_component_type(
            reference,
            store_key,
            document,
            [capability] * 257,
        )
