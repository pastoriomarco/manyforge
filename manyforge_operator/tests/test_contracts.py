import hashlib

import rfc8785
import pytest
from pydantic import ValidationError

from manyforge_operator import (
    ArtifactKind,
    CommandOutcome,
    CONFIGURATION_SCHEMA,
    ContainerResourceIdentity,
    FailureRecord,
    HostIdentity,
    HostRole,
    LaunchConfiguration,
    LaunchInstanceState,
    LaunchInstanceStatus,
    LaunchOperation,
    LaunchProfile,
    OperatorCommandError,
    OperatorCommandResult,
    ProcessResourceIdentity,
    ReadinessAuthority,
    ReadinessObservation,
    ReadinessState,
    ResourceKind,
    ResourceObservation,
    ResourcePresence,
    RosTopology,
    RuntimeArtifactIdentity,
    ServiceEndpoint,
    ServiceKind,
    SourceIdentityV1,
    StartDisposition,
    can_transition,
    configuration_digest,
    decide_start,
    operator_launch_contract_schema,
    source_identity_fingerprint,
)
from .conftest import (
    BOOT_RUNTIME,
    SHA,
    make_readiness,
    make_ready_resources,
    make_source,
)


def test_source_identity_round_trip_and_fingerprint(source_factory):
    source = source_factory()
    assert SourceIdentityV1.model_validate_json(source.model_dump_json()) == source
    assert source.fingerprint_sha256 == source_identity_fingerprint(
        head_commit=source.head_commit,
        head_tree=source.head_tree,
        dirty=source.dirty,
        delta_sha256=source.delta_sha256,
    )


def test_configuration_digest_uses_exact_json_ready_preimage(configuration_factory):
    configuration = configuration_factory(profile=LaunchProfile.NATIVE)
    assert LaunchConfiguration.model_validate_json(configuration.model_dump_json()) == configuration
    payload = configuration.model_dump(mode="json", exclude={"configuration_digest"})
    assert configuration_digest(payload) == configuration.configuration_digest
    assert configuration_digest(configuration) == configuration.configuration_digest
    changed = dict(payload)
    changed["profile"] = LaunchProfile.ISAAC.value
    assert configuration_digest(changed) != configuration.configuration_digest
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {
                **configuration.model_dump(),
                "configuration_digest": "e" * 64,
            }
        )
    with pytest.raises(ValueError):
        configuration_digest({**payload, "unexpected": True})


def test_native_and_isaac_configuration_rules(configuration_factory):
    native = configuration_factory(profile=LaunchProfile.NATIVE)
    isaac = configuration_factory(profile=LaunchProfile.ISAAC)
    assert native.ros_topology is None
    assert isaac.ros_topology is not None
    assert isaac.ros_topology.static_peers == ["192.168.1.10", "192.168.1.20"]
    native_payload = native.model_dump(mode="json", exclude={"configuration_digest"})
    native_payload["ros_topology"] = {
        "domain": 42,
        "rmw": "rmw_fastrtps_cpp",
        "automatic_discovery_range": "LOCALHOST",
        "static_peers": [],
    }
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {**native_payload, "configuration_digest": configuration_digest(native_payload)}
        )
    isaac_payload = isaac.model_dump(mode="json", exclude={"configuration_digest"})
    isaac_payload["ros_topology"] = None
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {**isaac_payload, "configuration_digest": configuration_digest(isaac_payload)}
        )
    reordered = native.model_dump(mode="json", exclude={"configuration_digest"})
    reordered["hosts"].reverse()
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {**reordered, "configuration_digest": configuration_digest(reordered)}
        )
    mismatched_artifact = native.model_dump(
        mode="json", exclude={"configuration_digest"}
    )
    mismatched_artifact["artifacts"][0]["source_fingerprint"] = "f" * 64
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {
                **mismatched_artifact,
                "configuration_digest": configuration_digest(mismatched_artifact),
            }
        )
    dirty_source = make_source(dirty=True)
    dirty_payload = native.model_dump(mode="json", exclude={"configuration_digest"})
    dirty_payload["source"] = dirty_source.model_dump(mode="json")
    dirty_payload["artifacts"][0]["source_fingerprint"] = (
        dirty_source.fingerprint_sha256
    )
    with pytest.raises(ValidationError):
        LaunchConfiguration.model_validate(
            {
                **dirty_payload,
                "configuration_digest": configuration_digest(dirty_payload),
            }
        )


