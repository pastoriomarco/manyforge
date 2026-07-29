from __future__ import annotations

from dataclasses import fields
import hashlib
import json
from pathlib import Path

import pytest

from verification.composable_cell_i1a.contracts import (
    SCHEMA_ID,
    ActiveRouteSnapshot,
    AlarmId,
    AlarmOccurrenceId,
    AlarmRevision,
    AlarmSeverity,
    AlarmSnapshot,
    AlarmState,
    BindingRevision,
    CatalogRevision,
    CellStateRevision,
    CellStateSnapshot,
    CellGraph,
    CapabilityId,
    ClientDisposition,
    CommandTargetId,
    ComponentStateRevision,
    ComponentStateSnapshot,
    CompiledBindingSnapshot,
    CompiledSymbolMap,
    CompilerRevision,
    ControllableComponentId,
    ControllerId,
    ControllerSpec,
    ControllerTopologyId,
    ControllerTopologyRecord,
    ControllerTopologyRevision,
    ContractError,
    ControlBindingId,
    ControlDomainId,
    ControlDomainStateRevision,
    ControlDomainStateSnapshot,
    ControlRouteSpec,
    ControlRequest,
    DomainRequestPrecondition,
    GraphRevision,
    InterfaceContractRevision,
    LifecycleIntent,
    LifecyclePhase,
    LifecycleReadiness,
    LifecycleRequest,
    LifecycleStatus,
    LifecycleTransactionId,
    ModelInstanceId,
    OperationId,
    PublicClientRequest,
    PublicClientStatus,
    RequestId,
    RequestKind,
    RequestMetadata,
    RequestPreconditions,
    RequestScope,
    ResourceId,
    RetainedArtifact,
    RetainedBundleManifest,
    RouteId,
    RouteGeneration,
    RouteSelectionPhase,
    RouteSelectionPreconditions,
    RouteSelectionRequest,
    RouteSelectionStatus,
    RouteSelectionTransactionId,
    RuntimeEpoch,
    RuntimeUnitId,
    RuntimeUnitObservation,
    SceneRevision,
    Sha256Digest,
    SymbolBinding,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE = (
    REPO_ROOT
    / "verification"
    / "composable_cell_i1a"
    / "fixtures"
    / "two_domain_cell.experimental.json"
)
FORBIDDEN_CALLER_POLICY_TERMS = {
    "authority",
    "lease",
    "permission",
    "principal",
    "session",
}


def _digest(label: str) -> str:
    return hashlib.sha256(label.encode("utf-8")).hexdigest()


def _compiled_binding_snapshot(
    *,
    resources: tuple[ResourceId, ...] = (ResourceId("a.joint1"),),
    endpoints: tuple[str, ...] = ("/a/controller",),
) -> CompiledBindingSnapshot:
    return CompiledBindingSnapshot(
        control_binding_id=ControlBindingId("binding-a"),
        binding_revision=BindingRevision(4),
        controllable_component_id=ControllableComponentId("component-a"),
        control_domain_id=ControlDomainId("domain-a"),
        command_target_id=CommandTargetId("target-arm"),
        capability_id=CapabilityId("capability-trajectory"),
        route_id=RouteId("route-direct"),
        interface_contract_revision=InterfaceContractRevision(2),
        controller_topology_id=ControllerTopologyId("topology-a"),
        topology_revision=ControllerTopologyRevision(3),
        ordered_resources=resources,
        endpoints=endpoints,
    )


def test_two_domain_graph_round_trip_is_strict_and_deterministic() -> None:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    graph = CellGraph.from_dict(raw)
    repeated = CellGraph.from_json(graph.to_json())

    assert repeated == graph
    assert graph.schema == SCHEMA_ID
    assert graph.sha256() == repeated.sha256()
    assert len(graph.runtime_units) == 2
    assert len(graph.controllable_components) == 2
    assert len(graph.model_instances) == 2
    assert len(graph.control_domains) == 2
    assert len(graph.control_bindings) == 2
    assert graph.attachment_edges == ()

    first, second = graph.controllable_components
    assert first.component_type_id == second.component_type_id
    assert first.controllable_component_id != second.controllable_component_id
    assert first.model_instance_id != second.model_instance_id
    assert set(first.control_binding_ids).isdisjoint(second.control_binding_ids)


def test_unknown_or_missing_graph_fields_are_rejected() -> None:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["authority_lease"] = "forbidden"
    with pytest.raises(ContractError, match="unknown=.*authority_lease"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    del raw["control_domains"]
    with pytest.raises(ContractError, match="missing=.*control_domains"):
        CellGraph.from_dict(raw)


def test_duplicate_and_dangling_graph_references_are_rejected() -> None:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["runtime_units"].append(dict(raw["runtime_units"][0]))
    with pytest.raises(ContractError, match="runtime units must be unique"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["control_bindings"][0]["control_domain_id"] = "domain.absent"
    with pytest.raises(ContractError, match="unknown control domain"):
        CellGraph.from_dict(raw)


def test_binding_revisions_topology_and_endpoints_are_exact_in_graph() -> None:
    graph = CellGraph.from_json(FIXTURE.read_text(encoding="utf-8"))
    binding = graph.control_bindings[0]

    assert binding.route_id == RouteId("route.joint_trajectory")
    assert binding.interface_contract_revision == InterfaceContractRevision(1)
    assert binding.controller_topology_id == ControllerTopologyId(
        "topology.lite6.a"
    )
    assert binding.topology_revision == ControllerTopologyRevision(1)
    assert binding.endpoints == ("/a/joint_trajectory",)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["control_bindings"][0]["topology_revision"] = 2
    with pytest.raises(ContractError, match="topology revision"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["control_bindings"][0]["endpoints"] = []
    with pytest.raises(ContractError, match="requires endpoints"):
        CellGraph.from_dict(raw)


def test_runtime_dependency_cycles_and_nonreciprocal_bindings_are_rejected() -> None:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["runtime_units"][0]["dependencies"] = ["runtime.control.b"]
    raw["runtime_units"][1]["dependencies"] = ["runtime.control.a"]
    with pytest.raises(ContractError, match="dependency graph contains a cycle"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["controllable_components"][0]["control_binding_ids"] = []
    with pytest.raises(ContractError, match="not reciprocal"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["controllable_components"][0]["control_binding_ids"] = [
        "binding.lite6.b.arm"
    ]
    with pytest.raises(ContractError, match="not reciprocal"):
        CellGraph.from_dict(raw)


def test_attachment_multiple_parent_edges_and_cycles_are_rejected() -> None:
    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["attachment_edges"] = [
        {
            "attachment_id": "attachment.first",
            "attachment_revision": 1,
            "parent_model_instance_id": "model.lite6.a",
            "child_model_instance_id": "model.lite6.b",
            "parent_frame": "a_tool",
            "child_frame": "b_base",
        },
        {
            "attachment_id": "attachment.second",
            "attachment_revision": 1,
            "parent_model_instance_id": "model.lite6.a",
            "child_model_instance_id": "model.lite6.b",
            "parent_frame": "a_tool",
            "child_frame": "b_base",
        },
    ]
    with pytest.raises(ContractError, match="exactly one parent edge"):
        CellGraph.from_dict(raw)

    raw = json.loads(FIXTURE.read_text(encoding="utf-8"))
    raw["attachment_edges"] = [
        {
            "attachment_id": "attachment.a-to-b",
            "attachment_revision": 1,
            "parent_model_instance_id": "model.lite6.a",
            "child_model_instance_id": "model.lite6.b",
            "parent_frame": "a_tool",
            "child_frame": "b_base",
        },
        {
            "attachment_id": "attachment.b-to-a",
            "attachment_revision": 1,
            "parent_model_instance_id": "model.lite6.b",
            "child_model_instance_id": "model.lite6.a",
            "parent_frame": "b_tool",
            "child_frame": "a_base",
        },
    ]
    with pytest.raises(ContractError, match="attachment graph contains a cycle"):
        CellGraph.from_dict(raw)


def _controller_topology(
    *,
    upstream_inputs: tuple[ControllerId, ...] = (),
    downstream_inputs: tuple[ControllerId, ...] = (ControllerId("admittance"),),
    upstream_resource: str = "reference.pose",
    downstream_resource: str = "joint.1",
    route_order: tuple[ControllerId, ...] = (
        ControllerId("admittance"),
        ControllerId("jtc"),
    ),
) -> ControllerTopologyRecord:
    return ControllerTopologyRecord(
        controller_topology_id=ControllerTopologyId("topology.arm"),
        topology_revision=ControllerTopologyRevision(1),
        controllers=(
            ControllerSpec(
                controller_id=ControllerId("admittance"),
                plugin="example/Admittance",
                claimed_resources=(ResourceId(upstream_resource),),
                chain_inputs=upstream_inputs,
            ),
            ControllerSpec(
                controller_id=ControllerId("jtc"),
                plugin="example/JTC",
                claimed_resources=(ResourceId(downstream_resource),),
                chain_inputs=downstream_inputs,
            ),
        ),
        routes=(
            ControlRouteSpec(
                route_id=RouteId("route.admittance"),
                capability_id=CapabilityId("capability.compliant_trajectory"),
                controllers=route_order,
                chained=True,
            ),
        ),
        activation_sets=((ControllerId("admittance"), ControllerId("jtc")),),
    )


def test_controller_chain_direction_and_legal_activation_set_are_explicit() -> None:
    topology = _controller_topology()
    assert topology.routes[0].controllers == (
        ControllerId("admittance"),
        ControllerId("jtc"),
    )

    with pytest.raises(ContractError, match="direction is invalid"):
        _controller_topology(
            route_order=(ControllerId("jtc"), ControllerId("admittance"))
        )

    with pytest.raises(ContractError, match="controller chain contains a cycle"):
        _controller_topology(
            upstream_inputs=(ControllerId("jtc"),),
        )


def test_activation_sets_reject_duplicate_controllers_and_resource_conflicts() -> None:
    with pytest.raises(ContractError, match="resource-claim conflict"):
        _controller_topology(
            upstream_resource="joint.1",
            downstream_resource="joint.1",
        )

    with pytest.raises(ContractError, match="activation-set controllers"):
        ControllerTopologyRecord(
            controller_topology_id=ControllerTopologyId("topology.duplicate"),
            topology_revision=ControllerTopologyRevision(1),
            controllers=(
                ControllerSpec(
                    controller_id=ControllerId("jtc"),
                    plugin="example/JTC",
                    claimed_resources=(ResourceId("joint.1"),),
                ),
            ),
            routes=(
                ControlRouteSpec(
                    route_id=RouteId("route.direct"),
                    capability_id=CapabilityId("capability.trajectory"),
                    controllers=(ControllerId("jtc"),),
                    chained=False,
                ),
            ),
            activation_sets=((ControllerId("jtc"), ControllerId("jtc")),),
        )

    with pytest.raises(ContractError, match="no legal activation set"):
        ControllerTopologyRecord(
            controller_topology_id=ControllerTopologyId("topology.unreachable"),
            topology_revision=ControllerTopologyRevision(1),
            controllers=(
                ControllerSpec(
                    controller_id=ControllerId("jtc"),
                    plugin="example/JTC",
                    claimed_resources=(ResourceId("joint.1"),),
                ),
                ControllerSpec(
                    controller_id=ControllerId("idle"),
                    plugin="example/Idle",
                    claimed_resources=(ResourceId("joint.2"),),
                ),
            ),
            routes=(
                ControlRouteSpec(
                    route_id=RouteId("route.direct"),
                    capability_id=CapabilityId("capability.trajectory"),
                    controllers=(ControllerId("jtc"),),
                    chained=False,
                ),
            ),
            activation_sets=((ControllerId("idle"),),),
        )


@pytest.mark.parametrize(
    "factory",
    [
        lambda: RuntimeUnitId(""),
        lambda: ControlDomainId(" edge-space "),
        lambda: GraphRevision(-1),
        lambda: Sha256Digest("not-a-digest"),
    ],
)
def test_typed_values_reject_invalid_input(factory) -> None:
    with pytest.raises(ContractError):
        factory()


def test_identity_and_revision_axes_remain_distinct() -> None:
    assert RuntimeUnitId("same") != ControlDomainId("same")
    assert GraphRevision(7) != BindingRevision(7)
    assert int(GraphRevision(7)) == 7
    with pytest.raises(ContractError, match="non-negative integer"):
        GraphRevision("graph-seven")


def test_request_metadata_is_attribution_only() -> None:
    metadata_fields = {field.name for field in fields(RequestMetadata)}
    assert metadata_fields == {
        "request_id",
        "source_label",
        "transport",
        "correlation_id",
        "submitted_monotonic_ns",
    }
    assert metadata_fields.isdisjoint(FORBIDDEN_CALLER_POLICY_TERMS)

    request = ControlRequest(
        metadata=RequestMetadata(
            request_id=RequestId("request-1"),
            source_label="composer",
            transport="http",
            correlation_id="trace-1",
            submitted_monotonic_ns=42,
        ),
        kind=RequestKind.START,
        scope=RequestScope(
            control_domain_ids=(ControlDomainId("domain-a"),),
            resource_ids=(ResourceId("a.joint1"),),
        ),
        preconditions=RequestPreconditions(
            expected_graph_revision=GraphRevision(3),
            domains=(
                DomainRequestPrecondition(
                    control_domain_id=ControlDomainId("domain-a"),
                    expected_binding_revision=BindingRevision(8),
                ),
            ),
        ),
        payload_sha256=_digest("start"),
    )
    encoded = request.to_json().lower()

    assert all(term not in encoded for term in FORBIDDEN_CALLER_POLICY_TERMS)
    assert ControlRequest.from_json(encoded) == request


def test_request_scope_is_typed_nonempty_and_domain_preconditions_are_local() -> None:
    with pytest.raises(ContractError, match="cannot be empty"):
        RequestScope()
    with pytest.raises(ContractError, match="ResourceId"):
        RequestScope(resource_ids=("joint.1",))  # type: ignore[arg-type]

    with pytest.raises(ContractError, match="outside request scope"):
        ControlRequest(
            metadata=RequestMetadata(
                request_id=RequestId("request-scope"),
                source_label="test",
                transport="in_process",
            ),
            kind=RequestKind.START,
            scope=RequestScope(
                control_domain_ids=(ControlDomainId("domain-a"),),
            ),
            preconditions=RequestPreconditions(
                domains=(
                    DomainRequestPrecondition(
                        control_domain_id=ControlDomainId("domain-b"),
                    ),
                ),
            ),
            payload_sha256=_digest("scope"),
        )


@pytest.mark.parametrize("intent", tuple(LifecycleIntent))
def test_typed_lifecycle_requests_cover_all_generic_intents(
    intent: LifecycleIntent,
) -> None:
    request = LifecycleRequest(
        metadata=RequestMetadata(
            request_id=RequestId(f"request-{intent.value}"),
            source_label="composer",
            transport="http",
        ),
        lifecycle_transaction_id=LifecycleTransactionId(
            f"lifecycle-{intent.value}"
        ),
        controllable_component_id=ControllableComponentId("component-a"),
        intent=intent,
        expected_graph_revision=GraphRevision(4),
        expected_component_state_revision=ComponentStateRevision(7),
    )
    status = LifecycleStatus(
        lifecycle_transaction_id=request.lifecycle_transaction_id,
        controllable_component_id=request.controllable_component_id,
        intent=intent,
        phase=LifecyclePhase.SUCCEEDED,
        component_state_revision=ComponentStateRevision(8),
        readiness=(
            LifecycleReadiness.DISABLED
            if intent is LifecycleIntent.DISABLE
            else LifecycleReadiness.READY
        ),
    )
    kind = RequestKind(intent.value)

    assert LifecycleRequest.from_json(request.to_json()) == request
    assert LifecycleStatus.from_json(status.to_json()) == status
    assert PublicClientRequest.from_json(
        PublicClientRequest(
            schema=SCHEMA_ID,
            kind=kind,
            payload=request,
        ).to_json()
    ).payload == request
    assert PublicClientStatus.from_json(
        PublicClientStatus(
            schema=SCHEMA_ID,
            request_id=request.metadata.request_id,
            kind=kind,
            disposition=ClientDisposition.SUCCEEDED,
            cell_state_revision=CellStateRevision(12),
            payload=status,
        ).to_json()
    ).payload == status


def test_route_selection_is_exact_and_uses_domain_local_preconditions() -> None:
    request = RouteSelectionRequest(
        metadata=RequestMetadata(
            request_id=RequestId("request-route"),
            source_label="plc",
            transport="ros",
        ),
        route_selection_id=RouteSelectionTransactionId("route-switch-1"),
        controllable_component_id=ControllableComponentId("component-a"),
        command_target_id=CommandTargetId("target-arm"),
        capability_id=CapabilityId("capability-trajectory"),
        control_domain_id=ControlDomainId("domain-a"),
        control_binding_id=ControlBindingId("binding-a"),
        route_id=RouteId("route-admittance"),
        preconditions=RouteSelectionPreconditions(
            expected_control_domain_state_revision=ControlDomainStateRevision(5),
            expected_interface_contract_revision=InterfaceContractRevision(2),
            expected_topology_revision=ControllerTopologyRevision(3),
            expected_route_generation=RouteGeneration(9),
            expected_binding_revision=BindingRevision(4),
        ),
    )
    status = RouteSelectionStatus(
        route_selection_id=request.route_selection_id,
        controllable_component_id=request.controllable_component_id,
        command_target_id=request.command_target_id,
        capability_id=request.capability_id,
        control_domain_id=request.control_domain_id,
        control_binding_id=request.control_binding_id,
        route_id=request.route_id,
        phase=RouteSelectionPhase.SELECTED,
        control_domain_state_revision=ControlDomainStateRevision(6),
        interface_contract_revision=InterfaceContractRevision(2),
        topology_revision=ControllerTopologyRevision(3),
        route_generation=RouteGeneration(10),
        binding_revision=BindingRevision(5),
    )

    assert RouteSelectionRequest.from_json(request.to_json()) == request
    assert RouteSelectionStatus.from_json(status.to_json()) == status
    assert PublicClientRequest.from_json(
        PublicClientRequest(
            schema=SCHEMA_ID,
            kind=RequestKind.SELECT_ROUTE,
            payload=request,
        ).to_json()
    ).payload == request


def test_typed_cell_state_and_four_alarm_states_round_trip() -> None:
    route = ActiveRouteSnapshot(
        control_binding_id=ControlBindingId("binding-a"),
        binding_revision=BindingRevision(4),
        command_target_id=CommandTargetId("target-arm"),
        capability_id=CapabilityId("capability-trajectory"),
        route_id=RouteId("route-direct"),
        route_generation=RouteGeneration(3),
    )
    alarms = tuple(
        AlarmSnapshot(
            alarm_id=AlarmId(f"alarm-{index}"),
            occurrence_id=AlarmOccurrenceId(f"occurrence-{index}"),
            alarm_revision=AlarmRevision(index),
            state=state,
            severity=AlarmSeverity.ERROR,
            condition_code=f"condition-{index}",
            subject_kind="control_domain",
            subject_id="domain-a",
            message=f"alarm state {state.value}",
        )
        for index, state in enumerate(AlarmState, start=1)
    )
    snapshot = CellStateSnapshot(
        schema=SCHEMA_ID,
        cell_state_revision=CellStateRevision(11),
        graph_revision=GraphRevision(4),
        scene_revision=SceneRevision("scene-8"),
        observed_monotonic_ns=99,
        runtime_units=(
            RuntimeUnitObservation(
                runtime_unit_id=RuntimeUnitId("runtime-a"),
                runtime_epoch=RuntimeEpoch("epoch-2"),
                health="healthy",
                observed_monotonic_ns=99,
            ),
        ),
        components=(
            ComponentStateSnapshot(
                controllable_component_id=ControllableComponentId("component-a"),
                component_state_revision=ComponentStateRevision(7),
                readiness=LifecycleReadiness.READY,
                active_bindings=(route,),
            ),
        ),
        control_domains=(
            ControlDomainStateSnapshot(
                control_domain_id=ControlDomainId("domain-a"),
                control_domain_state_revision=ControlDomainStateRevision(5),
                runtime_unit_id=RuntimeUnitId("runtime-a"),
                runtime_epoch=RuntimeEpoch("epoch-2"),
                health="healthy",
                interface_contract_revision=InterfaceContractRevision(2),
                controller_topology_id=ControllerTopologyId("topology-a"),
                topology_revision=ControllerTopologyRevision(3),
                active_routes=(route,),
            ),
        ),
        active_operations=(OperationId("operation-a"),),
        alarms=alarms,
    )

    assert {item.state for item in alarms} == set(AlarmState)
    assert CellStateSnapshot.from_json(snapshot.to_json()) == snapshot


def test_public_client_envelopes_reject_mismatched_typed_payloads() -> None:
    lifecycle = LifecycleRequest(
        metadata=RequestMetadata(
            request_id=RequestId("request-enable"),
            source_label="cli",
            transport="http",
        ),
        lifecycle_transaction_id=LifecycleTransactionId("lifecycle-enable"),
        controllable_component_id=ControllableComponentId("component-a"),
        intent=LifecycleIntent.ENABLE,
        expected_graph_revision=GraphRevision(1),
        expected_component_state_revision=ComponentStateRevision(1),
    )

    with pytest.raises(ContractError, match="does not match"):
        PublicClientRequest(
            schema=SCHEMA_ID,
            kind=RequestKind.DISABLE,
            payload=lifecycle,
        )


def test_retained_bundle_manifest_is_deterministic_and_binding_exact() -> None:
    manifest = RetainedBundleManifest(
        schema=SCHEMA_ID,
        compiler_revision=CompilerRevision("compiler-1"),
        catalog_revision=CatalogRevision("catalog-2"),
        graph_revision=GraphRevision(3),
        artifacts=(),
        symbol_maps=(
            CompiledSymbolMap(
                model_instance_id=ModelInstanceId("model-a"),
                symbols=(
                    SymbolBinding(
                        logical_name="joint1",
                        runtime_name="a_joint1",
                        kind="joint",
                    ),
                ),
            ),
        ),
        bindings=(
            _compiled_binding_snapshot(
                resources=(
                    ResourceId("a.joint1"),
                    ResourceId("a.joint2"),
                ),
                endpoints=(
                    "/a/joint_trajectory_controller/follow_joint_trajectory",
                ),
            ),
        ),
    )

    assert RetainedBundleManifest.from_json(manifest.to_json()) == manifest
    assert manifest.sha256() == _digest(manifest.to_json())


def test_duplicate_compiled_symbol_or_binding_identity_is_rejected() -> None:
    duplicate_symbol = SymbolBinding(
        logical_name="joint1",
        runtime_name="a_joint1",
        kind="joint",
    )
    with pytest.raises(ContractError, match="logical symbols must be unique"):
        CompiledSymbolMap(
            model_instance_id=ModelInstanceId("model-a"),
            symbols=(duplicate_symbol, duplicate_symbol),
        )

    snapshot = _compiled_binding_snapshot()
    with pytest.raises(ContractError, match="compiled bindings must be unique"):
        RetainedBundleManifest(
            schema=SCHEMA_ID,
            compiler_revision=CompilerRevision(1),
            catalog_revision=CatalogRevision(1),
            graph_revision=GraphRevision(1),
            artifacts=(),
            symbol_maps=(),
            bindings=(snapshot, snapshot),
        )


@pytest.mark.parametrize(
    "path",
    [
        "/absolute/bundle.json",
        "../escape.json",
        "manifests/../escape.json",
        "manifests//bundle.json",
        r"manifests\\bundle.json",
    ],
)
def test_retained_artifact_paths_are_safe_normalized_relative_paths(
    path: str,
) -> None:
    with pytest.raises(ContractError, match="artifact path"):
        RetainedArtifact(
            name="bundle",
            kind="compiled_bundle",
            relative_path=path,
            content_sha256=Sha256Digest(_digest("bundle")),
        )


@pytest.mark.parametrize(
    "resources,endpoints,error",
    [
        ((), ("/controller",), "requires ordered resources"),
        (
            (ResourceId("joint.1"), ResourceId("joint.1")),
            ("/controller",),
            "resources must be unique",
        ),
        ((ResourceId("joint.1"),), (), "requires endpoints"),
        ((ResourceId("joint.1"),), ("",), "endpoint"),
    ],
)
def test_compiled_binding_snapshot_requires_exact_resources_and_endpoint(
    resources: tuple[ResourceId, ...],
    endpoints: tuple[str, ...],
    error: str,
) -> None:
    with pytest.raises(ContractError, match=error):
        _compiled_binding_snapshot(resources=resources, endpoints=endpoints)


def test_retained_manifest_rejects_global_runtime_symbol_collisions() -> None:
    colliding = SymbolBinding(
        logical_name="joint",
        runtime_name="cell_joint",
        kind="joint",
    )
    with pytest.raises(ContractError, match="global runtime symbols"):
        RetainedBundleManifest(
            schema=SCHEMA_ID,
            compiler_revision=CompilerRevision("compiler-1"),
            catalog_revision=CatalogRevision("catalog-1"),
            graph_revision=GraphRevision(1),
            artifacts=(),
            symbol_maps=(
                CompiledSymbolMap(
                    model_instance_id=ModelInstanceId("model.a"),
                    symbols=(colliding,),
                ),
                CompiledSymbolMap(
                    model_instance_id=ModelInstanceId("model.b"),
                    symbols=(colliding,),
                ),
            ),
            bindings=(),
        )


def test_command_target_is_not_confused_with_component_identity() -> None:
    assert CommandTargetId("arm") != ControlBindingId("arm")
