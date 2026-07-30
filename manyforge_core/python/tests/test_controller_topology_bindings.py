"""Python parity tests for the immutable controller-topology boundary."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")

ZERO_DIGEST = "sha256:" + ("0" * 64)


def _interface(resource_role_id: str, interface_name: str):
    return core.ResourceInterfaceClaimDTO(
        resource_role_id=core.ResourceRoleId(resource_role_id),
        interface_name=interface_name,
    )


def _contract_claim(
    contract_id: str,
    contract_revision: str,
    directions=None,
):
    if directions is None:
        directions = [core.ControllerClaimDirection.kCommand]
    return core.ControllerControlInterfaceClaimDTO(
        control_interface_contract_id=core.ControlInterfaceContractId(
            contract_id
        ),
        control_interface_revision=core.ControlInterfaceRevision(
            contract_revision
        ),
        claimed_directions=directions,
    )


def _controller(
    role_id: str,
    contract_id: str,
    contract_revision: str,
    *,
    directions=None,
    exported_reference_interfaces=None,
    exported_state_interfaces=None,
):
    return core.ControllerTopologyControllerDTO(
        controller_role_id=core.ControllerRoleId(role_id),
        control_interface_claims=[
            _contract_claim(contract_id, contract_revision, directions)
        ],
        exported_reference_interfaces=exported_reference_interfaces or [],
        exported_state_interfaces=exported_state_interfaces or [],
    )


def _topology_dto(contract_revision: str = ZERO_DIGEST):
    trajectory = _controller(
        "trajectory",
        "arm_position_trajectory",
        contract_revision,
        exported_reference_interfaces=[_interface("arm", "position")],
        exported_state_interfaces=[_interface("arm", "position")],
    )
    hardware = _controller(
        "hardware",
        "arm_position_trajectory",
        contract_revision,
        directions=[core.ControllerClaimDirection.kState],
        exported_state_interfaces=[_interface("arm", "position")],
    )
    edge = core.ControllerChainEdgeDTO(
        upstream_controller_role_id=core.ControllerRoleId("trajectory"),
        downstream_controller_role_id=core.ControllerRoleId("hardware"),
        reference_interfaces=[_interface("arm", "position")],
        state_interfaces=[_interface("arm", "position")],
    )
    edge_ref = core.ControllerChainEdgeRefDTO(
        upstream_controller_role_id=core.ControllerRoleId("trajectory"),
        downstream_controller_role_id=core.ControllerRoleId("hardware"),
    )
    activation_set = core.ControllerActivationSetDTO(
        activation_set_id=core.ActivationSetId("trajectory_active"),
        active_controller_role_ids=[
            core.ControllerRoleId("trajectory"),
            core.ControllerRoleId("hardware"),
        ],
        active_chain_edges=[edge_ref],
    )
    route = core.ControlRouteDTO(
        control_route_id=core.ControlRouteId("arm_position"),
        control_interface_contract_id=core.ControlInterfaceContractId(
            "arm_position_trajectory"
        ),
        control_interface_revision=core.ControlInterfaceRevision(
            contract_revision
        ),
        ordered_controller_role_ids=[
            core.ControllerRoleId("trajectory"),
            core.ControllerRoleId("hardware"),
        ],
        compatible_activation_set_ids=[
            core.ActivationSetId("trajectory_active")
        ],
    )
    topology = core.ControllerTopologyDTO(
        controller_topology_id=core.ControllerTopologyId("arm_default"),
        topology_revision=core.TopologyRevision(ZERO_DIGEST),
        controllers=[trajectory, hardware],
        chain_edges=[edge],
        activation_sets=[activation_set],
        legal_routes=[route],
    )
    return topology


def _schema_revision(record_kind):
    registry = core.build_qualified_closed_schema_registry()
    for entry in registry.catalog_schemas:
        if entry.record_kind == record_kind:
            assert entry.compiled_schema is not None
            return entry.compiled_schema.schema_revision
    raise AssertionError(f"missing closed schema for {record_kind}")


def _content_revision(
    payload: dict,
    *,
    record_kind: str,
    api_version: str,
):
    canonical_payload = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        + f"\x00catalog-record-revision/{record_kind}".encode()
        + f"\x00{api_version}\x00".encode()
        + canonical_payload.encode()
    )
    return "sha256:" + hashlib.sha256(preimage).hexdigest()


def _control_interface_contract_document(
    *,
    contract_id: str = "arm_position_trajectory",
):
    payload = {
        "api_version": "manyforge.control_interface_contract.v1",
        "control_interface_contract_id": contract_id,
        "command_interfaces": [
            {"resource_role_id": "arm", "interface_name": "position"}
        ],
        "state_interfaces": [
            {"resource_role_id": "arm", "interface_name": "position"}
        ],
        "ordering": ["arm"],
    }
    payload["control_interface_revision"] = _content_revision(
        payload,
        record_kind="control_interface_contract",
        api_version="manyforge.control_interface_contract.v1",
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _qualified_control_interface_contract():
    document = _control_interface_contract_document()
    payload = json.loads(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/controller-topology-python-contract"
    )
    reference = core.ControlInterfaceContractCatalogRecordRefDTO(
        record_id=core.ControlInterfaceContractId(
            payload["control_interface_contract_id"]
        ),
        record_revision=core.ControlInterfaceRevision(
            payload["control_interface_revision"]
        ),
        schema_id=core.SchemaId("manyforge.control_interface_contract.v1"),
        schema_revision=_schema_revision(
            core.CatalogRecordKind.kControlInterfaceContract
        ),
        content_digest=core.ContentDigest(
            "sha256:" + hashlib.sha256(document.encode()).hexdigest()
        ),
        content_addressed_store_key=store_key,
    )
    return core.resolve_and_qualify_control_interface_contract(
        reference, store_key, document
    )


def _controller_topology_document(
    contract_revision: str,
    *,
    topology_id: str = "arm_default",
):
    interface = {"resource_role_id": "arm", "interface_name": "position"}
    contract_ref = {
        "control_interface_contract_id": "arm_position_trajectory",
        "control_interface_revision": contract_revision,
    }
    payload = {
        "api_version": "manyforge.controller_topology.v1",
        "controller_topology_id": topology_id,
        "controllers": [
            {
                "controller_role_id": "hardware",
                "control_interface_claims": [
                    {
                        **contract_ref,
                        "claimed_directions": ["command", "state"],
                    }
                ],
                "exported_reference_interfaces": [interface],
                "exported_state_interfaces": [interface],
            },
            {
                "controller_role_id": "trajectory",
                "control_interface_claims": [
                    {
                        **contract_ref,
                        "claimed_directions": ["state"],
                    }
                ],
                "exported_reference_interfaces": [],
                "exported_state_interfaces": [],
            },
        ],
        "chain_edges": [
            {
                "upstream_controller_role_id": "trajectory",
                "downstream_controller_role_id": "hardware",
                "reference_interfaces": [interface],
                "state_interfaces": [interface],
            }
        ],
        "activation_sets": [
            {
                "activation_set_id": "trajectory_active",
                "active_controller_role_ids": ["hardware", "trajectory"],
                "active_chain_edges": [
                    {
                        "upstream_controller_role_id": "trajectory",
                        "downstream_controller_role_id": "hardware",
                    }
                ],
            }
        ],
        "legal_routes": [
            {
                "control_route_id": "arm_position",
                **contract_ref,
                "ordered_controller_role_ids": ["trajectory", "hardware"],
                "compatible_activation_set_ids": ["trajectory_active"],
            }
        ],
    }
    payload["topology_revision"] = _content_revision(
        payload,
        record_kind="controller_topology",
        api_version="manyforge.controller_topology.v1",
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _controller_topology_reference(
    document: str,
    *,
    topology_id: str | None = None,
    digest: str | None = None,
):
    payload = json.loads(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/controller-topology-python"
    )
    return core.ControllerTopologyCatalogRecordRefDTO(
        record_id=core.ControllerTopologyId(
            topology_id or payload["controller_topology_id"]
        ),
        record_revision=core.TopologyRevision(payload["topology_revision"]),
        schema_id=core.SchemaId("manyforge.controller_topology.v1"),
        schema_revision=_schema_revision(
            core.CatalogRecordKind.kControllerTopology
        ),
        content_digest=core.ContentDigest(
            digest
            or ("sha256:" + hashlib.sha256(document.encode()).hexdigest())
        ),
        content_addressed_store_key=store_key,
    )


def test_controller_topology_constants_and_direction_enum_are_exact():
    assert (
        core.CONTROLLER_TOPOLOGY_API_VERSION
        == "manyforge.controller_topology.v1"
    )
    assert core.ControllerClaimDirection.kCommand != (
        core.ControllerClaimDirection.kState
    )
    expected_limits = {
        "MAX_CONTROLLER_TOPOLOGY_CONTROLLERS": 256,
        "MAX_CONTROLLER_TOPOLOGY_CONTROL_INTERFACE_CLAIMS_PER_CONTROLLER": 256,
        "MAX_CONTROLLER_TOPOLOGY_CLAIMED_DIRECTIONS_PER_CLAIM": 2,
        "MAX_CONTROLLER_TOPOLOGY_EXPORTED_REFERENCE_INTERFACES_PER_CONTROLLER": 4096,
        "MAX_CONTROLLER_TOPOLOGY_EXPORTED_STATE_INTERFACES_PER_CONTROLLER": 4096,
        "MAX_CONTROLLER_TOPOLOGY_CHAIN_EDGES": 4096,
        "MAX_CONTROLLER_TOPOLOGY_REFERENCE_INTERFACES_PER_CHAIN_EDGE": 4096,
        "MAX_CONTROLLER_TOPOLOGY_STATE_INTERFACES_PER_CHAIN_EDGE": 4096,
        "MAX_CONTROLLER_TOPOLOGY_ACTIVATION_SETS": 256,
        "MAX_CONTROLLER_TOPOLOGY_ACTIVE_CONTROLLER_ROLES_PER_ACTIVATION_SET": 256,
        "MAX_CONTROLLER_TOPOLOGY_ACTIVE_CHAIN_EDGES_PER_ACTIVATION_SET": 4096,
        "MAX_CONTROLLER_TOPOLOGY_CONTROL_ROUTES": 256,
        "MAX_CONTROLLER_TOPOLOGY_ORDERED_CONTROLLER_ROLES_PER_ROUTE": 256,
        "MAX_CONTROLLER_TOPOLOGY_COMPATIBLE_ACTIVATION_SETS_PER_ROUTE": 256,
        "MAX_CONTROLLER_TOPOLOGY_DISTINCT_CONTROL_INTERFACE_CONTRACTS": 256,
    }
    assert {
        name: getattr(core, name) for name in expected_limits
    } == expected_limits


def test_controller_topology_dto_graph_has_read_only_python_parity():
    topology = _topology_dto()

    assert topology.controller_topology_id == core.ControllerTopologyId(
        "arm_default"
    )
    assert [
        controller.controller_role_id.value
        for controller in topology.controllers
    ] == ["trajectory", "hardware"]
    assert topology.controllers[0].control_interface_claims[
        0
    ].claimed_directions == [core.ControllerClaimDirection.kCommand]
    assert topology.chain_edges[0].upstream_controller_role_id == (
        core.ControllerRoleId("trajectory")
    )
    assert topology.activation_sets[0].active_chain_edges == [
        core.ControllerChainEdgeRefDTO(
            core.ControllerRoleId("trajectory"),
            core.ControllerRoleId("hardware"),
        )
    ]
    assert topology.legal_routes[0].compatible_activation_set_ids == [
        core.ActivationSetId("trajectory_active")
    ]
    assert topology == _topology_dto()

    controller = topology.controllers[0]
    claim = controller.control_interface_claims[0]
    edge = topology.chain_edges[0]
    activation_set = topology.activation_sets[0]
    edge_ref = activation_set.active_chain_edges[0]
    route = topology.legal_routes[0]
    read_only_properties = [
        (
            topology,
            (
                "controller_topology_id",
                "topology_revision",
                "controllers",
                "chain_edges",
                "activation_sets",
                "legal_routes",
            ),
        ),
        (
            controller,
            (
                "controller_role_id",
                "control_interface_claims",
                "exported_reference_interfaces",
                "exported_state_interfaces",
            ),
        ),
        (
            claim,
            (
                "control_interface_contract_id",
                "control_interface_revision",
                "claimed_directions",
            ),
        ),
        (
            edge,
            (
                "upstream_controller_role_id",
                "downstream_controller_role_id",
                "reference_interfaces",
                "state_interfaces",
            ),
        ),
        (
            activation_set,
            (
                "activation_set_id",
                "active_controller_role_ids",
                "active_chain_edges",
            ),
        ),
        (
            edge_ref,
            (
                "upstream_controller_role_id",
                "downstream_controller_role_id",
            ),
        ),
        (
            route,
            (
                "control_route_id",
                "control_interface_contract_id",
                "control_interface_revision",
                "ordered_controller_role_ids",
                "compatible_activation_set_ids",
            ),
        ),
    ]
    for value, properties in read_only_properties:
        for attribute in properties:
            with pytest.raises(AttributeError):
                setattr(value, attribute, getattr(value, attribute))
        with pytest.raises(AttributeError):
            value.extra = "not allowed"

    controllers_snapshot = topology.controllers
    controllers_snapshot.clear()
    assert len(topology.controllers) == 2


def test_controller_topology_strong_ids_are_not_interchangeable():
    assert core.ControllerRoleId("shared") != core.ActivationSetId("shared")
    assert core.ActivationSetId("shared") != core.ControlRouteId("shared")

    with pytest.raises(TypeError):
        core.ControllerChainEdgeRefDTO(
            upstream_controller_role_id=core.ControlRouteId("upstream"),
            downstream_controller_role_id=core.ControllerRoleId("downstream"),
        )
    with pytest.raises(TypeError):
        core.ControllerActivationSetDTO(
            activation_set_id=core.ControllerRoleId("active"),
            active_controller_role_ids=[],
            active_chain_edges=[],
        )
    with pytest.raises(TypeError):
        core.ControlRouteDTO(
            control_route_id=core.ActivationSetId("route"),
            control_interface_contract_id=core.ControlInterfaceContractId(
                "contract"
            ),
            control_interface_revision=core.ControlInterfaceRevision(
                ZERO_DIGEST
            ),
            ordered_controller_role_ids=[],
            compatible_activation_set_ids=[],
        )


def test_controller_topology_is_exactly_qualified_from_qualified_contracts():
    qualified_contract = _qualified_control_interface_contract()
    document = _controller_topology_document(
        qualified_contract.value.control_interface_revision.value
    )
    reference = _controller_topology_reference(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/controller-topology-python"
    )

    qualified = core.resolve_and_qualify_controller_topology(
        reference,
        store_key,
        document,
        [qualified_contract],
    )

    assert isinstance(qualified, core.QualifiedControllerTopology)
    assert qualified.value.controller_topology_id == (
        core.ControllerTopologyId("arm_default")
    )
    assert qualified.value.topology_revision == reference.record_revision
    assert [
        controller.controller_role_id.value
        for controller in qualified.value.controllers
    ] == ["hardware", "trajectory"]
    assert json.loads(qualified.normalized_canonical_json) == json.loads(
        document
    )
    with pytest.raises(TypeError):
        core.QualifiedControllerTopology()
    with pytest.raises(AttributeError):
        qualified.value = qualified.value

    with pytest.raises(TypeError):
        core.resolve_and_qualify_controller_topology(
            reference,
            store_key,
            document,
            [qualified_contract.value],
        )


def test_controller_topology_qualification_rejects_missing_or_stale_contracts():
    qualified_contract = _qualified_control_interface_contract()
    contract_revision = (
        qualified_contract.value.control_interface_revision.value
    )
    document = _controller_topology_document(contract_revision)
    reference = _controller_topology_reference(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/controller-topology-python"
    )

    with pytest.raises(
        core.ControllerTopologyCodecError,
        match="missing qualified control-interface contract",
    ):
        core.resolve_and_qualify_controller_topology(
            reference, store_key, document, []
        )

    stale_document = _controller_topology_document(ZERO_DIGEST)
    stale_reference = _controller_topology_reference(stale_document)
    with pytest.raises(
        core.ControllerTopologyCodecError,
        match="stale control-interface contract revision",
    ):
        core.resolve_and_qualify_controller_topology(
            stale_reference,
            store_key,
            stale_document,
            [qualified_contract],
        )


def test_controller_topology_qualification_rejects_referent_mismatches():
    qualified_contract = _qualified_control_interface_contract()
    document = _controller_topology_document(
        qualified_contract.value.control_interface_revision.value
    )
    reference = _controller_topology_reference(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/controller-topology-python"
    )

    with pytest.raises(
        core.ControllerTopologyCodecError, match="different store key"
    ):
        core.resolve_and_qualify_controller_topology(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            document,
            [qualified_contract],
        )

    wrong_id = _controller_topology_reference(
        document, topology_id="different"
    )
    with pytest.raises(core.ControllerTopologyCodecError):
        core.resolve_and_qualify_controller_topology(
            wrong_id,
            store_key,
            document,
            [qualified_contract],
        )

    wrong_digest = _controller_topology_reference(
        document, digest=ZERO_DIGEST
    )
    with pytest.raises(core.ControllerTopologyCodecError):
        core.resolve_and_qualify_controller_topology(
            wrong_digest,
            store_key,
            document,
            [qualified_contract],
        )