def test_topology_bounds_and_ordering():
    topology = RosTopology(
        domain=0,
        rmw="rmw_fastrtps_cpp",
        automatic_discovery_range="LOCALHOST",
        static_peers=["10.0.0.1", "10.0.0.2"],
    )
    assert topology.domain == 0
    with pytest.raises(ValidationError):
        RosTopology(
            domain=233,
            rmw="rmw_fastrtps_cpp",
            automatic_discovery_range="LOCALHOST",
            static_peers=[],
        )
    with pytest.raises(ValidationError):
        RosTopology(
            domain=1,
            rmw="rmw bad",
            automatic_discovery_range="LOCALHOST",
            static_peers=[],
        )
    with pytest.raises(ValidationError):
        RosTopology(
            domain=1,
            rmw="rmw_fastrtps_cpp",
            automatic_discovery_range="LOCALHOST",
            static_peers=["10.0.0.2", "10.0.0.1"],
        )


def test_endpoints_enforce_frozen_placement_and_url_rules():
    endpoint = ServiceEndpoint(
        service=ServiceKind.COMPOSER,
        bind_host="0.0.0.0",
        port=9000,
        advertised_host="runtime-host",
        url="https://runtime-host:9000/",
    )
    assert endpoint.url == "https://runtime-host:9000/"
    for url in (
        "ftp://runtime-host:9000/",
        "http://user:pass@runtime-host:9000/",
        "http://runtime-host:9000/health",
        "http://runtime-host:9000/?query=1",
    ):
        with pytest.raises(ValidationError):
            ServiceEndpoint(
                service=ServiceKind.COMPOSER,
                bind_host="0.0.0.0",
                port=9000,
                advertised_host="runtime-host",
                url=url,
            )
    with pytest.raises(ValidationError):
        ServiceEndpoint(
            service=ServiceKind.COMPOSER,
            bind_host="127.0.0.1",
            port=9000,
            advertised_host="runtime-host",
            url="http://runtime-host:9000/",
        )


def test_artifact_substrate_xor_and_bounds(source_factory):
    source = source_factory()
    common = {
        "host_id": "runtime",
        "source_fingerprint": source.fingerprint_sha256,
        "package_aggregate_sha256": SHA,
        "compiler_build_manifest_sha256": SHA,
        "node_environment_sha256": SHA,
    }
    mounted = RuntimeArtifactIdentity(
        **common, kind=ArtifactKind.MOUNTED_INSTALL, install_root="/opt/manyforge"
    )
    image = RuntimeArtifactIdentity(
        **common, kind=ArtifactKind.CONTAINER_IMAGE, container_image_id=SHA
    )
    assert mounted.install_root == "/opt/manyforge"
    assert image.container_image_id == SHA
    for kind, values in (
        (ArtifactKind.CONTAINER_IMAGE, {"install_root": "/opt/manyforge"}),
        (ArtifactKind.MOUNTED_INSTALL, {"container_image_id": SHA}),
        (ArtifactKind.MOUNTED_INSTALL, {}),
    ):
        with pytest.raises(ValidationError):
            RuntimeArtifactIdentity(**common, kind=kind, **values)
    with pytest.raises(ValidationError):
        RuntimeArtifactIdentity(
            **common,
            kind=ArtifactKind.CONTAINER_IMAGE,
            container_image_id=SHA,
            install_root="/opt/manyforge",
        )


