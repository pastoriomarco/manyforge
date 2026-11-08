"""Frozen ManyForge operator-launch machine contracts.

This package is the strict contract layer of the operator-launch
correction: bounded identity, configuration, status, and command DTOs,
the deterministic contract schema, and the pure lifecycle/start
decisions.  It performs no I/O at import time and exports no launcher,
persistence, subprocess, container, socket, HTTP, or profile-adapter
constructor.  The internal ``manyforge_operator.profile`` protocol seam
is deliberately not exported here.
"""

from .contracts import (
    CONFIGURATION_SCHEMA,
    LAUNCH_CONTRACT_SCHEMA,
    RESULT_SCHEMA,
    SOURCE_IDENTITY_SCHEMA,
    STATUS_SCHEMA,
    ArtifactKind,
    CommandOutcome,
    ContainerResourceIdentity,
    FailureRecord,
    HostIdentity,
    HostRole,
    LaunchConfiguration,
    LaunchInstanceStatus,
    LaunchInstanceState,
    LaunchOperation,
    LaunchProfile,
    OwnedResourceIdentity,
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
    SERVICE_BIND_HOSTS,
    SERVICE_PORTS,
    ServiceEndpoint,
    ServiceKind,
    SourceIdentityV1,
    StartDisposition,
    StrictOperatorContract,
    can_transition,
    configuration_digest,
    decide_start,
    operator_launch_contract_schema,
    source_identity_fingerprint,
)


__all__ = [
    # Frozen schema identifiers
    "CONFIGURATION_SCHEMA",
    "LAUNCH_CONTRACT_SCHEMA",
    "RESULT_SCHEMA",
    "SOURCE_IDENTITY_SCHEMA",
    "STATUS_SCHEMA",
    # Enums
    "ArtifactKind",
    "CommandOutcome",
    "HostRole",
    "LaunchInstanceState",
    "LaunchOperation",
    "LaunchProfile",
    "ReadinessAuthority",
    "ReadinessState",
    "ResourceKind",
    "ResourcePresence",
    "ServiceKind",
    "StartDisposition",
    # DTOs
    "ContainerResourceIdentity",
    "FailureRecord",
    "HostIdentity",
    "LaunchConfiguration",
    "LaunchInstanceStatus",
    "OperatorCommandError",
    "OperatorCommandResult",
    "OwnedResourceIdentity",
    "ProcessResourceIdentity",
    "ReadinessObservation",
    "ResourceObservation",
    "RosTopology",
    "RuntimeArtifactIdentity",
    "ServiceEndpoint",
    "SourceIdentityV1",
    "StrictOperatorContract",
    # Frozen per-service placement
    "SERVICE_BIND_HOSTS",
    "SERVICE_PORTS",
    # Pure decisions and contract helpers
    "can_transition",
    "configuration_digest",
    "decide_start",
    "operator_launch_contract_schema",
    "source_identity_fingerprint",
]
