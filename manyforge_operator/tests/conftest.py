from collections.abc import Callable

import pytest

from manyforge_operator import (
    ArtifactKind,
    CONFIGURATION_SCHEMA,
    HostIdentity,
    HostRole,
    LaunchConfiguration,
    LaunchInstanceState,
    LaunchProfile,
    ReadinessAuthority,
    ReadinessObservation,
    ReadinessState,
    ResourceObservation,
    ResourcePresence,
    ResourceKind,
    RuntimeArtifactIdentity,
    SERVICE_PORTS,
    ServiceEndpoint,
    ServiceKind,
    SourceIdentityV1,
    RosTopology,
    configuration_digest,
    source_identity_fingerprint,
    ContainerResourceIdentity,
    ProcessResourceIdentity,
)

SHA = "a" * 64
GIT_COMMIT = "b" * 40
GIT_TREE = "c" * 40
BOOT_CONTROL = "11111111-1111-1111-1111-111111111111"
BOOT_RUNTIME = "22222222-2222-2222-2222-222222222222"
BOOT_SIMULATOR = "33333333-3333-3333-3333-333333333333"


def make_source(*, dirty: bool = False) -> SourceIdentityV1:
    delta = "d" * 64
    fingerprint = source_identity_fingerprint(
        head_commit=GIT_COMMIT,
        head_tree=GIT_TREE,
        dirty=dirty,
        delta_sha256=delta,
    )
    return SourceIdentityV1(
        schema_version="manyforge.source_identity.v1",
        head_commit=GIT_COMMIT,
        head_tree=GIT_TREE,
        dirty=dirty,
        delta_sha256=delta,
        fingerprint_sha256=fingerprint,
    )


def make_configuration(
    *,
    profile: LaunchProfile = LaunchProfile.NATIVE,
    artifact_kind: ArtifactKind = ArtifactKind.CONTAINER_IMAGE,
) -> LaunchConfiguration:
    source = make_source()
    hosts = [
        HostIdentity(
            host_id="control",
            role=HostRole.CONTROL,
            hostname_or_address="thor",
            boot_id=BOOT_CONTROL,
        ),
        HostIdentity(
            host_id="runtime",
            role=HostRole.RUNTIME,
            hostname_or_address="orion",
            boot_id=BOOT_RUNTIME,
        ),
    ]
    if profile is LaunchProfile.ISAAC:
        hosts.append(
            HostIdentity(
                host_id="simulator",
                role=HostRole.SIMULATOR,
                hostname_or_address="isaac",
                boot_id=BOOT_SIMULATOR,
            )
        )
    if artifact_kind is ArtifactKind.CONTAINER_IMAGE:
        artifact = RuntimeArtifactIdentity(
            host_id="runtime",
            kind=artifact_kind,
            source_fingerprint=source.fingerprint_sha256,
            package_aggregate_sha256=SHA,
            compiler_build_manifest_sha256=SHA,
            node_environment_sha256=SHA,
            container_image_id=SHA,
        )
    else:
        artifact = RuntimeArtifactIdentity(
            host_id="runtime",
            kind=artifact_kind,
            source_fingerprint=source.fingerprint_sha256,
            package_aggregate_sha256=SHA,
            compiler_build_manifest_sha256=SHA,
            node_environment_sha256=SHA,
            install_root="/opt/manyforge",
        )
    endpoints = [
        ServiceEndpoint(
            service=service,
            bind_host={
                ServiceKind.COMPOSER: "0.0.0.0",
                ServiceKind.HEADLESS: "127.0.0.1",
                ServiceKind.PROGRAM_RUNNER: "127.0.0.1",
            }[service],
            port=SERVICE_PORTS[service.value],
            advertised_host="orion",
            url=f"http://orion:{SERVICE_PORTS[service.value]}/",
        )
        for service in ServiceKind
    ]
    topology = (
        RosTopology(
            domain=42,
            rmw="rmw_fastrtps_cpp",
            automatic_discovery_range="LOCALHOST",
            static_peers=["192.168.1.10", "192.168.1.20"],
        )
        if profile is LaunchProfile.ISAAC
        else None
    )
    payload = {
        "schema_version": CONFIGURATION_SCHEMA,
        "profile": profile.value,
        "source": source.model_dump(mode="json"),
        "hosts": [host.model_dump(mode="json") for host in hosts],
        "artifacts": [artifact.model_dump(mode="json")],
        "endpoints": [endpoint.model_dump(mode="json") for endpoint in endpoints],
        "ros_topology": topology.model_dump(mode="json") if topology else None,
    }
    return LaunchConfiguration(
        schema_version=CONFIGURATION_SCHEMA,
        profile=profile,
        source=source,
        hosts=hosts,
        artifacts=[artifact],
        endpoints=endpoints,
        ros_topology=topology,
        configuration_digest=configuration_digest(payload),
    )


def make_ready_resources() -> list[ProcessResourceIdentity | ContainerResourceIdentity]:
    return [
        ContainerResourceIdentity(
            kind=ResourceKind.CONTAINER,
            resource_id="container-1",
            component_id="composer",
            host_id="runtime",
            container_id=SHA,
            image_id=SHA,
            launch_instance_label="launch-1",
            log_reference="logs/composer",
        ),
        ProcessResourceIdentity(
            kind=ResourceKind.PROCESS,
            resource_id="process-1",
            component_id="headless",
            host_id="runtime",
            pid=101,
            start_ticks=202,
            boot_id=BOOT_RUNTIME,
            process_group_id=303,
            log_reference="logs/headless",
        ),
    ]


def make_readiness(state: ReadinessState = ReadinessState.READY) -> ReadinessObservation:
    return ReadinessObservation(
        component_id="composer",
        authority=ReadinessAuthority.OWNER_ISSUED,
        state=state,
        source_schema="manyforge.readiness.v1",
        owner_id="headless",
        owner_epoch="epoch-1",
        payload_sha256=SHA,
        observed_at_unix_ns=500,
        reason_code=None if state is ReadinessState.READY else "warming_up",
    )


def make_status(
    configuration: LaunchConfiguration,
    *,
    state: LaunchInstanceState = LaunchInstanceState.READY,
    with_resources: bool | None = None,
):
    from manyforge_operator import LaunchInstanceStatus

    if with_resources is None:
        with_resources = state is LaunchInstanceState.READY
    resources = make_ready_resources() if with_resources else []
    presence = (
        ResourcePresence.PRESENT
        if state is not LaunchInstanceState.STOPPED
        else ResourcePresence.ABSENT
    )
    observations = [
        ResourceObservation(
            resource_id=resource.resource_id,
            presence=presence,
            observed_at_unix_ns=600,
        )
        for resource in resources
    ]
    readiness = [make_readiness()] if state is LaunchInstanceState.READY else []
    return LaunchInstanceStatus(
        schema_version="manyforge.launch_instance_status.v1",
        launch_instance_id="launch-1",
        record_generation=1,
        configuration=configuration,
        configuration_digest=configuration.configuration_digest,
        state=state,
        owned_resources=resources,
        resource_observations=observations,
        readiness_observations=readiness,
        updated_at_unix_ns=700,
    )


@pytest.fixture
def source_factory() -> Callable[..., SourceIdentityV1]:
    return make_source


@pytest.fixture
def configuration_factory() -> Callable[..., LaunchConfiguration]:
    return make_configuration


@pytest.fixture
def status_factory() -> Callable[..., object]:
    return make_status