def test_resource_graph_is_discriminated_and_round_trips(configuration_factory, status_factory):
    status = status_factory(
        configuration_factory(artifact_kind=ArtifactKind.CONTAINER_IMAGE),
        with_resources=True,
    )
    assert isinstance(status.owned_resources[0], ContainerResourceIdentity)
    assert isinstance(status.owned_resources[1], ProcessResourceIdentity)
    assert LaunchInstanceStatus.model_validate_json(status.model_dump_json()) == status
    broken = status.model_dump(mode="json")
    broken["owned_resources"][0]["kind"] = ResourceKind.PROCESS.value
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(broken)


def test_resource_observation_readiness_and_failure_dtos():
    process, container = make_ready_resources()[1], make_ready_resources()[0]
    assert process.boot_id == BOOT_RUNTIME
    observation = ResourceObservation(
        resource_id=container.resource_id,
        presence=ResourcePresence.PRESENT,
        observed_at_unix_ns=42,
    )
    readiness = make_readiness()
    failure = FailureRecord(
        reason_code="component_lost",
        component_id="composer",
        detail="owner stopped responding",
        occurred_at_unix_ns=43,
    )
    assert observation.model_validate_json(observation.model_dump_json()) == observation
    assert ReadinessObservation.model_validate_json(readiness.model_dump_json()) == readiness
    assert FailureRecord.model_validate_json(failure.model_dump_json()) == failure


def test_status_requires_identity_order_coverage_and_truthful_ready_state(
    configuration_factory, status_factory
):
    configuration = configuration_factory()
    status = status_factory(configuration, with_resources=True)
    assert status.state is LaunchInstanceState.READY
    wrong_order = status.model_dump(mode="json")
    wrong_order["owned_resources"].reverse()
    wrong_order["resource_observations"].reverse()
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(wrong_order)
    missing_observation = status.model_dump(mode="json")
    missing_observation["resource_observations"] = []
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(missing_observation)
    not_ready = status.model_dump(mode="json")
    not_ready["readiness_observations"][0]["state"] = ReadinessState.DEGRADED.value
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(not_ready)


def test_command_result_success_error_and_round_trip(configuration_factory, status_factory):
    status = status_factory(configuration_factory(), with_resources=True)
    result = OperatorCommandResult(
        schema_version="manyforge.operator_command_result.v1",
        operation=LaunchOperation.START,
        outcome=CommandOutcome.STARTED,
        status=status,
    )
    assert OperatorCommandResult.model_validate_json(result.model_dump_json()) == result
    conflict = OperatorCommandResult(
        schema_version="manyforge.operator_command_result.v1",
        operation=LaunchOperation.START,
        outcome=CommandOutcome.CONFLICT,
        error=OperatorCommandError(code="port_occupied", message="port is occupied"),
    )
    assert conflict.error is not None
    with pytest.raises(ValidationError):
        OperatorCommandResult(
            schema_version="manyforge.operator_command_result.v1",
            operation=LaunchOperation.START,
            outcome=CommandOutcome.STARTED,
        )
    with pytest.raises(ValidationError):
        OperatorCommandResult(
            schema_version="manyforge.operator_command_result.v1",
            operation=LaunchOperation.START,
            outcome=CommandOutcome.CONFLICT,
        )


def test_strict_extra_wrong_type_and_bounded_inputs(source_factory):
    with pytest.raises(ValidationError):
        SourceIdentityV1(
            schema_version="manyforge.source_identity.v1",
            head_commit="b" * 40,
            head_tree="c" * 40,
            dirty=False,
            delta_sha256="d" * 64,
            fingerprint_sha256=source_factory().fingerprint_sha256,
            extra="forbidden",
        )
    with pytest.raises(ValidationError):
        source_factory().head_commit = "f" * 40
    with pytest.raises(ValidationError):
        HostIdentity(
            host_id="runtime",
            role=HostRole.RUNTIME,
            hostname_or_address="not a host",
            boot_id="22222222-2222-2222-2222-222222222222",
        )
    with pytest.raises(ValidationError):
        HostIdentity(
            host_id="runtime",
            role=HostRole.RUNTIME,
            hostname_or_address="runtime-host",
            boot_id=222,
        )
    with pytest.raises(ValidationError):
        ServiceEndpoint(
            service="not-a-service",
            bind_host="0.0.0.0",
            port="9000",
            advertised_host="runtime-host",
            url="http://runtime-host:9000/",
        )
    with pytest.raises(ValidationError):
        SourceIdentityV1(
            schema_version=CONFIGURATION_SCHEMA,
            head_commit="b" * 40,
            head_tree="c" * 40,
            dirty=False,
            delta_sha256="d" * 64,
            fingerprint_sha256="e" * 64,
        )


