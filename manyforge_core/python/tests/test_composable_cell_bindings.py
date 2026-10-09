"""C0.2 parity tests for immutable composable-cell deployment boundaries."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import typing

import pytest

core = pytest.importorskip("manyforge_core_py")


def test_stop_timing_keeps_ramp_allowances_and_operational_timeout_distinct():
    timing = core.StopTimingParametersDTO(250_000_000, 750_000_000, 2_000_000_000)
    assert timing.command_latency_allowance_ns == 250_000_000
    assert timing.settling_allowance_ns == 750_000_000
    assert timing.operational_timeout_ns == 2_000_000_000
    assert core.validate_controlled_stop_timing(timing, 1_000_000_000) is None
    # Item 24: no Apply-time refusal compares the timeout with the profile.
    assert core.validate_controlled_stop_timing(timing, 1_000_000_001) is None
    with pytest.raises(AttributeError):
        timing.operational_timeout_ns = 1
    selection = core.StopTimingSelectionDTO(
        core.ControlBindingId("arm"),
        core.ControlBindingRevision("sha256:" + "a" * 64),
        timing,
    )
    inputs = core.DeploymentApplicabilityInputsDTO(
        None, [], [], [], [], [], [], [selection]
    )
    assert inputs.stop_timing_parameters == [selection]
    assert inputs.stop_timing_parameters[0].timing == timing
    with pytest.raises(AttributeError):
        selection.timing = core.StopTimingParametersDTO(1, 0, 2)


def test_authored_stop_duration_uses_the_same_decimal_rounding_as_deadlines():
    assert core.authored_stop_duration_ns(1.1) == 1_100_000_000
    assert core.authored_stop_duration_ns(1.100000001) == 1_100_000_001
    for invalid in (0, -1, float("nan"), float("inf"), 2.1):
        with pytest.raises(ValueError):
            core.authored_stop_duration_ns(invalid)


def test_gripper_evidence_preserves_provenance_and_pure_expiry():
    observation = core.GripperGraspObservationDTO()
    observation.gripper_id = "tool.gripper"
    observation.command_id = 7
    observation.generation = 9
    observation.expected_status_revision = 3
    observation.observation_id = "operator-press"
    observation.confirmed = True
    observation.observed_at_ns = 31 * 365 * 24 * 60 * 60 * 1_000_000_000
    observation.attribution = "operator:station-a"
    evidence = core.GripperGraspEvidenceDTO(
        source_kind=core.GraspConfirmationSourceKind.kOperator,
        source_id=core.ControlBindingId("binding.operator"),
        source_revision=core.ControlBindingRevision("revision.operator"),
        source_epoch=core.RuntimeEpoch("owner.1"),
        observation=observation,
        expires_at_ns=observation.observed_at_ns + 100,
    )
    assert core.gripper_grasp_evidence_is_fresh(evidence, observation.observed_at_ns)
    assert not core.gripper_grasp_evidence_is_fresh(
        evidence, observation.observed_at_ns - 1
    )
    assert not core.gripper_grasp_evidence_is_fresh(evidence, evidence.expires_at_ns)
    assert evidence.observation.observed_at_ns == observation.observed_at_ns
    assert evidence.observation.expected_status_revision == 3
    assert evidence.source_id == core.ControlBindingId("binding.operator")
    assert evidence.source_epoch == core.RuntimeEpoch("owner.1")


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
CLOSED_SCHEMA_RESOURCES = (
    REPOSITORY_ROOT / "manyforge_deployment" / "resources" / "closed_schema"
)

STRONG_STRING_TYPES = (
    "CellId",
    "GraphRevision",
    "DeploymentArtifactId",
    "DeploymentRevision",
    "SchemaId",
    "SchemaRevision",
    "ContentDigest",
    "ContentAddressedStoreKey",
    "RuntimeProfileId",
    "RuntimeProfileRevision",
    "RuntimeRoleId",
    "RuntimeUnitId",
    "RuntimeEpoch",
    "RuntimePolicyId",
    "RuntimePolicyRevision",
    "InterruptSourceId",
    "InterruptPolicyId",
    "InterruptLatchId",
    "MotionInterruptScopeId",
    "ComponentTypeId",
    "ComponentTypeRevision",
    "ControllableComponentId",
    "ModelAssetId",
    "ModelInstanceId",
    "ModelRevision",
    "ModelRoleId",
    "ModelGroupRoleId",
    "ComponentRoleId",
    "CommandTargetId",
    "TargetRoleId",
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
    "ControllerRoleId",
    "ActivationSetId",
    "ControlRouteId",
    "ScalingDomainId",
    "CoordinatedScalingBarrierId",
    "ControlDomainId",
    "ControlDomainRevision",
    "ControlBindingId",
    "ControlBindingRevision",
    "ActiveControlConfigurationId",
    "ResolvedStopBasePlanId",
    "EndpointRoleId",
    "StateAuthorityId",
    "StopEvidencePolicyId",
    "StopEvidencePolicyRevision",
    "StopEscalationStageId",
    "StopBudgetPhaseId",
    "StopEvidenceItemId",
    "ProviderContractRoleId",
    "RegisteredStopCommandKindId",
    "RegisteredStopEvidenceKindId",
    "CommissionedStopInterfaceId",
    "CommissionedStopInterfaceRevision",
    "StopInvocationLaneId",
    "ContactPolicyId",
    "ContactPolicyRevision",
    "ProviderInstanceId",
    "ProviderManifestId",
    "ProviderManifestRevision",
    "CapabilityProviderBindingId",
    "CapabilityProviderBindingRevision",
    "SiteOverlayId",
    "SiteOverlayRevision",
    "ExternalResourceAlias",
    "SiteOverlayValueRef",
    "ProductId",
    "ProductContractId",
    "ProductContractRevision",
    "ArtifactRoleId",
    "ComponentAttachmentId",
    "ComponentAttachmentRevision",
    "RetainedBundleId",
    "SceneId",
    "ProjectionLeaseId",
    "CommandSchemaId",
    "CommandSchemaRevision",
    "CommandOptionId",
    "ChannelRoleId",
    "ProgramId",
    "ProgramRevision",
    "ProgramBindingId",
    "ProgramRunId",
    "MoveId",
    "RequestOwnerEpoch",
    "RequestId",
    "LifecycleTransactionId",
    "LifecycleFactId",
    "LifecyclePrimitiveId",
    "LifecyclePrimitiveAttemptId",
    "ConfigurationFenceId",
    "OperationId",
    "StopId",
    "StopCertificateId",
    "StopFaultLatchId",
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
        "ProviderManifestId",
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
        "manyforge.stop_evidence_policy.v2",
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

    phases = [
        {"phase_id": "command", "deadline_offset_ns": 100},
        {"phase_id": "terminal", "deadline_offset_ns": 500},
    ]
    stages = [
        {
            "stop_escalation_stage_id": "nominal",
            "stage_kind": "nominal",
            "containment_granularity": "control_domain",
            "containment_command": {
                "command_phase_id": "command",
                "command": {"command_kind": "controller_goal_cancel"},
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
    ]
    if evidence_kind == "motion_rest":
        phases.append({"phase_id": "hard_command", "deadline_offset_ns": 700})
        stages.append(
            {
                "stop_escalation_stage_id": "software_hard_floor",
                "stage_kind": "software_hard",
                "containment_granularity": "control_domain",
                "containment_command": {
                    "command_phase_id": "hard_command",
                    "command": {
                        "command_kind": "registered",
                        "registered_command_kind_id": "software_hard_stop",
                        "provider_contract_ref": {
                            "provider_contract_role_id": "software_hard_stop",
                            "contract_schema_id": (
                                "manyforge.stop_provider_contract.v1"
                            ),
                            "contract_schema_revision": ZERO_DIGEST,
                        },
                    },
                },
                "evidence_conjunction": "all_of",
                "evidence_requirements": [
                    {
                        "evidence_item_id": "hard_acknowledged",
                        "phase_id": "hard_command",
                        "evidence": {"evidence_kind": "discrete_acknowledgement"},
                        "source_requirement": {
                            "source_kind": "command_target_status",
                            "freshness_budget_ns": 25,
                        },
                        "observation_semantics": {"observation_kind": "command_result"},
                    }
                ],
            }
        )

    payload = {
        "api_version": "manyforge.stop_evidence_policy.v2",
        "stop_evidence_policy_id": policy_id,
        "disposition": "stoppable",
        "containment_granularity": "control_domain",
        "deadline_budget": {
            "outer_duration_ns": 1000,
            "kernel_completion_reserve_ns": 100,
            "phases": phases,
        },
        "escalation_stages": stages,
    }
    canonical_payload = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/stop_evidence_policy"
        b"\x00manyforge.stop_evidence_policy.v2\x00" + canonical_payload.encode()
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
        schema_id=core.SchemaId("manyforge.stop_evidence_policy.v2"),
        schema_revision=core.SchemaRevision(
            "sha256:c75dcdd59f2101b1c47ccc020cc61b28037256a40dc71fe896f0e4d912f4a970"
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
        runtime_profile_revision=core.RuntimeProfileRevision("sha256:runtime-profile"),
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
        control_interface_revision=core.ControlInterfaceRevision("sha256:interface"),
        controller_topology_id=domain.controller_topology_id,
        topology_revision=domain.topology_revision,
        legal_control_route_ids=[route_projection.control_route_id],
        route_endpoint_projections=[route_projection],
        provider_instance_id=core.ProviderInstanceId("provider/fake"),
        provider_runtime_unit_id=runtime_unit.runtime_unit_id,
        provider_manifest_id=core.ProviderManifestId("provider/xarm"),
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


class _GuardedOverBoundIterable:
    def __init__(self, value, maximum: int):
        self._value = value
        self._maximum = maximum
        self.items_requested = 0

    def __iter__(self):
        while self.items_requested <= self._maximum:
            self.items_requested += 1
            yield self._value
        raise AssertionError("binding consumed beyond the first over-bound item")


def _applicability_constructor_fixture():
    route = core.DesiredControlBindingRouteDTO(
        control_binding_id=core.ControlBindingId("binding"),
        control_binding_revision=core.ControlBindingRevision(ZERO_DIGEST),
        control_route_id=core.ControlRouteId("route"),
    )
    configuration_kwargs = {
        "control_domain_id": core.ControlDomainId("domain"),
        "control_domain_revision": core.ControlDomainRevision(ZERO_DIGEST),
        "controller_topology_id": core.ControllerTopologyId("topology"),
        "topology_revision": core.TopologyRevision(ZERO_DIGEST),
        "activation_set_id": core.ActivationSetId("activation"),
        "binding_routes": (route,),
    }
    configuration = core.DesiredControlConfigurationDTO(**configuration_kwargs)

    endpoint = core.CapabilityProviderEndpointBindingDTO(
        endpoint_role_id=core.EndpointRoleId("endpoint"),
        source_kind=core.CapabilityEndpointSourceKind.kRuntimeUnit,
        source_id=core.RuntimeUnitId("runtime"),
        source_endpoint_role_id=core.EndpointRoleId("source_endpoint"),
    )
    provider_kwargs = {
        "capability_provider_binding_id": core.CapabilityProviderBindingId(
            "provider_binding"
        ),
        "capability_provider_binding_revision": (
            core.CapabilityProviderBindingRevision(ZERO_DIGEST)
        ),
        "capability_id": core.CapabilityId("capability"),
        "capability_definition_revision": core.CapabilityDefinitionRevision(
            ZERO_DIGEST
        ),
        "scope": core.CapabilityScope.kCellGlobal,
        "target": core.CellGlobalCapabilityProviderTargetDTO(
            cell_id=core.CellId("cell")
        ),
        "provider_instance_id": core.ProviderInstanceId("provider"),
        "provider_manifest_id": core.ProviderManifestId("manifest"),
        "provider_manifest_revision": core.ProviderManifestRevision(ZERO_DIGEST),
        "endpoint_bindings": (endpoint,),
    }
    provider = core.CapabilityProviderBindingDTO(**provider_kwargs)

    external = core.ExternalResourceDTO(
        external_resource_alias=core.ExternalResourceAlias("external"),
        resource_kind=core.ExternalResourceKind.kServiceEndpoint,
        provider_instance_id=core.ProviderInstanceId("provider"),
        provider_manifest_id=core.ProviderManifestId("manifest"),
        provider_manifest_revision=core.ProviderManifestRevision(ZERO_DIGEST),
        endpoint_role_id=core.EndpointRoleId("endpoint"),
        site_overlay_value_ref=core.SiteOverlayValueRef("resources.external.endpoint"),
    )
    applicability_kwargs = {
        "site_overlay_ref": None,
        "desired_control_configurations": (configuration,),
        "capability_provider_bindings": (provider,),
        "external_resources": (external,),
        "stop_stage_parameters": (),
        "commissioned_stop_interfaces": (),
        "retained_policy_records": (),
    }
    return {
        "binding_routes": (
            core.DesiredControlConfigurationDTO,
            configuration_kwargs,
            route,
        ),
        "endpoint_bindings": (
            core.CapabilityProviderBindingDTO,
            provider_kwargs,
            endpoint,
        ),
        "desired_control_configurations": (
            core.DeploymentApplicabilityInputsDTO,
            applicability_kwargs,
            configuration,
        ),
        "capability_provider_bindings": (
            core.DeploymentApplicabilityInputsDTO,
            applicability_kwargs,
            provider,
        ),
        "external_resources": (
            core.DeploymentApplicabilityInputsDTO,
            applicability_kwargs,
            external,
        ),
    }


@pytest.mark.parametrize(
    ("field", "maximum"),
    (
        ("binding_routes", core.MAX_DEPLOYMENT_APPLICABILITY_ROWS),
        ("endpoint_bindings", core.MAX_CAPABILITY_PROVIDER_ENDPOINT_BINDINGS),
        (
            "desired_control_configurations",
            core.MAX_DEPLOYMENT_APPLICABILITY_ROWS,
        ),
        (
            "capability_provider_bindings",
            core.MAX_DEPLOYMENT_APPLICABILITY_ROWS,
        ),
        ("external_resources", core.MAX_DEPLOYMENT_APPLICABILITY_ROWS),
    ),
)
def test_applicability_constructors_accept_their_exact_iterable_bound(field, maximum):
    constructor, base_kwargs, value = _applicability_constructor_fixture()[field]
    kwargs = dict(base_kwargs)
    kwargs[field] = (value for _ in range(maximum))

    constructed = constructor(**kwargs)

    assert len(getattr(constructed, field)) == maximum


@pytest.mark.parametrize(
    ("field", "maximum"),
    (
        ("binding_routes", core.MAX_DEPLOYMENT_APPLICABILITY_ROWS),
        ("endpoint_bindings", core.MAX_CAPABILITY_PROVIDER_ENDPOINT_BINDINGS),
        (
            "desired_control_configurations",
            core.MAX_DEPLOYMENT_APPLICABILITY_ROWS,
        ),
        (
            "capability_provider_bindings",
            core.MAX_DEPLOYMENT_APPLICABILITY_ROWS,
        ),
        ("external_resources", core.MAX_DEPLOYMENT_APPLICABILITY_ROWS),
    ),
)
def test_applicability_constructors_stop_at_the_first_over_bound_item(field, maximum):
    constructor, base_kwargs, value = _applicability_constructor_fixture()[field]
    guarded = _GuardedOverBoundIterable(value, maximum)
    kwargs = dict(base_kwargs)
    kwargs[field] = guarded

    with pytest.raises(
        core.DeploymentApplicabilityCodecError,
        match=rf"{field} exceeds its public compiler-ingress bound",
    ):
        constructor(**kwargs)

    assert guarded.items_requested == maximum + 1


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

    assert len(STRONG_STRING_TYPES) == 105
    assert len({type(value) for value in values}) == len(STRONG_STRING_TYPES)
    assert len(set(values)) == len(STRONG_STRING_TYPES)
    for index, left in enumerate(values):
        for right in values[index + 1 :]:
            assert left != right
    assert core.CellId("shared") != core.GraphRevision("shared")
    assert len({core.CellId("shared"), core.GraphRevision("shared")}) == 2

    with pytest.raises(TypeError):

        class InvalidSubclass(core.CellId):
            pass


def test_stop_terminal_fault_generation_is_a_distinct_immutable_counter():
    generation = core.StopFaultLatchGeneration(7)
    assert generation.value == 7
    assert generation.nonzero is True
    assert generation == core.StopFaultLatchGeneration(7)
    assert generation != core.StopGeneration(7)
    assert generation != core.StopCertificateRevision(7)
    with pytest.raises(AttributeError):
        generation.value = 8


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
    assert len(union_members) == len(CATALOG_REF_CASES) == 10
    assert len(core.CATALOG_RECORD_TYPE_REGISTRY) == 10
    assert (
        len(
            {descriptor.record_kind for descriptor in core.CATALOG_RECORD_TYPE_REGISTRY}
        )
        == 10
    )

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

    with pytest.raises(TypeError):
        core.ProviderManifestCatalogRecordRefDTO(
            record_id=core.ProviderInstanceId("legacy-instance-key"),
            record_revision=core.ProviderManifestRevision("revision"),
            schema_id=core.SchemaId("manyforge.provider_manifest.v1"),
            schema_revision=core.SchemaRevision("schema-revision"),
            content_digest=core.ContentDigest("sha256:content"),
            content_addressed_store_key=core.ContentAddressedStoreKey("cas:key"),
        )


def test_capability_definition_registry_and_qualified_referent_are_exact():
    assert (
        core.CAPABILITY_DEFINITION_API_VERSION == "manyforge.capability_definition.v1"
    )
    registry = core.BUILTIN_CAPABILITY_DEFINITION_REGISTRY
    assert isinstance(registry, tuple)
    assert len(registry) == 68
    assert len({row.capability_id for row in registry}) == 68
    assert (
        sum(row.scope == core.CapabilityScope.kTargetBinding for row in registry) == 32
    )
    assert sum(row.scope == core.CapabilityScope.kCellGlobal for row in registry) == 28
    assert (
        sum(row.scope == core.CapabilityScope.kControlDomain for row in registry) == 8
    )
    assert (
        sum(
            row.provider_binding_kind
            == core.CapabilityProviderBindingKind.kCapabilityProviderBinding
            for row in registry
        )
        == 47
    )
    assert (
        sum(
            row.provider_binding_kind
            == core.CapabilityProviderBindingKind.kControlBinding
            for row in registry
        )
        == 21
    )
    assert all(row.requirement_parameters_schema_ref is None for row in registry)
    assert core.BUILTIN_CAPABILITY_DEFINITION_REGISTRY_DIGEST == (
        "sha256:ee78bb448062b0c4784697b876fa37653ed56402887488b108deb72a0b163ea3"
    )
    assert core.find_builtin_capability_definition("context.state.current") is None

    descriptor = core.find_builtin_capability_definition("planning.motion.joint_goal")
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
        core.CapabilityDefinitionRevision(descriptor.capability_definition_revision),
        core.SchemaId("manyforge.capability_definition.v1"),
        core.SchemaRevision(
            "sha256:" "2de23ede0e941349e90f0a3a344b7d240426703d250234f86e788343213727e0"
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
    assert json.loads(qualified.normalized_canonical_json) == json.loads(stored_bytes)

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
    assert core.STOP_EVIDENCE_POLICY_API_VERSION == "manyforge.stop_evidence_policy.v2"
    assert core.MAX_STOP_ESCALATION_STAGES == 8
    assert core.MAX_STOP_EVIDENCE_POLICY_ITEMS == 64
    assert core.MAX_STOP_EVIDENCE_POLICY_DURATION_NS == (2**53) - 1

    contract = core.ProviderContractRefDTO(
        provider_contract_role_id=core.ProviderContractRoleId("stop_contract"),
        contract_schema_id=core.SchemaId("manyforge.stop_provider_contract.v1"),
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
            core.StopBudgetPhaseDTO(core.StopBudgetPhaseId("hard_command"), 700),
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
    nominal_stage = core.StopEscalationStageDTO(
        stop_escalation_stage_id=core.StopEscalationStageId("nominal_controlled"),
        stage_kind=core.StopEscalationStageKind.kNominal,
        containment_granularity=core.ContainmentGranularity.kResource,
        containment_command=command,
        evidence_conjunction=core.StopEvidenceConjunction.kAllOf,
        evidence_requirements=[evidence],
    )
    hard_command = core.StopContainmentCommandDTO(
        command_phase_id=core.StopBudgetPhaseId("hard_command"),
        command_kind=core.StopContainmentCommandKind.kRegistered,
        provider_contract_ref=contract,
        registered_command_kind_id=core.RegisteredStopCommandKindId(
            "software_hard_stop"
        ),
    )
    hard_evidence = core.StopEvidenceRequirementDTO(
        evidence_item_id=core.StopEvidenceItemId("hard_acknowledged"),
        phase_id=core.StopBudgetPhaseId("hard_command"),
        evidence_kind=core.StopEvidenceRequirementKind.kDiscreteAcknowledgement,
        source_requirement=core.StopEvidenceSourceRequirementDTO(
            source_kind=core.StopEvidenceSourceKind.kCommandTargetStatus,
            freshness_budget_ns=25,
        ),
        observation_semantics=core.StopEvidenceObservationDTO(
            observation_kind=core.StopEvidenceObservationKind.kCommandResult,
        ),
    )
    hard_stage = core.StopEscalationStageDTO(
        stop_escalation_stage_id=core.StopEscalationStageId("software_hard_floor"),
        stage_kind=core.StopEscalationStageKind.kSoftwareHard,
        containment_granularity=core.ContainmentGranularity.kControlDomain,
        containment_command=hard_command,
        evidence_conjunction=core.StopEvidenceConjunction.kAllOf,
        evidence_requirements=[hard_evidence],
    )
    dto = core.StopEvidencePolicyDTO(
        stop_evidence_policy_id=core.StopEvidencePolicyId("typed_stop"),
        stop_evidence_policy_revision=core.StopEvidencePolicyRevision(ZERO_DIGEST),
        disposition=core.StopEvidenceDisposition.kStoppable,
        containment_granularity=core.ContainmentGranularity.kControlDomain,
        deadline_budget=budget,
        escalation_stages=[nominal_stage, hard_stage],
    )
    assert dto.deadline_budget == budget
    assert dto.escalation_stages == [nominal_stage, hard_stage]
    with pytest.raises(AttributeError):
        dto.escalation_stages = []
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
        qualified.value.escalation_stages[0].evidence_requirements[0].evidence_kind
        == core.StopEvidenceRequirementKind.kControllerGoalTerminal
    )
    assert json.loads(qualified.normalized_canonical_json) == json.loads(document)


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
    with pytest.raises(core.StopEvidencePolicyCodecError, match="different store key"):
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
        core.resolve_and_qualify_stop_evidence_policy(wrong_digest, store_key, document)


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
    assert values["binding"].provider_instance_id == core.ProviderInstanceId(
        "provider/fake"
    )
    assert values["binding"].provider_manifest_id == core.ProviderManifestId(
        "provider/xarm"
    )
    assert values["binding"].runtime_scale_route_qualifications == []
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


def test_control_binding_requires_distinct_manifest_identity():
    binding = _cell_graph()["binding"]
    constructor_fields = (
        "control_binding_id",
        "control_binding_revision",
        "controllable_component_id",
        "command_target_id",
        "capability_id",
        "capability_definition_revision",
        "control_domain_id",
        "resource_bindings",
        "control_interface_contract_id",
        "control_interface_revision",
        "controller_topology_id",
        "topology_revision",
        "legal_control_route_ids",
        "route_endpoint_projections",
        "provider_instance_id",
        "provider_runtime_unit_id",
        "provider_manifest_id",
        "provider_manifest_revision",
        "state_authority_id",
        "state_authority_class",
        "stop_evidence_policy_id",
        "stop_evidence_policy_revision",
        "containment_granularity",
    )
    arguments = {field: getattr(binding, field) for field in constructor_fields}

    legacy_arguments = dict(arguments)
    legacy_arguments.pop("provider_manifest_id")
    with pytest.raises(TypeError):
        core.ControlBindingDTO(**legacy_arguments)

    wrong_type_arguments = dict(arguments)
    wrong_type_arguments["provider_manifest_id"] = binding.provider_instance_id
    with pytest.raises(TypeError):
        core.ControlBindingDTO(**wrong_type_arguments)


def test_runtime_scale_route_qualification_preserves_strong_id_types():
    qualification = core.RuntimeScaleRouteQualificationDTO(
        control_route_id=core.ControlRouteId("route/jtc"),
        scaling_domain_id=core.ScalingDomainId("scale/arm"),
        command_endpoint_role_id=core.EndpointRoleId("runtime_scale_command"),
        applied_state_endpoint_role_id=core.EndpointRoleId("runtime_scale_state"),
        coordinated_barrier_id=core.CoordinatedScalingBarrierId("barrier/cell"),
        acknowledgement_kind=core.RuntimeScaleAcknowledgementKind.kAppliedState,
        maximum_slew_per_second=4.0,
        freshness_budget_ns=100_000_000,
    )

    assert qualification.scaling_domain_id == core.ScalingDomainId("scale/arm")
    assert qualification.coordinated_barrier_id == core.CoordinatedScalingBarrierId(
        "barrier/cell"
    )
    assert qualification.maximum_slew_per_second == 4.0
    assert qualification.freshness_budget_ns == 100_000_000


def test_active_control_configuration_is_complete_typed_and_read_only():
    binding_source = core.KinematicControlBindingSourceDTO(
        control_binding_id=core.ControlBindingId("binding/arm/trajectory"),
        control_binding_revision=core.ControlBindingRevision("sha256:binding"),
    )
    route = core.ActiveControlConfigurationBindingRouteDTO(
        control_binding_id=binding_source.control_binding_id,
        control_binding_revision=binding_source.control_binding_revision,
        control_route_id=core.ControlRouteId("route/jtc"),
    )
    projection = core.KinematicResourceSourceProjectionDTO(
        resource_id=core.ResourceId("arm_joint1"),
        contributing_control_bindings=[binding_source],
        provider_instance_id=core.ProviderInstanceId("provider/fake"),
        provider_manifest_id=core.ProviderManifestId("provider/xarm"),
        provider_manifest_revision=core.ProviderManifestRevision(
            "sha256:provider-manifest"
        ),
        state_authority_id=core.StateAuthorityId("state/measured"),
        state_authority_class=core.StateAuthorityClass.kMeasured,
        publication_source=core.ObservedStateSourceKind.kJointState,
    )
    configuration = core.ActiveControlConfigurationDTO(
        active_control_configuration_id=core.ActiveControlConfigurationId("active/arm"),
        control_domain_id=core.ControlDomainId("domain/arm"),
        control_domain_revision=core.ControlDomainRevision("sha256:domain"),
        binding_routes=[route],
        activation_set_id=core.ActivationSetId("activation/arm"),
        controller_topology_id=core.ControllerTopologyId("topology/arm"),
        topology_revision=core.TopologyRevision("sha256:topology"),
        source_runtime_unit_id=core.RuntimeUnitId("runtime/arm"),
        source_runtime_epoch=core.RuntimeEpoch("epoch/1"),
        source_sequence=7,
        kinematic_resource_source_projections=[projection],
        active_control_configuration_generation=(
            core.ActiveControlConfigurationGeneration(3)
        ),
        state=core.ActiveControlConfigurationStateDTO.kReconciled,
    )

    assert route.locally_valid()
    assert configuration.locally_valid()
    assert configuration.source_runtime_unit_id == core.RuntimeUnitId("runtime/arm")
    assert configuration.binding_routes == (route,)
    assert configuration.kinematic_resource_source_projections == (projection,)

    projected_routes = configuration.binding_routes
    with pytest.raises(AttributeError):
        projected_routes.append(route)
    with pytest.raises(AttributeError):
        configuration.source_runtime_unit_id = core.RuntimeUnitId("runtime/other")


def test_public_stop_contract_dtos_are_typed_and_authority_rows_are_read_only():
    controlled_limit = core.ControlledStopResourceLimitDTO(
        resource_id=core.ResourceId("arm_joint1"),
        max_deceleration=core.AngularAccelerationDTO(2.0),
        settle_velocity_threshold=core.AngularVelocityDTO(0.01),
    )
    hard_limit = core.HardStopResourceLimitDTO(
        resource_id=controlled_limit.resource_id,
        settle_velocity_threshold=core.AngularVelocityDTO(0.02),
    )
    soft = core.SoftOnPathStopParametersDTO(
        stop_mode=core.MotionControlledStopMode.kSoftOnPath,
        deceleration_time_s=0.25,
        per_resource_limits=[controlled_limit],
        allow_natural_completion_on_short_path=False,
        recoverable_substitution_mode=(core.MotionControlledStopMode.kSpringBackHold),
    )
    spring = core.SpringBackHoldStopParametersDTO(
        stop_mode=core.MotionControlledStopMode.kSpringBackHold,
        deceleration_time_s=0.2,
        per_resource_limits=[controlled_limit],
    )
    assert spring.allow_natural_completion_on_short_path is False
    assert (
        core.SpringBackHoldStopParametersDTO(
            stop_mode=core.MotionControlledStopMode.kSpringBackHold,
            deceleration_time_s=0.2,
            per_resource_limits=[controlled_limit],
            allow_natural_completion_on_short_path=True,
        ).allow_natural_completion_on_short_path
        is True
    )
    hard = core.HardDecelerationStopParametersDTO(
        stop_mode=core.MotionControlledStopMode.kHardDeceleration,
        per_resource_limits=[hard_limit],
    )

    assert typing.get_args(core.MotionControlledStopContractParametersDTO) == (
        core.SoftOnPathStopParametersDTO,
        core.SpringBackHoldStopParametersDTO,
        core.HardDecelerationStopParametersDTO,
    )
    assert soft.per_resource_limits == (controlled_limit,)
    assert spring.per_resource_limits == (controlled_limit,)
    assert hard.per_resource_limits == (hard_limit,)
    assert (
        soft.recoverable_substitution_mode
        == core.MotionControlledStopMode.kSpringBackHold
    )

    definitions = core.registered_stop_command_definitions()
    assert isinstance(definitions, tuple)
    assert [row.registered_command_kind_id.value for row in definitions] == [
        "manyforge.stop.controlled.provider",
        "manyforge.stop.safety.emergency_request",
        "manyforge.stop.safety.protective_request",
        "manyforge.stop.software_hard",
    ]
    assert definitions[-1].stop_action_class == core.StopActionClass.kSoftwareHard
    with pytest.raises(AttributeError):
        definitions[-1].stop_action_class = core.StopActionClass.kControlled

    with pytest.raises(TypeError):
        core.ControlledStopSubstitutionContractDTO()


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
    assert typed.structurally_normalized_canonical_json == parsed.canonical_json

    validated = core.validate_intrinsic_cell_graph_candidate(typed)
    assert isinstance(validated, core.IntrinsicallyValidatedCellGraphCandidate)
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
    assert isinstance(artifact, core.UnqualifiedSchemaNormalizedArtifactCandidate)
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
    assert len(registry.catalog_schemas) == 10
    qualified_entries = [
        entry
        for entry in registry.catalog_schemas
        if entry.qualification == core.CatalogSchemaQualification.kQualified
    ]
    assert len(qualified_entries) == 10
    assert {entry.record_kind for entry in qualified_entries} == {
        core.CatalogRecordKind.kCapabilityDefinition,
        core.CatalogRecordKind.kCommandSchema,
        core.CatalogRecordKind.kComponentType,
        core.CatalogRecordKind.kContactPolicy,
        core.CatalogRecordKind.kControlInterfaceContract,
        core.CatalogRecordKind.kControllerTopology,
        core.CatalogRecordKind.kModelAsset,
        core.CatalogRecordKind.kProviderManifest,
        core.CatalogRecordKind.kRuntimeProfile,
        core.CatalogRecordKind.kStopEvidencePolicy,
    }
    assert all(entry.compiled_schema is not None for entry in qualified_entries)
    assert {entry.relative_path for entry in qualified_entries} == {
        "catalog/manyforge.capability_definition.v1.yaml",
        "catalog/manyforge.command_schema.v1.yaml",
        "catalog/manyforge.component_type.v1.yaml",
        "catalog/manyforge.contact_policy.v1.yaml",
        "catalog/manyforge.control_interface_contract.v1.yaml",
        "catalog/manyforge.controller_topology.v1.yaml",
        "catalog/manyforge.model_asset.v1.yaml",
        "catalog/manyforge.provider_manifest.v1.yaml",
        "catalog/manyforge.runtime_profile.v1.yaml",
        "catalog/manyforge.stop_evidence_policy.v2.yaml",
    }
    assert all(
        entry.compiled_schema is None and entry.relative_path == ""
        for entry in registry.catalog_schemas
        if entry.qualification == core.CatalogSchemaQualification.kUnqualified
    )
    assert len(registry.value_schemas) == 5
    assert {
        (entry.schema_id.value, entry.schema_revision.value)
        for entry in registry.value_schemas
    } == {
        (
            "manyforge.value_schema.provider_contract.empty.v1",
            "sha256:fb466dd341076cbdb6fc97558ad28bc46032d25daaf4d4bb9d548bf642b83270",
        ),
        (
            "manyforge.value_schema.provider_contract.motion_controlled_stop.v1",
            "sha256:58ec6382a268329e9d8001eb53aac31259b5eb95a3690095aa4bbe12ac51d83b",
        ),
        (
            "manyforge.value_schema.runtime.empty.v1",
            "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785",
        ),
        (
            "manyforge.value_schema.runtime.native_execution.v1",
            "sha256:6b540dac39434e05a2cb5da209206e2105b824b9f56704a74f0d88f327944689",
        ),
        (
            "manyforge.value_schema.runtime.ros2_control_execution.v1",
            "sha256:9d37332a6f3c943e2f1811b5009c329d24248665f4ec14924a5ca437510c6905",
        ),
    }
    assert (
        registry.qualification_registry_digest.value
        == "sha256:"
        + hashlib.sha256(
            registry.normalized_projection_canonical_json.encode("utf-8")
        ).hexdigest()
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


def test_graph_revision_trust_surface_is_staged_narrow_and_immutable():
    intrinsic = core.validate_intrinsic_cell_graph_candidate(
        core.decode_cell_graph_candidate(
            core.parse_strict_json_model_yaml(EMPTY_GRAPH_YAML)
        )
    )
    ready = core.prepare_cell_graph_identity(intrinsic, [])
    assert isinstance(ready, core.GraphIdentityReadyCellGraphCandidate)
    assert json.loads(ready.normalized_canonical_json)["graph_revision"] == ZERO_DIGEST

    with pytest.raises(core.CellGraphCodecError, match="graph_revision"):
        core.verify_graph_revision(ready)

    computed = core.compute_graph_revision(ready)
    assert isinstance(computed, core.GraphRevisionVerifiedCellGraph)
    assert computed.graph.graph_revision == core.GraphRevision(
        "sha256:443285fa98d7408c9249b581a8802f498bbe1501486bd579cc674be7a6b2cb34"
    )

    verification_ready = core.prepare_cell_graph_identity(
        core.validate_intrinsic_cell_graph_candidate(
            core.decode_cell_graph_candidate(
                core.parse_strict_json_model_yaml(computed.normalized_canonical_json)
            )
        ),
        [],
    )
    verified = core.verify_graph_revision(verification_ready)
    assert verified.graph == computed.graph
    assert verified.normalized_canonical_json == computed.normalized_canonical_json

    for value in (ready, computed, verified):
        with pytest.raises(AttributeError):
            value.graph = value.graph
        with pytest.raises(AttributeError):
            value.extra = "not allowed"

    for value_type in (
        core.GraphIdentityReadyCellGraphCandidate,
        core.GraphRevisionVerifiedCellGraph,
    ):
        with pytest.raises(TypeError):
            value_type()

    with pytest.raises(TypeError):
        core.compute_graph_revision(intrinsic.graph)

    for forbidden_name in ("TrustedCellGraph", "TrustedCellGraphCandidate"):
        assert not hasattr(core, forbidden_name)


def test_assistant_compiler_projection_is_exact_read_only_and_bounded():
    assistant = {
        "audit_policy": {
            "redact_argument_values": True,
            "store_full_prompts": False,
            "store_full_responses": False,
        },
        "model_endpoint_aliases": {
            "primary_model": {
                "external_resource_alias": "assistant_model",
                "model_id": "i5-python-test-model",
                "request_contract": {
                    "default_timeout_s": 30.0,
                    "max_output_tokens": 512,
                    "supported_image_transports": [
                        "http_url",
                        "inline_data_url",
                    ],
                    "supports_image_input": True,
                    "supports_json_object": True,
                    "supports_response_tools": True,
                    "tool_call_family": "openai_chat_tools",
                },
            }
        },
        "modes": {
            "query": {
                "catalog": {
                    "nodes": [],
                    "skills": [],
                    "tools": ["scene_inspect", "status_read"],
                },
                "description": "Read-only H0 query",
                "enabled": True,
                "grant_policy": {
                    "approval_mode": None,
                    "review_policy": "not_required",
                    "session_kind": None,
                },
                "model_endpoint_alias": "primary_model",
                "provider_alias": "direct_local",
            }
        },
        "prompt_templates": {},
        "provider_aliases": {
            "direct_local": {
                "bridge_external_resource_alias": "assistant_bridge",
                "provider_kind": "direct",
            }
        },
    }
    external_resources = [
        {
            "endpoint_role_id": "state",
            "external_resource_alias": "assistant_bridge",
            "provider_instance_id": "assistant_provider_instance",
            "provider_manifest_id": "assistant_provider_manifest",
            "provider_manifest_revision": ZERO_DIGEST,
            "resource_kind": "service_endpoint",
            "site_overlay_value_ref": "resources.assistant_bridge",
        },
        {
            "endpoint_role_id": "state",
            "external_resource_alias": "assistant_model",
            "provider_instance_id": "assistant_provider_instance",
            "provider_manifest_id": "assistant_provider_manifest",
            "provider_manifest_revision": ZERO_DIGEST,
            "resource_kind": "model_endpoint",
            "site_overlay_value_ref": "resources.assistant_model",
        },
    ]
    document = {
        "api_version": "manyforge.deployment.v1",
        "assistant": assistant,
        "desired_control_configurations": [],
        "capability_provider_bindings": [],
        "external_resources": external_resources,
    }
    candidate = core.decode_deployment_applicability_inputs_candidate(
        core.parse_strict_json_model_yaml(
            json.dumps(document, sort_keys=True, separators=(",", ":"))
        )
    )

    assistant_candidate = candidate.assistant_settings
    assert assistant_candidate.source_was_present
    assert assistant_candidate.enabled
    assert json.loads(assistant_candidate.normalized_source_canonical_json) == assistant

    qualified = core.qualify_assistant_settings(
        assistant_candidate, candidate.value.external_resources
    )
    projection = json.loads(qualified.normalized_projection_canonical_json)
    registry = core.build_qualified_assistant_instrument_registry()
    registry_projection = json.loads(registry.normalized_projection_canonical_json)

    assert qualified.enabled
    assert qualified.projection_digest.value == projection["projection_digest"]
    assert qualified.instrument_registry_digest == registry.registry_digest
    assert projection["instruments"] == registry_projection
    assert projection["model_endpoint_aliases"][0]["request_contract"] == {
        "default_timeout_s": 30,
        "max_output_tokens": 512,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "supports_image_input": True,
        "supports_json_object": True,
        "supports_response_tools": True,
        "tool_call_family": "openai_chat_tools",
    }
    assert [mode["mode_id"] for mode in projection["modes"]] == ["query"]
    assert projection["modes"][0]["catalog"]["tools"] == [
        "scene_inspect",
        "status_read",
    ]
    assert [tool["tool_id"] for tool in projection["instruments"]["tools"]] == [
        "alarms_read",
        "blackboard_draft_remove_keys",
        "blackboard_draft_remove_namespaces",
        "blackboard_draft_upsert_keys",
        "blackboard_draft_upsert_namespaces",
        "catalog_read",
        "deployment_capabilities_read",
        "history_read",
        "inspect_isaac_scene",
        "program_draft_remove_parameters",
        "program_draft_upsert_parameters",
        "program_read",
        "program_validate",
        "propose_scene_object_nodes",
        "propose_scene_objects",
        "scene_draft_add_object",
        "scene_draft_remove_objects",
        "scene_draft_update_object",
        "scene_draft_upsert_objects",
        "scene_inspect",
        "skills_read",
        "status_read",
        "tree_draft_change_node_kind",
        "tree_draft_delete_node",
        "tree_draft_insert_node",
        "tree_draft_move_node",
        "tree_draft_rename_node",
        "tree_draft_replace_subtree",
        "tree_draft_update_node_params",
        "tree_draft_wrap_node",
    ]

    for value, field in (
        (assistant_candidate, "enabled"),
        (qualified, "projection_digest"),
        (registry, "registry_digest"),
    ):
        with pytest.raises(AttributeError):
            setattr(value, field, getattr(value, field))
        with pytest.raises(AttributeError):
            value.extra = "not allowed"

    for value_type in (
        core.TypedAssistantSettingsCandidate,
        core.QualifiedAssistantSettings,
        core.BuildQualifiedAssistantInstrumentRegistry,
    ):
        with pytest.raises(TypeError):
            value_type()

    wrong_resources = list(candidate.value.external_resources)
    wrong_resources[0] = core.ExternalResourceDTO(
        external_resource_alias=core.ExternalResourceAlias("assistant_bridge"),
        resource_kind=core.ExternalResourceKind.kModelEndpoint,
        provider_instance_id=core.ProviderInstanceId("assistant_provider_instance"),
        provider_manifest_id=core.ProviderManifestId("assistant_provider_manifest"),
        provider_manifest_revision=core.ProviderManifestRevision(ZERO_DIGEST),
        endpoint_role_id=core.EndpointRoleId("state"),
        site_overlay_value_ref=core.SiteOverlayValueRef("resources.assistant_bridge"),
    )
    with pytest.raises(
        core.AssistantSettingsCodecError,
        match="bridge external resource is not a service_endpoint",
    ):
        core.qualify_assistant_settings(assistant_candidate, wrong_resources)
