"""C0.2 parity tests for immutable composable-cell deployment boundaries."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import typing

import pytest

core = pytest.importorskip("manyforge_core_py")

REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
CLOSED_SCHEMA_RESOURCES = (
    REPOSITORY_ROOT / "manyforge_deployment" / "resources" / "closed_schema"
)

STRONG_STRING_TYPES = (
    "CellId",
    "GraphRevision",
    "SchemaId",
    "SchemaRevision",
    "ContentDigest",
    "ContentAddressedStoreKey",
    "RuntimeProfileId",
    "RuntimeProfileRevision",
    "RuntimeUnitId",
    "RuntimePolicyId",
    "RuntimePolicyRevision",
    "ComponentTypeId",
    "ComponentTypeRevision",
    "ControllableComponentId",
    "ModelAssetId",
    "ModelInstanceId",
    "ModelRevision",
    "ModelRoleId",
    "ModelGroupRoleId",
    "CommandTargetId",
    "CapabilityId",
    "CapabilityDefinitionRevision",
    "FrameRoleId",
    "CollisionBodyRoleId",
    "ResourceId",
    "ResourceRoleId",
    "AttachmentInterfaceId",
    "ControlInterfaceContractId",
    "ControlInterfaceRevision",
    "ControllerTopologyId",
    "TopologyRevision",
    "ControlRouteId",
    "ControlDomainId",
    "ControlDomainRevision",
    "ControlBindingId",
    "ControlBindingRevision",
    "EndpointRoleId",
    "StateAuthorityId",
    "StopEvidencePolicyId",
    "StopEvidencePolicyRevision",
    "StopBudgetPhaseId",
    "StopEvidenceItemId",
    "ProviderContractRoleId",
    "RegisteredStopCommandKindId",
    "RegisteredStopEvidenceKindId",
    "ContactPolicyId",
    "ContactPolicyRevision",
    "ProviderInstanceId",
    "ProviderManifestRevision",
    "ComponentAttachmentId",
    "ComponentAttachmentRevision",
    "CommandSchemaId",
    "CommandSchemaRevision",
)


CATALOG_REF_CASES = (
    (
        "ModelAssetCatalogRecordRefDTO",
        "ModelAssetId",
        "ModelRevision",
        "kModelAsset",
        "manyforge.model_asset.v1",
    ),
    (
        "RuntimeProfileCatalogRecordRefDTO",
        "RuntimeProfileId",
        "RuntimeProfileRevision",
        "kRuntimeProfile",
        "manyforge.runtime_profile.v1",
    ),
    (
        "ComponentTypeCatalogRecordRefDTO",
        "ComponentTypeId",
        "ComponentTypeRevision",
        "kComponentType",
        "manyforge.component_type.v1",
    ),
    (
        "ControlInterfaceContractCatalogRecordRefDTO",
        "ControlInterfaceContractId",
        "ControlInterfaceRevision",
        "kControlInterfaceContract",
        "manyforge.control_interface_contract.v1",
    ),
    (
        "ControllerTopologyCatalogRecordRefDTO",
        "ControllerTopologyId",
        "TopologyRevision",
        "kControllerTopology",
        "manyforge.controller_topology.v1",
    ),
    (
        "ProviderManifestCatalogRecordRefDTO",
        "ProviderInstanceId",
        "ProviderManifestRevision",
        "kProviderManifest",
        "manyforge.provider_manifest.v1",
    ),
    (
        "CommandSchemaCatalogRecordRefDTO",
        "CommandSchemaId",
        "CommandSchemaRevision",
        "kCommandSchema",
        "manyforge.command_schema.v1",
    ),
    (
        "CapabilityDefinitionCatalogRecordRefDTO",
        "CapabilityId",
        "CapabilityDefinitionRevision",
        "kCapabilityDefinition",
        "manyforge.capability_definition.v1",
    ),
    (
        "ContactPolicyCatalogRecordRefDTO",
        "ContactPolicyId",
        "ContactPolicyRevision",
        "kContactPolicy",
        "manyforge.contact_policy.v1",
    ),
    (
        "StopEvidencePolicyCatalogRecordRefDTO",
        "StopEvidencePolicyId",
        "StopEvidencePolicyRevision",
        "kStopEvidencePolicy",
        "manyforge.stop_evidence_policy.v1",
    ),
    (
        "RuntimePolicyCatalogRecordRefDTO",
        "RuntimePolicyId",
        "RuntimePolicyRevision",
        "kRuntimePolicy",
        "manyforge.runtime_policy.v1",
    ),
)

ZERO_DIGEST = "sha256:" + ("0" * 64)
EMPTY_GRAPH_YAML = f"""\
api_version: manyforge.cell_graph.v1
graph_revision: {ZERO_DIGEST}
cell_id: cell_a
runtime_units: []
model_instances: []
controllable_components: []
control_domains: []
control_bindings: []
component_attachments: []
"""


def _id(type_name: str, value: str):
    return getattr(core, type_name)(value)


def _catalog_ref(case, *, schema_family: str | None = None):
    ref_name, id_name, revision_name, _, expected_family = case
    return getattr(core, ref_name)(
        record_id=_id(id_name, "record"),
        record_revision=_id(revision_name, "revision"),
        schema_id=core.SchemaId(schema_family or expected_family),
        schema_revision=core.SchemaRevision("schema-revision"),
        content_digest=core.ContentDigest("sha256:content"),
        content_addressed_store_key=core.ContentAddressedStoreKey("cas:key"),
    )


def _control_interface_contract_document(
    *,
    contract_id: str = "arm_position_trajectory",
    command_interfaces=None,
    state_interfaces=None,
    ordering=None,
):
    command_interfaces = (
        [{"resource_role_id": "arm", "interface_name": "position"}]
        if command_interfaces is None
        else command_interfaces
    )
    state_interfaces = (
        [
            {"resource_role_id": "arm", "interface_name": "position"},
            {"resource_role_id": "arm", "interface_name": "velocity"},
        ]
        if state_interfaces is None
        else state_interfaces
    )
    ordering = ["arm"] if ordering is None else ordering
    payload = {
        "api_version": "manyforge.control_interface_contract.v1",
        "control_interface_contract_id": contract_id,
        "command_interfaces": command_interfaces,
        "state_interfaces": state_interfaces,
        "ordering": ordering,
    }
    canonical_payload = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/control_interface_contract"
        b"\x00manyforge.control_interface_contract.v1\x00" + canonical_payload.encode()
    )
    payload["control_interface_revision"] = (
        "sha256:" + hashlib.sha256(preimage).hexdigest()
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _control_interface_contract_reference(document: str, contract_id: str):
    parsed = json.loads(document)
    return core.ControlInterfaceContractCatalogRecordRefDTO(
        record_id=core.ControlInterfaceContractId(contract_id),
        record_revision=core.ControlInterfaceRevision(
            parsed["control_interface_revision"]
        ),
        schema_id=core.SchemaId("manyforge.control_interface_contract.v1"),
        schema_revision=core.SchemaRevision(
            "sha256:4abdf2c402d8e6e818bc26df59ce1c9cdc911d1e807144bdf525d2f69d0a449d"
        ),
        content_digest=core.ContentDigest(
            "sha256:" + hashlib.sha256(document.encode()).hexdigest()
        ),
        content_addressed_store_key=core.ContentAddressedStoreKey(
            "sha256/control-interface-python"
        ),
    )


def _stop_evidence_policy_document(
    *,
    policy_id: str = "measured_rest",
    evidence_kind: str = "controller_goal_terminal",
    source_kind: str = "controller_goal_status",
    observation_kind: str = "post_command_sample",
):
    evidence = {"evidence_kind": evidence_kind}
    if evidence_kind in {
        "motion_rest",
        "io_safe_output",
        "logical_cancellation_no_continuing_effect",
        "registered",
    }:
        evidence["provider_contract_ref"] = {
            "provider_contract_role_id": "stop_contract",
            "contract_schema_id": "manyforge.stop_provider_contract.v1",
            "contract_schema_revision": ZERO_DIGEST,
        }
    if evidence_kind == "registered":
        evidence["registered_evidence_kind_id"] = "vendor_terminal"

    source = {
        "source_kind": source_kind,
        "freshness_budget_ns": 25,
    }
    if source_kind == "binding_state_authority":
        source["state_endpoint_role_id"] = "measured_state"
        source["allowed_authority_classes"] = ["measured"]
    elif source_kind == "provider_evidence":
        source["evidence_endpoint_role_id"] = "stop_evidence"

    observation = {"observation_kind": observation_kind}
    if observation_kind == "post_command_dwell":
        observation["minimum_dwell_ns"] = 200

    payload = {
        "api_version": "manyforge.stop_evidence_policy.v1",
        "stop_evidence_policy_id": policy_id,
        "disposition": "stoppable",
        "containment_granularity": "control_domain",
        "containment_command": {
            "command_phase_id": "command",
            "command": {"command_kind": "controller_goal_cancel"},
        },
        "deadline_budget": {
            "outer_duration_ns": 1000,
            "kernel_completion_reserve_ns": 100,
            "phases": [
                {"phase_id": "command", "deadline_offset_ns": 100},
                {"phase_id": "terminal", "deadline_offset_ns": 500},
            ],
        },
        "evidence_conjunction": "all_of",
        "evidence_requirements": [
            {
                "evidence_item_id": "terminal",
                "phase_id": "terminal",
                "evidence": evidence,
                "source_requirement": source,
                "observation_semantics": observation,
            }
        ],
    }
    canonical_payload = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/stop_evidence_policy"
        b"\x00manyforge.stop_evidence_policy.v1\x00"
        + canonical_payload.encode()
    )
    payload["stop_evidence_policy_revision"] = (
        "sha256:" + hashlib.sha256(preimage).hexdigest()
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _stop_evidence_policy_reference(document: str, policy_id: str):
    parsed = json.loads(document)
    return core.StopEvidencePolicyCatalogRecordRefDTO(
        record_id=core.StopEvidencePolicyId(policy_id),
        record_revision=core.StopEvidencePolicyRevision(
            parsed["stop_evidence_policy_revision"]
        ),
        schema_id=core.SchemaId("manyforge.stop_evidence_policy.v1"),
        schema_revision=core.SchemaRevision(
            "sha256:e516114453cac5912b204bf17267b66f08a2afa47a72a8dd0c0f0f6b808478f2"
        ),
        content_digest=core.ContentDigest(
            "sha256:" + hashlib.sha256(document.encode()).hexdigest()
        ),
        content_addressed_store_key=core.ContentAddressedStoreKey(
            "sha256/stop-policy-python"
        ),
    )


def _cell_graph():
    configuration = {
        "driver": core.JsonValueDTO("fake"),
        "rate_hz": core.JsonValueDTO(100.0),
        "enabled": core.JsonValueDTO(True),
        "nested": core.JsonValueDTO({"mode": "position"}),
    }
    runtime_unit = core.RuntimeUnitDTO(
        runtime_unit_id=core.RuntimeUnitId("runtime/arm"),
        runtime_profile_id=core.RuntimeProfileId("runtime-profile/fake"),
        runtime_profile_revision=core.RuntimeProfileRevision(
            "sha256:runtime-profile"
        ),
        dependency_runtime_unit_ids=[],
        configuration=configuration,
    )
    placement_pose = core.PlacementPoseDTO(
        position_m=[0.0, 0.0, 0.0],
        orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    model_instance = core.ModelInstanceDTO(
        model_instance_id=core.ModelInstanceId("model/arm"),
        model_asset_id=core.ModelAssetId("asset/lite6"),
        model_revision=core.ModelRevision("sha256:model"),
    )
    component = core.ControllableComponentDTO(
        controllable_component_id=core.ControllableComponentId("component/arm"),
        component_type_id=core.ComponentTypeId("component-type/lite6"),
        component_type_revision=core.ComponentTypeRevision("sha256:component-type"),
        model_role_bindings=[
            core.ModelRoleBindingDTO(
                model_role_id=core.ModelRoleId("primary"),
                model_instance_id=model_instance.model_instance_id,
            )
        ],
        runtime_unit_ids=[runtime_unit.runtime_unit_id],
        root_pose_in_cell=placement_pose,
    )
    domain = core.ControlDomainDTO(
        control_domain_id=core.ControlDomainId("domain/arm"),
        control_domain_revision=core.ControlDomainRevision("sha256:domain"),
        runtime_unit_ids=[runtime_unit.runtime_unit_id],
        control_coordinator_endpoint_role=core.EndpointRoleId("controller_manager"),
        controller_topology_id=core.ControllerTopologyId("topology/arm"),
        topology_revision=core.TopologyRevision("sha256:topology"),
        containment_granularity=core.ContainmentGranularity.kControlDomain,
    )
    route_projection = core.RouteEndpointProjectionDTO(
        control_route_id=core.ControlRouteId("route/jtc"),
        command_endpoint_role_ids=[core.EndpointRoleId("follow_joint_trajectory")],
        state_endpoint_role_ids=[core.EndpointRoleId("joint_states")],
    )
    binding = core.ControlBindingDTO(
        control_binding_id=core.ControlBindingId("binding/arm/trajectory"),
        control_binding_revision=core.ControlBindingRevision("sha256:binding"),
        controllable_component_id=component.controllable_component_id,
        command_target_id=core.CommandTargetId("target/arm"),
        capability_id=core.CapabilityId("execution.trajectory.execute"),
        capability_definition_revision=core.CapabilityDefinitionRevision(
            "sha256:capability"
        ),
        control_domain_id=domain.control_domain_id,
        resource_bindings=[
            core.ResourceBindingDTO(
                resource_role_id=core.ResourceRoleId("joint/1"),
                resource_id=core.ResourceId("arm_joint1"),
            )
        ],
        control_interface_contract_id=core.ControlInterfaceContractId(
            "interface/position"
        ),
        control_interface_revision=core.ControlInterfaceRevision(
            "sha256:interface"
        ),
        controller_topology_id=domain.controller_topology_id,
        topology_revision=domain.topology_revision,
        legal_control_route_ids=[route_projection.control_route_id],
        route_endpoint_projections=[route_projection],
        provider_instance_id=core.ProviderInstanceId("provider/fake"),
        provider_manifest_revision=core.ProviderManifestRevision(
            "sha256:provider-manifest"
        ),
        state_authority_id=core.StateAuthorityId("state/measured"),
        state_authority_class=core.StateAuthorityClass.kMeasured,
        stop_evidence_policy_id=core.StopEvidencePolicyId("stop/measured-rest"),
        stop_evidence_policy_revision=core.StopEvidencePolicyRevision(
            "sha256:stop-policy"
        ),
        containment_granularity=core.ContainmentGranularity.kControlDomain,
    )
    parent = core.ComponentAttachmentEndpointDTO(
        controllable_component_id=component.controllable_component_id,
        attachment_interface_id=core.AttachmentInterfaceId("flange"),
    )
    child = core.ComponentAttachmentEndpointDTO(
        controllable_component_id=core.ControllableComponentId("component/tool"),
        attachment_interface_id=core.AttachmentInterfaceId("mount"),
    )
    attachment = core.ComponentAttachmentEdgeDTO(
        component_attachment_id=core.ComponentAttachmentId("attachment/tool"),
        component_attachment_revision=core.ComponentAttachmentRevision(
            "sha256:attachment"
        ),
        parent=parent,
        child=child,
        child_interface_pose_in_parent_interface=placement_pose,
    )
    graph = core.CellGraphDTO(
        api_version=core.CELL_GRAPH_API_VERSION,
        graph_revision=core.GraphRevision("sha256:graph"),
        cell_id=core.CellId("cell/test"),
        runtime_units=[runtime_unit],
        model_instances=[model_instance],
        controllable_components=[component],
        control_domains=[domain],
        control_bindings=[binding],
        component_attachments=[attachment],
    )
    return {
        "runtime_unit": runtime_unit,
        "model_instance": model_instance,
        "component": component,
        "domain": domain,
        "binding": binding,
        "attachment": attachment,
        "graph": graph,
    }


def test_every_core_strong_string_has_distinct_immutable_python_type():
    values = []
    for type_name in STRONG_STRING_TYPES:
        cls = getattr(core, type_name)
        value = cls("shared")
        values.append(value)
        assert value.value == "shared"
        assert str(value) == "shared"
        assert type_name in repr(value)
        assert value == cls("shared")
        assert hash(value) == hash(cls("shared"))
        with pytest.raises(AttributeError):
            value.value = "changed"
        with pytest.raises(AttributeError):
            value.extra = "changed"
        with pytest.raises(TypeError):
            cls()

    assert len({type(value) for value in values}) == len(STRONG_STRING_TYPES)
    assert core.CellId("shared") != core.GraphRevision("shared")
    assert len({core.CellId("shared"), core.GraphRevision("shared")}) == 2

    with pytest.raises(TypeError):

        class InvalidSubclass(core.CellId):
            pass


@pytest.mark.parametrize("case", CATALOG_REF_CASES)
def test_catalog_record_ref_union_is_exact_typed_and_read_only(case):
    ref = _catalog_ref(case)
    ref_name, _, _, kind_name, schema_family = case
    kind = getattr(core.CatalogRecordKind, kind_name)

    assert type(ref).__name__ == ref_name
    assert ref.record_kind == kind
    assert core.catalog_record_kind(ref) == kind
    assert core.catalog_record_id_value(ref) == "record"
    assert core.catalog_record_revision_value(ref) == "revision"
    assert core.catalog_record_ref_matches_registry_family(ref)
    descriptor = core.catalog_record_type_descriptor(ref)
    assert descriptor.record_kind == kind
    assert descriptor.schema_family == schema_family
    assert descriptor == core.catalog_record_type_descriptor(kind)
    with pytest.raises(AttributeError):
        ref.record_id = ref.record_id
    with pytest.raises(AttributeError):
        descriptor.schema_family = "other"


def test_catalog_record_ref_runtime_union_and_registry_are_closed():
    union_members = typing.get_args(core.CatalogRecordRefDTO)
    assert union_members == tuple(core.CatalogRecordRefTypes)
    assert len(union_members) == len(CATALOG_REF_CASES) == 11
    assert len(core.CATALOG_RECORD_TYPE_REGISTRY) == 11
    assert len(
        {
            descriptor.record_kind
            for descriptor in core.CATALOG_RECORD_TYPE_REGISTRY
        }
    ) == 11

    invalid_family = _catalog_ref(
        CATALOG_REF_CASES[0], schema_family="manyforge.runtime_profile.v1"
    )
    assert not core.catalog_record_ref_matches_registry_family(invalid_family)

    with pytest.raises(TypeError):
        core.ModelAssetCatalogRecordRefDTO(
            record_id=core.RuntimeProfileId("record"),
            record_revision=core.ModelRevision("revision"),
            schema_id=core.SchemaId("manyforge.model_asset.v1"),
            schema_revision=core.SchemaRevision("schema-revision"),
            content_digest=core.ContentDigest("sha256:content"),
            content_addressed_store_key=core.ContentAddressedStoreKey("cas:key"),
        )


def test_capability_definition_registry_and_qualified_referent_are_exact():
    assert (
        core.CAPABILITY_DEFINITION_API_VERSION
        == "manyforge.capability_definition.v1"
    )
    registry = core.BUILTIN_CAPABILITY_DEFINITION_REGISTRY
    assert isinstance(registry, tuple)
    assert len(registry) == 67
    assert len({row.capability_id for row in registry}) == 67
    assert sum(
        row.scope == core.CapabilityScope.kTargetBinding for row in registry
    ) == 33
    assert sum(
        row.scope == core.CapabilityScope.kCellGlobal for row in registry
    ) == 26
    assert sum(
        row.scope == core.CapabilityScope.kControlDomain for row in registry
    ) == 8
    assert sum(
        row.provider_binding_kind
        == core.CapabilityProviderBindingKind.kCapabilityProviderBinding
        for row in registry
    ) == 45
    assert sum(
        row.provider_binding_kind
        == core.CapabilityProviderBindingKind.kControlBinding
        for row in registry
    ) == 22
    assert all(row.requirement_parameters_schema_ref is None for row in registry)
    assert core.BUILTIN_CAPABILITY_DEFINITION_REGISTRY_DIGEST == (
        "sha256:8f61f8285d72110c040aa2bd7095545e9d3369ca6f4ba5b5e44e18afe363b9e2"
    )
    assert core.find_builtin_capability_definition("context.state.current") is None

    descriptor = core.find_builtin_capability_definition(
        "planning.motion.joint_goal"
    )
    assert descriptor is not None
    value = core.materialize_capability_definition(descriptor)
    assert isinstance(value, core.CapabilityDefinitionDTO)
    assert value.capability_id.value == descriptor.capability_id
    assert (
        value.capability_definition_revision.value
        == descriptor.capability_definition_revision
    )
    stored_bytes = json.dumps(
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
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    store_key = core.ContentAddressedStoreKey(
        "sha256/capability-definition-planning-motion-joint-goal"
    )
    reference = core.CapabilityDefinitionCatalogRecordRefDTO(
        core.CapabilityId(descriptor.capability_id),
        core.CapabilityDefinitionRevision(
            descriptor.capability_definition_revision
        ),
        core.SchemaId("manyforge.capability_definition.v1"),
        core.SchemaRevision(
            "sha256:"
            "2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0"
        ),
        core.ContentDigest(
            f"sha256:{hashlib.sha256(stored_bytes.encode()).hexdigest()}"
        ),
        store_key,
    )
    qualified = core.resolve_and_qualify_capability_definition(
        reference, store_key, stored_bytes
    )
    assert isinstance(qualified, core.QualifiedCapabilityDefinition)
    assert qualified.value == value
    assert json.loads(qualified.normalized_canonical_json) == json.loads(
        stored_bytes
    )

    with pytest.raises(
        core.CapabilityDefinitionCodecError, match="different store key"
    ):
        core.resolve_and_qualify_capability_definition(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            stored_bytes,
        )


def test_control_interface_contract_is_typed_ordered_and_exactly_qualified():
    assert (
        core.CONTROL_INTERFACE_CONTRACT_API_VERSION
        == "manyforge.control_interface_contract.v1"
    )
    assert core.MAX_CONTROL_INTERFACE_CONTRACT_ITEMS == 4096
    command = [
        core.ResourceInterfaceClaimDTO(core.ResourceRoleId("arm"), "position"),
        core.ResourceInterfaceClaimDTO(core.ResourceRoleId("arm"), "effort"),
        core.ResourceInterfaceClaimDTO(core.ResourceRoleId("tool"), "position"),
    ]
    state = [
        core.ResourceInterfaceClaimDTO(core.ResourceRoleId("arm"), "position"),
        core.ResourceInterfaceClaimDTO(core.ResourceRoleId("tool"), "position"),
    ]
    dto = core.ControlInterfaceContractDTO(
        control_interface_contract_id=core.ControlInterfaceContractId(
            "application_specific"
        ),
        control_interface_revision=core.ControlInterfaceRevision(ZERO_DIGEST),
        command_interfaces=command,
        state_interfaces=state,
        ordering=[
            core.ResourceRoleId("arm"),
            core.ResourceRoleId("tool"),
        ],
    )
    assert dto.command_interfaces == command
    assert dto.state_interfaces == state
    assert [role.value for role in dto.ordering] == ["arm", "tool"]
    with pytest.raises(AttributeError):
        dto.ordering = []
    with pytest.raises(AttributeError):
        command[0].interface_name = "changed"

    document = _control_interface_contract_document(
        contract_id="application_specific",
        command_interfaces=[
            {"resource_role_id": "arm", "interface_name": "position"},
            {"resource_role_id": "arm", "interface_name": "effort"},
            {"resource_role_id": "tool", "interface_name": "position"},
        ],
        state_interfaces=[
            {"resource_role_id": "arm", "interface_name": "position"},
            {"resource_role_id": "tool", "interface_name": "position"},
        ],
        ordering=["arm", "tool"],
    )
    reference = _control_interface_contract_reference(document, "application_specific")
    store_key = core.ContentAddressedStoreKey("sha256/control-interface-python")
    qualified = core.resolve_and_qualify_control_interface_contract(
        reference, store_key, document
    )
    assert isinstance(qualified, core.QualifiedControlInterfaceContract)
    assert qualified.value.control_interface_contract_id.value == "application_specific"
    assert [
        (claim.resource_role_id.value, claim.interface_name)
        for claim in qualified.value.command_interfaces
    ] == [
        ("arm", "position"),
        ("arm", "effort"),
        ("tool", "position"),
    ]
    assert json.loads(qualified.normalized_canonical_json) == json.loads(document)


@pytest.mark.parametrize(
    ("command_interfaces", "state_interfaces", "ordering"),
    [
        ([], [], []),
        (
            [
                {"resource_role_id": "arm", "interface_name": "position"},
                {"resource_role_id": "arm", "interface_name": "position"},
            ],
            [],
            ["arm"],
        ),
        (
            [
                {"resource_role_id": "arm", "interface_name": "position"},
                {"resource_role_id": "tool", "interface_name": "position"},
                {"resource_role_id": "arm", "interface_name": "velocity"},
            ],
            [],
            ["arm", "tool"],
        ),
        (
            [
                {"resource_role_id": "tool", "interface_name": "position"},
                {"resource_role_id": "arm", "interface_name": "position"},
            ],
            [],
            ["arm", "tool"],
        ),
        (
            [{"resource_role_id": "arm", "interface_name": "position"}],
            [],
            ["arm", "arm"],
        ),
        (
            [{"resource_role_id": "arm", "interface_name": "position"}],
            [],
            ["arm", "unused"],
        ),
    ],
)
def test_control_interface_contract_rejects_cross_field_semantic_defects(
    command_interfaces, state_interfaces, ordering
):
    document = _control_interface_contract_document(
        contract_id="semantic_negative",
        command_interfaces=command_interfaces,
        state_interfaces=state_interfaces,
        ordering=ordering,
    )
    reference = _control_interface_contract_reference(document, "semantic_negative")
    with pytest.raises(core.ControlInterfaceContractCodecError):
        core.resolve_and_qualify_control_interface_contract(
            reference,
            core.ContentAddressedStoreKey("sha256/control-interface-python"),
            document,
        )


def test_control_interface_contract_rejects_referent_identity_mismatches():
    document = _control_interface_contract_document()
    reference = _control_interface_contract_reference(
        document, "arm_position_trajectory"
    )
    store_key = core.ContentAddressedStoreKey("sha256/control-interface-python")
    with pytest.raises(
        core.ControlInterfaceContractCodecError, match="different store key"
    ):
        core.resolve_and_qualify_control_interface_contract(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            document,
        )

    wrong_id = core.ControlInterfaceContractCatalogRecordRefDTO(
        record_id=core.ControlInterfaceContractId("different"),
        record_revision=reference.record_revision,
        schema_id=reference.schema_id,
        schema_revision=reference.schema_revision,
        content_digest=reference.content_digest,
        content_addressed_store_key=reference.content_addressed_store_key,
    )
    with pytest.raises(core.ControlInterfaceContractCodecError):
        core.resolve_and_qualify_control_interface_contract(
            wrong_id, store_key, document
        )

    wrong_digest = core.ControlInterfaceContractCatalogRecordRefDTO(
        record_id=reference.record_id,
        record_revision=reference.record_revision,
        schema_id=reference.schema_id,
        schema_revision=reference.schema_revision,
        content_digest=core.ContentDigest(ZERO_DIGEST),
        content_addressed_store_key=reference.content_addressed_store_key,
    )
    with pytest.raises(core.ControlInterfaceContractCodecError):
        core.resolve_and_qualify_control_interface_contract(
            wrong_digest, store_key, document
        )


def test_stop_evidence_policy_is_typed_immutable_and_exactly_qualified():
    assert (
        core.STOP_EVIDENCE_POLICY_API_VERSION
        == "manyforge.stop_evidence_policy.v1"
    )
    assert core.MAX_STOP_EVIDENCE_POLICY_ITEMS == 64
    assert core.MAX_STOP_EVIDENCE_POLICY_DURATION_NS == (2**53) - 1

    contract = core.ProviderContractRefDTO(
        provider_contract_role_id=core.ProviderContractRoleId("stop_contract"),
        contract_schema_id=core.SchemaId(
            "manyforge.stop_provider_contract.v1"
        ),
        contract_schema_revision=core.SchemaRevision(ZERO_DIGEST),
    )
    command = core.StopContainmentCommandDTO(
        command_phase_id=core.StopBudgetPhaseId("command"),
        command_kind=core.StopContainmentCommandKind.kMotionControlledStop,
        provider_contract_ref=contract,
    )
    budget = core.StopDeadlineBudgetDTO(
        outer_duration_ns=1000,
        kernel_completion_reserve_ns=100,
        phases=[
            core.StopBudgetPhaseDTO(core.StopBudgetPhaseId("command"), 100),
            core.StopBudgetPhaseDTO(core.StopBudgetPhaseId("rest"), 500),
        ],
    )
    evidence = core.StopEvidenceRequirementDTO(
        evidence_item_id=core.StopEvidenceItemId("rest"),
        phase_id=core.StopBudgetPhaseId("rest"),
        evidence_kind=core.StopEvidenceRequirementKind.kMotionRest,
        provider_contract_ref=contract,
        source_requirement=core.StopEvidenceSourceRequirementDTO(
            source_kind=core.StopEvidenceSourceKind.kBindingStateAuthority,
            freshness_budget_ns=25,
            endpoint_role_id=core.EndpointRoleId("measured_state"),
            allowed_authority_classes=[core.StateAuthorityClass.kMeasured],
        ),
        observation_semantics=core.StopEvidenceObservationDTO(
            observation_kind=core.StopEvidenceObservationKind.kPostCommandDwell,
            minimum_dwell_ns=200,
        ),
    )
    dto = core.StopEvidencePolicyDTO(
        stop_evidence_policy_id=core.StopEvidencePolicyId("typed_stop"),
        stop_evidence_policy_revision=core.StopEvidencePolicyRevision(
            ZERO_DIGEST
        ),
        disposition=core.StopEvidenceDisposition.kStoppable,
        containment_granularity=core.ContainmentGranularity.kControlDomain,
        containment_command=command,
        deadline_budget=budget,
        evidence_conjunction=core.StopEvidenceConjunction.kAllOf,
        evidence_requirements=[evidence],
    )
    assert dto.containment_command == command
    assert dto.deadline_budget == budget
    assert dto.evidence_requirements == [evidence]
    with pytest.raises(AttributeError):
        dto.evidence_requirements = []
    with pytest.raises(AttributeError):
        evidence.evidence_kind = (
            core.StopEvidenceRequirementKind.kControllerGoalTerminal
        )

    document = _stop_evidence_policy_document()
    reference = _stop_evidence_policy_reference(document, "measured_rest")
    store_key = core.ContentAddressedStoreKey("sha256/stop-policy-python")
    qualified = core.resolve_and_qualify_stop_evidence_policy(
        reference, store_key, document
    )
    assert isinstance(qualified, core.QualifiedStopEvidencePolicy)
    assert qualified.value.stop_evidence_policy_id == core.StopEvidencePolicyId(
        "measured_rest"
    )
    assert (
        qualified.value.evidence_requirements[0].evidence_kind
        == core.StopEvidenceRequirementKind.kControllerGoalTerminal
    )
    assert json.loads(qualified.normalized_canonical_json) == json.loads(
        document
    )


def test_stop_evidence_policy_rejects_cross_field_and_referent_defects():
    invalid_document = _stop_evidence_policy_document(
        policy_id="invalid_matrix",
        evidence_kind="motion_rest",
        source_kind="provider_evidence",
        observation_kind="post_command_dwell",
    )
    invalid_reference = _stop_evidence_policy_reference(
        invalid_document, "invalid_matrix"
    )
    store_key = core.ContentAddressedStoreKey("sha256/stop-policy-python")
    with pytest.raises(core.StopEvidencePolicyCodecError):
        core.resolve_and_qualify_stop_evidence_policy(
            invalid_reference, store_key, invalid_document
        )

    document = _stop_evidence_policy_document()
    reference = _stop_evidence_policy_reference(document, "measured_rest")
    with pytest.raises(
        core.StopEvidencePolicyCodecError, match="different store key"
    ):
        core.resolve_and_qualify_stop_evidence_policy(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            document,
        )
    wrong_digest = core.StopEvidencePolicyCatalogRecordRefDTO(
        record_id=reference.record_id,
        record_revision=reference.record_revision,
        schema_id=reference.schema_id,
        schema_revision=reference.schema_revision,
        content_digest=core.ContentDigest(ZERO_DIGEST),
        content_addressed_store_key=reference.content_addressed_store_key,
    )
    with pytest.raises(core.StopEvidencePolicyCodecError):
        core.resolve_and_qualify_stop_evidence_policy(
            wrong_digest, store_key, document
        )


def test_json_value_is_explicit_recursive_and_immutable():
    value = core.JsonValueDTO(
        {
            "array": [None, False, 2, -1, {"inner": 1.5}, "item"],
            "enabled": True,
        }
    )
    assert value.value["enabled"].value is True
    array = value.value["array"].value
    assert array[0].value is None
    assert array[1].value is False
    assert array[2].value == 2.0
    assert array[3].value == -1.0
    assert array[4].value["inner"].value == 1.5
    assert array[5].value == "item"
    projected = value.value
    projected["other"] = core.JsonValueDTO("not retained")
    assert "other" not in value.value
    with pytest.raises(AttributeError):
        value.value = None


@pytest.mark.parametrize(
    ("source", "expected", "expected_type"),
    (
        (None, None, type(None)),
        (False, False, bool),
        (True, True, bool),
        (0, 0.0, float),
        (1, 1.0, float),
        (2, 2.0, float),
        (-1, -1.0, float),
        ((2**53) - 1, float((2**53) - 1), float),
        (0.0, 0.0, float),
        (1.5, 1.5, float),
    ),
)
def test_json_value_dispatch_preserves_exact_scalar_kind(
    source, expected, expected_type
):
    projected = core.JsonValueDTO(source).value
    assert projected == expected
    assert type(projected) is expected_type


@pytest.mark.parametrize(
    "source",
    (
        2**53,
        -(2**53),
        float(2**53),
        -float(2**53),
        float("inf"),
        -float("inf"),
        float("nan"),
        -0.0,
    ),
)
def test_json_value_rejects_values_outside_qualified_number_model(source):
    with pytest.raises(ValueError):
        core.JsonValueDTO(source)


@pytest.mark.parametrize(
    "source",
    (
        (1, 2),
        {1, 2},
        {"valid": object()},
        {1: "non-string key"},
    ),
)
def test_json_value_rejects_non_json_python_shapes(source):
    with pytest.raises(TypeError):
        core.JsonValueDTO(source)


def test_json_value_rejects_cycles_but_allows_shared_acyclic_values():
    self_list = []
    self_list.append(self_list)
    with pytest.raises(ValueError, match="cyclic JSON"):
        core.JsonValueDTO(self_list)

    self_dict = {}
    self_dict["self"] = self_dict
    with pytest.raises(ValueError, match="cyclic JSON"):
        core.JsonValueDTO(self_dict)

    shared = [1, 2]
    assert len(core.JsonValueDTO([shared, shared]).value) == 2


def test_json_value_enforces_v1_depth_and_node_bounds():
    nested = None
    for _ in range(64):
        nested = [nested]
    core.JsonValueDTO(nested)

    nested = [nested]
    with pytest.raises(ValueError, match="nesting-depth limit"):
        core.JsonValueDTO(nested)

    with pytest.raises(ValueError, match="node-count limit"):
        core.JsonValueDTO([None] * 250000)


def test_all_six_graph_records_and_envelope_round_trip_read_only():
    values = _cell_graph()
    graph = values["graph"]

    assert graph.api_version == "manyforge.cell_graph.v1"
    assert graph.graph_revision == core.GraphRevision("sha256:graph")
    assert graph.cell_id == core.CellId("cell/test")
    assert graph.runtime_units == [values["runtime_unit"]]
    assert graph.model_instances == [values["model_instance"]]
    assert graph.controllable_components == [values["component"]]
    assert graph.control_domains == [values["domain"]]
    assert graph.control_bindings == [values["binding"]]
    assert graph.component_attachments == [values["attachment"]]
    assert values["model_instance"].root_pose_in_cell is None
    assert values["component"].root_pose_in_cell == core.PlacementPoseDTO(
        position_m=[0.0, 0.0, 0.0],
        orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    assert (
        values["attachment"].child_interface_pose_in_parent_interface
        == values["component"].root_pose_in_cell
    )
    for value, removed_field in (
        (values["runtime_unit"], "runtime_profile_digest"),
        (values["model_instance"], "instance_parameters"),
        (values["model_instance"], "mount_parent_frame_role"),
        (values["model_instance"], "mount_pose"),
        (values["component"], "component_type_digest"),
        (values["binding"], "provider_digest"),
        (values["binding"], "stop_evidence_policy_digest"),
        (values["attachment"], "mount_pose"),
        (values["attachment"], "role"),
        (values["attachment"], "detachable"),
        (values["attachment"], "tcp_effects"),
        (values["attachment"], "collision_effects"),
        (values["attachment"], "provenance_digest"),
    ):
        assert not hasattr(value, removed_field)
    assert not hasattr(core, "MountPoseDTO")
    assert not hasattr(core, "AttachmentRoleId")

    expected_record_types = {
        "RuntimeUnitDTO",
        "ModelInstanceDTO",
        "ControllableComponentDTO",
        "ControlDomainDTO",
        "ControlBindingDTO",
        "ComponentAttachmentEdgeDTO",
    }
    assert {
        type(values[name]).__name__
        for name in (
            "runtime_unit",
            "model_instance",
            "component",
            "domain",
            "binding",
            "attachment",
        )
    } == expected_record_types

    for instance, field in (
        (values["runtime_unit"], "runtime_unit_id"),
        (values["model_instance"], "model_instance_id"),
        (values["component"], "controllable_component_id"),
        (values["domain"], "control_domain_id"),
        (values["binding"], "control_binding_id"),
        (values["attachment"], "component_attachment_id"),
        (graph, "graph_revision"),
    ):
        with pytest.raises(AttributeError):
            setattr(instance, field, getattr(instance, field))

    projected_units = graph.runtime_units
    projected_units.clear()
    assert graph.runtime_units == [values["runtime_unit"]]


def test_removed_model_instance_parameters_keyword_is_rejected():
    with pytest.raises(TypeError):
        core.ModelInstanceDTO(
            model_instance_id=core.ModelInstanceId("model/legacy"),
            model_asset_id=core.ModelAssetId("asset/legacy"),
            model_revision=core.ModelRevision("sha256:legacy"),
            instance_parameters={},
        )


def test_graph_dto_constructors_do_not_coerce_raw_strings_to_strong_ids():
    with pytest.raises(TypeError):
        core.RuntimeUnitDTO(
            runtime_unit_id="runtime/arm",
            runtime_profile_id=core.RuntimeProfileId("profile"),
            runtime_profile_revision=core.RuntimeProfileRevision("revision"),
            dependency_runtime_unit_ids=[],
            configuration={},
        )


def test_deployment_trust_phases_are_immutable_and_explicit():
    parsed = core.parse_strict_json_model_yaml(EMPTY_GRAPH_YAML)
    assert isinstance(parsed, core.ParsedArtifactCandidate)
    assert json.loads(parsed.canonical_json)["cell_id"] == "cell_a"

    typed = core.decode_cell_graph_candidate(parsed)
    assert isinstance(typed, core.TypedCellGraphCandidate)
    assert typed.graph.cell_id == core.CellId("cell_a")
    assert (
        typed.structurally_normalized_canonical_json
        == parsed.canonical_json
    )

    validated = core.validate_intrinsic_cell_graph_candidate(typed)
    assert isinstance(
        validated, core.IntrinsicallyValidatedCellGraphCandidate
    )
    assert validated.graph == typed.graph
    assert (
        validated.structurally_normalized_canonical_json
        == typed.structurally_normalized_canonical_json
    )

    for candidate, field in (
        (parsed, "canonical_json"),
        (typed, "graph"),
        (validated, "graph"),
    ):
        with pytest.raises(AttributeError):
            setattr(candidate, field, getattr(candidate, field))
        with pytest.raises(AttributeError):
            candidate.extra = "not allowed"

    for candidate_type in (
        core.ParsedArtifactCandidate,
        core.TypedCellGraphCandidate,
        core.IntrinsicallyValidatedCellGraphCandidate,
    ):
        with pytest.raises(TypeError):
            candidate_type()


def test_deployment_trust_phase_failures_map_to_value_error():
    assert issubclass(core.ArtifactCodecError, ValueError)
    assert issubclass(core.CellGraphCodecError, ValueError)

    with pytest.raises(
        core.ArtifactCodecError, match="duplicate mapping key"
    ) as parse_error:
        core.parse_strict_json_model_yaml("key: one\nkey: two\n")
    assert type(parse_error.value) is core.ArtifactCodecError

    incomplete = core.parse_strict_json_model_yaml(
        "api_version: manyforge.cell_graph.v1\n"
    )
    with pytest.raises(
        core.CellGraphCodecError, match="missing required field"
    ) as decode_error:
        core.decode_cell_graph_candidate(incomplete)
    assert type(decode_error.value) is core.CellGraphCodecError

    dangling_runtime = core.decode_cell_graph_candidate(
        core.parse_strict_json_model_yaml(
            f"""\