def test_source_digest_and_host_url_digest_bounds(source_factory):
    source = source_factory()
    with pytest.raises(ValidationError):
        SourceIdentityV1(
            schema_version="manyforge.source_identity.v1",
            head_commit="B" * 40,
            head_tree=source.head_tree,
            dirty=False,
            delta_sha256=source.delta_sha256,
            fingerprint_sha256=source.fingerprint_sha256,
        )
    with pytest.raises(ValidationError):
        ServiceEndpoint(
            service=ServiceKind.COMPOSER,
            bind_host="0.0.0.0",
            port=9000,
            advertised_host="runtime-host",
            url="http://runtime-host/",
        )
    with pytest.raises(ValidationError):
        RuntimeArtifactIdentity(
            host_id="runtime",
            kind=ArtifactKind.CONTAINER_IMAGE,
            source_fingerprint="not-a-digest",
            package_aggregate_sha256=SHA,
            compiler_build_manifest_sha256=SHA,
            node_environment_sha256=SHA,
            container_image_id=SHA,
        )


def test_can_transition_matches_the_complete_legal_table():
    legal = {
        LaunchInstanceState.UNAVAILABLE: {
            LaunchInstanceState.UNAVAILABLE,
            LaunchInstanceState.STARTING,
            LaunchInstanceState.STOPPED,
        },
        LaunchInstanceState.STARTING: {
            LaunchInstanceState.STARTING,
            LaunchInstanceState.READY,
            LaunchInstanceState.DEGRADED,
            LaunchInstanceState.STOPPING,
        },
        LaunchInstanceState.READY: {
            LaunchInstanceState.READY,
            LaunchInstanceState.DEGRADED,
            LaunchInstanceState.STOPPING,
        },
        LaunchInstanceState.DEGRADED: {
            LaunchInstanceState.DEGRADED,
            LaunchInstanceState.STARTING,
            LaunchInstanceState.READY,
            LaunchInstanceState.STOPPING,
        },
        LaunchInstanceState.STOPPING: {
            LaunchInstanceState.STOPPING,
            LaunchInstanceState.STOPPED,
            LaunchInstanceState.DEGRADED,
        },
        LaunchInstanceState.STOPPED: {
            LaunchInstanceState.STOPPED,
            LaunchInstanceState.STARTING,
        },
    }
    for previous in LaunchInstanceState:
        for next_state in LaunchInstanceState:
            assert can_transition(previous, next_state) is (
                next_state in legal[previous]
            )
    assert can_transition(LaunchInstanceState.STOPPING, LaunchInstanceState.STOPPED)


def test_can_transition_rejects_illegal_edges_and_unknown_states():
    for previous, next_state in (
        (LaunchInstanceState.READY, LaunchInstanceState.STARTING),
        (LaunchInstanceState.STOPPED, LaunchInstanceState.READY),
        (LaunchInstanceState.UNAVAILABLE, LaunchInstanceState.READY),
        (LaunchInstanceState.STOPPING, LaunchInstanceState.READY),
    ):
        assert not can_transition(previous, next_state)
    with pytest.raises(ValueError):
        can_transition("not-a-state", LaunchInstanceState.READY)
    with pytest.raises(TypeError):
        can_transition(1, LaunchInstanceState.READY)


