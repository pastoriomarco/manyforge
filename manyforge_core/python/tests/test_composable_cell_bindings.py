"""C0.1 parity tests for the immutable composable-cell core projection."""

from __future__ import annotations

import json
import typing

import pytest

core = pytest.importorskip("manyforge_core_py")


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
    "CommandTargetId",
    "CapabilityId",
    "CapabilityDefinitionRevision",
    "FrameRoleId",
    "CollisionBodyRoleId",
    "ResourceId",
    "ResourceRoleId",
    "AttachmentInterfaceId",
    "AttachmentRoleId",
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
    "ContactPolicyId",
    "ContactPolicyRevision",
    "ProviderInstanceId",
    "ProviderManifestRevision",
    "ProviderDigest",
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
        runtime_profile_digest=core.ContentDigest("sha256:runtime-profile"),
        dependency_runtime_unit_ids=[],
        configuration=configuration,
    )
    mount_pose = core.MountPoseDTO(
        position_m=[0.0, 0.0, 0.0],
        orientation_xyzw=[0.0, 0.0, 0.0, 1.0],
    )
    model_instance = core.ModelInstanceDTO(
        model_instance_id=core.ModelInstanceId("model/arm"),
        model_asset_id=core.ModelAssetId("asset/lite6"),
        model_revision=core.ModelRevision("sha256:model"),
        mount_parent_frame_role=core.FrameRoleId("world"),
        mount_pose=mount_pose,
        instance_parameters={"prefix": core.JsonValueDTO("arm_")},
    )
    component = core.ControllableComponentDTO(
        controllable_component_id=core.ControllableComponentId("component/arm"),
        component_type_id=core.ComponentTypeId("component-type/lite6"),
        component_type_revision=core.ComponentTypeRevision("sha256:component-type"),
        component_type_digest=core.ContentDigest("sha256:component-type-bytes"),
        model_role_bindings=[
            core.ModelRoleBindingDTO(
                model_role_id=core.ModelRoleId("primary"),
                model_instance_id=model_instance.model_instance_id,
            )
        ],
        runtime_unit_ids=[runtime_unit.runtime_unit_id],
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
        provider_digest=core.ProviderDigest("sha256:provider"),
        state_authority_id=core.StateAuthorityId("state/measured"),
        state_authority_class=core.StateAuthorityClass.kMeasured,
        stop_evidence_policy_id=core.StopEvidencePolicyId("stop/measured-rest"),
        stop_evidence_policy_revision=core.StopEvidencePolicyRevision(
            "sha256:stop-policy"
        ),
        stop_evidence_policy_digest=core.ContentDigest("sha256:stop-policy-bytes"),
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
        mount_pose=mount_pose,
        role=core.AttachmentRoleId("tool"),
        detachable=True,
        tcp_effects=[core.FrameRoleId("tool_tcp")],
        collision_effects=[core.CollisionBodyRoleId("tool_body")],
        provenance_digest=core.ContentDigest("sha256:attachment-provenance"),
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


def test_graph_dto_constructors_do_not_coerce_raw_strings_to_strong_ids():
    with pytest.raises(TypeError):
        core.RuntimeUnitDTO(
            runtime_unit_id="runtime/arm",
            runtime_profile_id=core.RuntimeProfileId("profile"),
            runtime_profile_digest=core.ContentDigest("digest"),
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
        core.CellGraphCodecError, match="wrong number of fields"
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
    runtime_profile_digest: {ZERO_DIGEST}
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


def test_python_exposes_no_premature_graph_revision_trust_surface():
    for forbidden_name in (
        "compute_graph_revision",
        "verify_graph_revision",
        "TrustedCellGraph",
        "TrustedCellGraphCandidate",
    ):
        assert not hasattr(core, forbidden_name)