api_version: manyforge.cell_graph.v1
graph_revision: {ZERO_DIGEST}
cell_id: cell_a
runtime_units:
  - runtime_unit_id: driver
    runtime_profile_id: fake_driver
    runtime_profile_revision: {ZERO_DIGEST}
    dependency_runtime_unit_ids: [missing]
    configuration: {{}}
model_instances: []
controllable_components: []
control_domains: []
control_bindings: []
component_attachments: []
"""
        )
    )
    with pytest.raises(
        core.CellGraphCodecError, match="dangling reference"
    ) as validation_error:
        core.validate_intrinsic_cell_graph_candidate(dangling_runtime)
    assert type(validation_error.value) is core.CellGraphCodecError


def test_closed_schema_trust_phases_and_registry_are_immutable():
    descriptor_yaml = (
        CLOSED_SCHEMA_RESOURCES
        / "examples"
        / "manyforge.closed_schema.example_command.v1.yaml"
    ).read_text(encoding="utf-8")
    schema = core.compile_closed_schema_descriptor(
        core.parse_strict_json_model_yaml(descriptor_yaml)
    )

    assert isinstance(schema, core.CompiledClosedSchema)
    assert schema.schema_id == core.SchemaId(
        "manyforge.closed_schema.example_command.v1"
    )
    assert schema.schema_revision == core.SchemaRevision(
        "sha256:37985bd40f1c50cca9a82e3952bf407ddecca0d1c32d7787cd903bfffbf5fce7"
    )
    assert schema.schema_purpose == core.ClosedSchemaPurpose.kArtifactDocument
    assert schema.root_shape == core.ClosedSchemaRootShape.kTaggedUnion
    assert (
        json.loads(schema.normalized_descriptor_canonical_json)["schema_id"]
        == schema.schema_id.value
    )

    artifact = core.validate_and_normalize_unqualified_artifact_candidate(
        schema,
        core.parse_strict_json_model_yaml(
            """\
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
target: 0.5
"""
        ),
    )
    assert isinstance(
        artifact, core.UnqualifiedSchemaNormalizedArtifactCandidate
    )
    assert artifact.schema_id == schema.schema_id
    assert artifact.schema_revision == schema.schema_revision
    assert artifact.schema_purpose == schema.schema_purpose
    assert json.loads(artifact.normalized_canonical_json) == {
        "api_version": "manyforge.closed_schema.example_command.v1",
        "command_kind": "position",
        "target": 0.5,
    }

    registry = core.build_qualified_closed_schema_registry()
    assert isinstance(registry, core.BuildQualifiedClosedSchemaRegistry)
    assert isinstance(registry.catalog_schemas, tuple)
    assert len(registry.catalog_schemas) == 11
    qualified_entries = [
        entry
        for entry in registry.catalog_schemas
        if entry.qualification == core.CatalogSchemaQualification.kQualified
    ]
    assert len(qualified_entries) == 4
    assert {entry.record_kind for entry in qualified_entries} == {
        core.CatalogRecordKind.kCapabilityDefinition,
        core.CatalogRecordKind.kControlInterfaceContract,
        core.CatalogRecordKind.kModelAsset,
        core.CatalogRecordKind.kStopEvidencePolicy,
    }
    assert all(entry.compiled_schema is not None for entry in qualified_entries)
    assert {entry.relative_path for entry in qualified_entries} == {
        "catalog/manyforge.capability_definition.v1.yaml",
        "catalog/manyforge.control_interface_contract.v1.yaml",
        "catalog/manyforge.model_asset.v1.yaml",
        "catalog/manyforge.stop_evidence_policy.v1.yaml",
    }
    assert all(
        entry.compiled_schema is None and entry.relative_path == ""
        for entry in registry.catalog_schemas
        if entry.qualification == core.CatalogSchemaQualification.kUnqualified
    )
    assert registry.value_schemas == ()
    assert registry.qualification_registry_digest.value == (
        "sha256:f1e038cc6895c8939e946dcab4d9923a9487957e421a60f6af632d6f5c4b71d7"
    )
    assert (
        json.loads(registry.normalized_projection_canonical_json)["catalog_schemas"][0][
            "qualification"
        ]
        == "qualified"
    )
    assert core.CLOSED_SCHEMA_API_VERSION == "manyforge.closed_schema.v1"
    assert core.CLOSED_SCHEMA_BOOTSTRAP_CONTRACT_DIGEST == (
        "sha256:29a2fef8e781ca406eaaad448e46fb6061fe18e99e2c1cd39f303e2e0d815834"
    )

    for value, field in (
        (schema, "schema_id"),
        (artifact, "normalized_canonical_json"),
        (registry, "catalog_schemas"),
        (registry.catalog_schemas[0], "qualification"),
    ):
        with pytest.raises(AttributeError):
            setattr(value, field, getattr(value, field))
        with pytest.raises(AttributeError):
            value.extra = "not allowed"

    for value_type in (
        core.CompiledClosedSchema,
        core.UnqualifiedSchemaNormalizedArtifactCandidate,
        core.BuildCatalogSchemaEntry,
        core.BuildQualifiedValueSchemaEntry,
        core.BuildQualifiedClosedSchemaRegistry,
    ):
        with pytest.raises(TypeError):
            value_type()


def test_closed_schema_failures_map_to_value_error():
    assert issubclass(core.ClosedSchemaError, ValueError)

    descriptor_yaml = (
        CLOSED_SCHEMA_RESOURCES
        / "examples"
        / "manyforge.closed_schema.example_command.v1.yaml"
    ).read_text(encoding="utf-8")
    schema = core.compile_closed_schema_descriptor(
        core.parse_strict_json_model_yaml(descriptor_yaml)
    )
    with pytest.raises(
        core.ClosedSchemaError, match="above maximum"
    ) as normalization_error:
        core.validate_and_normalize_unqualified_artifact_candidate(
            schema,
            core.parse_strict_json_model_yaml(
                """\
api_version: manyforge.closed_schema.example_command.v1
command_kind: position
target: 2
"""
            ),
        )
    assert type(normalization_error.value) is core.ClosedSchemaError


def test_python_exposes_no_premature_graph_revision_trust_surface():
    for forbidden_name in (
        "compute_graph_revision",
        "verify_graph_revision",
        "TrustedCellGraph",
        "TrustedCellGraphCandidate",
    ):
        assert not hasattr(core, forbidden_name)