def test_stopped_cleanup_guard_rejects_present_resources(configuration_factory, status_factory):
    stopped = status_factory(
        configuration_factory(), state=LaunchInstanceState.STOPPED, with_resources=True
    )
    assert all(
        observation.presence is ResourcePresence.ABSENT
        for observation in stopped.resource_observations
    )
    dishonest = stopped.model_dump(mode="json")
    dishonest["resource_observations"][0]["presence"] = ResourcePresence.PRESENT.value
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(dishonest)
    ready_row = stopped.model_dump(mode="json")
    ready_row["readiness_observations"] = [
        make_readiness().model_dump(mode="json")
    ]
    with pytest.raises(ValidationError):
        LaunchInstanceStatus.model_validate(ready_row)


def test_decide_start_covers_absent_idempotent_recovery_and_conflict(
    configuration_factory, status_factory
):
    configuration = configuration_factory()
    assert decide_start(None, configuration.configuration_digest) is StartDisposition.CREATE
    for state in (LaunchInstanceState.STARTING, LaunchInstanceState.READY):
        assert (
            decide_start(status_factory(configuration, state=state), configuration.configuration_digest)
            is StartDisposition.IDEMPOTENT
        )
    for state in (LaunchInstanceState.DEGRADED, LaunchInstanceState.UNAVAILABLE):
        assert (
            decide_start(status_factory(configuration, state=state), configuration.configuration_digest)
            is StartDisposition.RECOVER
        )
    assert (
        decide_start(
            status_factory(configuration, state=LaunchInstanceState.STOPPING),
            configuration.configuration_digest,
        )
        is StartDisposition.CONFLICT
    )


def test_decide_start_different_configuration_requires_stopped_absence(
    configuration_factory, status_factory
):
    configuration = configuration_factory()
    other = configuration_factory(profile=LaunchProfile.ISAAC)
    stopped = status_factory(configuration, state=LaunchInstanceState.STOPPED)
    assert decide_start(stopped, other.configuration_digest) is StartDisposition.CREATE
    assert (
        decide_start(status_factory(configuration, state=LaunchInstanceState.READY), other.configuration_digest)
        is StartDisposition.CONFLICT
    )
    with pytest.raises(ValueError):
        decide_start(None, "not-a-sha")


def test_contract_schema_is_closed_discriminated_and_stable():
    first = operator_launch_contract_schema()
    second = operator_launch_contract_schema()
    assert first == second
    assert first["$schema"] == "https://json-schema.org/draft/2020-12/schema"
    assert first["$id"] == "manyforge.operator_launch_contract.v1"
    assert first["discriminator"]["propertyName"] == "schema_version"
    assert {"LaunchConfiguration", "LaunchInstanceStatus", "OperatorCommandResult"} <= set(
        first["$defs"]
    )
    for definition in first["$defs"].values():
        if "properties" in definition:
            assert definition["additionalProperties"] is False
    configuration_required = set(first["$defs"]["LaunchConfiguration"]["required"])
    assert {
        "schema_version",
        "profile",
        "source",
        "hosts",
        "artifacts",
        "endpoints",
        "ros_topology",
        "configuration_digest",
    } <= configuration_required
    resources = first["$defs"]["LaunchInstanceStatus"]["properties"]["owned_resources"]["items"]
    assert resources["discriminator"]["propertyName"] == "kind"
    assert {
        ref["$ref"] for ref in resources["oneOf"]
    } == {
        "#/$defs/ProcessResourceIdentity",
        "#/$defs/ContainerResourceIdentity",
    }
    assert rfc8785.dumps(first) == rfc8785.dumps(second)
    assert hashlib.sha256(rfc8785.dumps(first)).hexdigest() == (
        "89400fdfa3f72a5553f0e094432d1fb7870a5d0a64dc9c8f1660f0fc3e4e996b"
    )


def test_public_boundary_has_no_runtime_owner_or_launcher_exports():
    import manyforge_operator

    forbidden = {
        "launch",
        "launcher",
        "OperatorLauncher",
        "ProfileAdapter",
        "Persistence",
        "start",
        "stop",
        "subprocess",
        "socket",
    }
    assert forbidden.isdisjoint(set(manyforge_operator.__all__))
    assert not any(hasattr(manyforge_operator, name) for name in forbidden)
