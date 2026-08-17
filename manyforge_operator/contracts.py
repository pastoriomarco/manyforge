"""Frozen O1.2 operator-launch machine contracts for ManyForge.

This module is the strict, side-effect-free contract layer of the O1
operator-launch correction: bounded identity, configuration, status, and
command DTOs plus the pure lifecycle and start decisions.  It records
machine facts only.  It owns no runtime semantics and performs no I/O:
no launcher, persistence, subprocess, container, socket, HTTP, timing,
or process-inspection behavior lives here.  Manager, Headless, Program
runner, Kernel, provider, and simulator adapters retain their qualified
semantic ownership; the common orchestration (O1.3+) consumes these DTOs
and owns locking, persistence, and exact fallback policy.
"""

from __future__ import annotations

import hashlib
import ipaddress
import re
import unicodedata
from collections.abc import Mapping
from enum import StrEnum
from typing import Annotated, Any, Literal
from urllib.parse import urlsplit

import rfc8785
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    TypeAdapter,
    field_validator,
    model_validator,
)

# ---------------------------------------------------------------------------
# Frozen schema identifiers
# ---------------------------------------------------------------------------

LAUNCH_CONTRACT_SCHEMA = "manyforge.operator_launch_contract.v1"
CONFIGURATION_SCHEMA = "manyforge.operator_launch_configuration.v1"
STATUS_SCHEMA = "manyforge.launch_instance_status.v1"
RESULT_SCHEMA = "manyforge.operator_command_result.v1"
SOURCE_IDENTITY_SCHEMA = "manyforge.source_identity.v1"
JSON_SCHEMA_DRAFT_2020_12 = "https://json-schema.org/draft/2020-12/schema"

# ---------------------------------------------------------------------------
# Bounds
# ---------------------------------------------------------------------------

MAX_SAFE_INTEGER = 9_007_199_254_740_991

MAX_ID_BYTES = 256
MAX_HOST_REFERENCE_BYTES = 253
MAX_DISCOVERY_RANGE_BYTES = 64
MAX_RMW_BYTES = 128
MAX_REASON_CODE_BYTES = 128
MAX_DETAIL_BYTES = 4096
MAX_LOG_REFERENCE_BYTES = 512
MAX_SOURCE_SCHEMA_BYTES = 256
MAX_INSTALL_ROOT_BYTES = 1024
MAX_URL_BYTES = 2048

MAX_HOSTS = 16
MAX_ARTIFACTS = 32
MAX_OWNED_RESOURCES = 64
MAX_RESOURCE_OBSERVATIONS = 64
MAX_READINESS_OBSERVATIONS = 64
MAX_STATIC_PEERS = 16

# Frozen per-service placement.  The advertised host is deployment data and
# deliberately stays in the DTO; only the bind host and port are frozen.
SERVICE_BIND_HOSTS: dict[str, str] = {
    "composer": "0.0.0.0",
    "headless": "127.0.0.1",
    "program_runner": "127.0.0.1",
}
SERVICE_PORTS: dict[str, int] = {
    "composer": 9000,
    "headless": 9010,
    "program_runner": 9011,
}

# ---------------------------------------------------------------------------
# Patterns
# ---------------------------------------------------------------------------

_GIT_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_BOOT_ID_RE = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)
_RMW_RE = re.compile(r"^[a-z][a-z0-9_]{0,127}$")
_DISCOVERY_RANGE_RE = re.compile(r"^[A-Z][A-Z0-9_]{0,63}$")
_REASON_CODE_RE = re.compile(r"^[a-z][a-z0-9_]{0,127}$")
_HOSTNAME_LABEL_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")


# ---------------------------------------------------------------------------
# Enums (exact frozen vocabularies)
# ---------------------------------------------------------------------------


class LaunchOperation(StrEnum):
    START = "start"
    STATUS = "status"
    STOP = "stop"


class LaunchProfile(StrEnum):
    NATIVE = "native"
    ISAAC = "isaac"


class HostRole(StrEnum):
    CONTROL = "control"
    RUNTIME = "runtime"
    SIMULATOR = "simulator"


class ServiceKind(StrEnum):
    COMPOSER = "composer"
    HEADLESS = "headless"
    PROGRAM_RUNNER = "program_runner"


class LaunchInstanceState(StrEnum):
    UNAVAILABLE = "unavailable"
    STARTING = "starting"
    READY = "ready"
    DEGRADED = "degraded"
    STOPPING = "stopping"
    STOPPED = "stopped"


class ReadinessState(StrEnum):
    """Readiness rows use the same six-state vocabulary as instances."""

    UNAVAILABLE = "unavailable"
    STARTING = "starting"
    READY = "ready"
    DEGRADED = "degraded"
    STOPPING = "stopping"
    STOPPED = "stopped"


class ResourcePresence(StrEnum):
    ABSENT = "absent"
    PRESENT = "present"
    UNKNOWN = "unknown"


class ReadinessAuthority(StrEnum):
    OWNER_ISSUED = "owner_issued"
    EXTERNAL_OBSERVATION = "external_observation"


class ArtifactKind(StrEnum):
    CONTAINER_IMAGE = "container_image"
    MOUNTED_INSTALL = "mounted_install"


class ResourceKind(StrEnum):
    PROCESS = "process"
    CONTAINER = "container"


class CommandOutcome(StrEnum):
    STARTED = "started"
    ALREADY_RUNNING = "already_running"
    REPORTED = "reported"
    STOPPING = "stopping"
    STOPPED = "stopped"
    ALREADY_STOPPED = "already_stopped"
    CONFLICT = "conflict"
    FAILED = "failed"


class StartDisposition(StrEnum):
    CREATE = "create"
    IDEMPOTENT = "idempotent"
    RECOVER = "recover"
    CONFLICT = "conflict"


# ---------------------------------------------------------------------------
# Bounded field helpers
# ---------------------------------------------------------------------------


def _bounded_nfc(value: str, *, label: str, maximum_bytes: int) -> str:
    if not value or unicodedata.normalize("NFC", value) != value:
        raise ValueError(f"{label} must be non-empty NFC text")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise ValueError(f"{label} exceeds {maximum_bytes} UTF-8 bytes")
    return value


def _bounded_git_sha(value: str, *, label: str) -> str:
    if not _GIT_SHA_RE.fullmatch(value):
        raise ValueError(f"{label} must be one full lowercase Git SHA")
    return value


def _bounded_sha256(value: str, *, label: str) -> str:
    if not _SHA256_RE.fullmatch(value):
        raise ValueError(f"{label} must be one full lowercase SHA-256")
    return value


def _bounded_boot_id(value: str, *, label: str) -> str:
    if not _BOOT_ID_RE.fullmatch(value):
        raise ValueError(f"{label} must be one lowercase Linux boot UUID")
    return value


def _bounded_host_reference(value: str, *, label: str) -> str:
    value = _bounded_nfc(
        value, label=label, maximum_bytes=MAX_HOST_REFERENCE_BYTES
    )
    try:
        ipaddress.ip_address(value)
        return value
    except ValueError:
        pass
    labels = value.split(".")
    if len(labels) > 63 or any(
        not _HOSTNAME_LABEL_RE.fullmatch(part) for part in labels
    ):
        raise ValueError(f"{label} must be one hostname or IP address")
    return value


def _bounded_reason_code(value: str, *, label: str) -> str:
    value = _bounded_nfc(value, label=label, maximum_bytes=MAX_REASON_CODE_BYTES)
    if not _REASON_CODE_RE.fullmatch(value):
        raise ValueError(f"{label} must be one lowercase snake_case reason code")
    return value


def _bounded_install_root(value: str, *, label: str) -> str:
    value = _bounded_nfc(value, label=label, maximum_bytes=MAX_INSTALL_ROOT_BYTES)
    if not value.startswith("/") or "\\" in value:
        raise ValueError(f"{label} must be one absolute install root")
    if value == "/" or value.endswith("/"):
        raise ValueError(f"{label} must be one normalized absolute install root")
    if any(part in ("", ".", "..") for part in value[1:].split("/")):
        raise ValueError(f"{label} must be one normalized absolute install root")
    return value


class StrictOperatorContract(BaseModel):
    """Frozen, strict, extra-forbidding base for O1.2 operator DTOs."""

    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)


# ---------------------------------------------------------------------------
# Source identity
# ---------------------------------------------------------------------------


def source_identity_fingerprint(
    *,
    head_commit: str,
    head_tree: str,
    dirty: bool,
    delta_sha256: str,
) -> str:
    """Frame and hash one ``manyforge.source_identity.v1`` identity.

    The framing is byte-identical to the live qualification collector
    (``scripts/qualification/source_identity.py``) so the product-side
    validator accepts exactly the identities that collector mints.  Only
    this small framing is reused; the collector's Git walk is deliberately
    not imported or duplicated here.
    """
    digest = hashlib.sha256()
    digest.update(f"{SOURCE_IDENTITY_SCHEMA}\0".encode("ascii"))
    fields = (
        (b"head_commit", head_commit.encode("ascii")),
        (b"head_tree", head_tree.encode("ascii")),
        (b"dirty", b"true" if dirty else b"false"),
        (b"delta_sha256", delta_sha256.encode("ascii")),
    )
    for name, payload in fields:
        digest.update(len(name).to_bytes(4, "big"))
        digest.update(name)
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
    return digest.hexdigest()


class SourceIdentityV1(StrictOperatorContract):
    """Exact ``manyforge.source_identity.v1`` fields, fingerprint revalidated."""

    schema_version: Literal[SOURCE_IDENTITY_SCHEMA]
    head_commit: str
    head_tree: str
    dirty: bool
    delta_sha256: str
    fingerprint_sha256: str

    @field_validator("head_commit", "head_tree")
    @classmethod
    def _validate_git_sha(cls, value: str, info) -> str:
        return _bounded_git_sha(value, label=info.field_name)

    @field_validator("delta_sha256", "fingerprint_sha256")
    @classmethod
    def _validate_sha256(cls, value: str, info) -> str:
        return _bounded_sha256(value, label=info.field_name)

    @model_validator(mode="after")
    def _validate_fingerprint(self) -> "SourceIdentityV1":
        expected = source_identity_fingerprint(
            head_commit=self.head_commit,
            head_tree=self.head_tree,
            dirty=self.dirty,
            delta_sha256=self.delta_sha256,
        )
        if self.fingerprint_sha256 != expected:
            raise ValueError("source identity fingerprint is inconsistent")
        return self


# ---------------------------------------------------------------------------
# Configuration building blocks
# ---------------------------------------------------------------------------


class HostIdentity(StrictOperatorContract):
    """One launch host: role, network reference, and boot identity."""

    host_id: str
    role: HostRole
    hostname_or_address: str
    boot_id: str

    @field_validator("host_id")
    @classmethod
    def _validate_host_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="host_id", maximum_bytes=MAX_ID_BYTES)

    @field_validator("hostname_or_address")
    @classmethod
    def _validate_hostname_or_address(cls, value: str) -> str:
        return _bounded_host_reference(value, label="hostname_or_address")

    @field_validator("boot_id")
    @classmethod
    def _validate_boot_id(cls, value: str) -> str:
        return _bounded_boot_id(value, label="boot_id")


class RosTopology(StrictOperatorContract):
    """Recorded ROS discovery facts for the Isaac split topology."""

    domain: Annotated[int, Field(ge=0, le=232)]
    rmw: str
    automatic_discovery_range: str
    static_peers: Annotated[list[str], Field(max_length=MAX_STATIC_PEERS)]

    @field_validator("rmw")
    @classmethod
    def _validate_rmw(cls, value: str) -> str:
        if len(value.encode("utf-8")) > MAX_RMW_BYTES or not _RMW_RE.fullmatch(value):
            raise ValueError(
                "rmw must be one bounded canonical RMW implementation name"
            )
        return value

    @field_validator("automatic_discovery_range")
    @classmethod
    def _validate_discovery_range(cls, value: str) -> str:
        if (
            len(value.encode("utf-8")) > MAX_DISCOVERY_RANGE_BYTES
            or not _DISCOVERY_RANGE_RE.fullmatch(value)
        ):
            raise ValueError(
                "automatic_discovery_range must be one bounded canonical range token"
            )
        return value

    @field_validator("static_peers")
    @classmethod
    def _validate_static_peers(cls, value: list[str]) -> list[str]:
        peers = [_bounded_host_reference(peer, label="static peer") for peer in value]
        if peers != sorted(peers) or len(set(peers)) != len(peers):
            raise ValueError("static_peers must be strictly sorted and unique")
        return peers


class ServiceEndpoint(StrictOperatorContract):
    """One frozen service placement with its operator-observable URL."""

    service: ServiceKind
    bind_host: str
    port: int
    advertised_host: str
    url: str

    @field_validator("advertised_host")
    @classmethod
    def _validate_advertised_host(cls, value: str) -> str:
        return _bounded_host_reference(value, label="advertised_host")

    @field_validator("url")
    @classmethod
    def _validate_url(cls, value: str) -> str:
        if len(value.encode("utf-8")) > MAX_URL_BYTES:
            raise ValueError(f"url exceeds {MAX_URL_BYTES} UTF-8 bytes")
        parts = urlsplit(value)
        if parts.scheme not in ("http", "https"):
            raise ValueError("url must be absolute HTTP(S)")
        if parts.username is not None or parts.password is not None:
            raise ValueError("url must not carry credentials")
        if parts.hostname is None:
            raise ValueError("url must carry an explicit host")
        if parts.port is None:
            raise ValueError("url must carry an explicit port")
        if parts.path != "/":
            raise ValueError("url must use the root path only")
        if parts.query or parts.fragment:
            raise ValueError("url must not carry a query or fragment")
        return value

    @model_validator(mode="after")
    def _validate_frozen_placement(self) -> "ServiceEndpoint":
        expected_bind_host = SERVICE_BIND_HOSTS[self.service.value]
        expected_port = SERVICE_PORTS[self.service.value]
        if self.bind_host != expected_bind_host:
            raise ValueError(
                f"{self.service.value} must bind on the frozen host "
                f"{expected_bind_host}"
            )
        if self.port != expected_port:
            raise ValueError(
                f"{self.service.value} must use the frozen port {expected_port}"
            )
        parts = urlsplit(self.url)
        if parts.port != self.port:
            raise ValueError("url port does not match the frozen service port")
        if parts.hostname != self.advertised_host:
            raise ValueError("url host does not match the advertised host")
        return self


class RuntimeArtifactIdentity(StrictOperatorContract):
    """One source-matched artifact on one host with exactly one substrate."""

    host_id: str
    kind: ArtifactKind
    source_fingerprint: str
    package_aggregate_sha256: str
    compiler_build_manifest_sha256: str
    node_environment_sha256: str
    container_image_id: str | None = None
    install_root: str | None = None

    @field_validator("host_id")
    @classmethod
    def _validate_host_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="host_id", maximum_bytes=MAX_ID_BYTES)

    @field_validator(
        "source_fingerprint",
        "package_aggregate_sha256",
        "compiler_build_manifest_sha256",
        "node_environment_sha256",
    )
    @classmethod
    def _validate_sha256(cls, value: str, info) -> str:
        return _bounded_sha256(value, label=info.field_name)

    @field_validator("container_image_id")
    @classmethod
    def _validate_container_image_id(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_sha256(value, label="container_image_id")

    @field_validator("install_root")
    @classmethod
    def _validate_install_root(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_install_root(value, label="install_root")

    @model_validator(mode="after")
    def _validate_exactly_one_substrate(self) -> "RuntimeArtifactIdentity":
        if self.kind is ArtifactKind.CONTAINER_IMAGE:
            if self.container_image_id is None or self.install_root is not None:
                raise ValueError(
                    "container_image artifact requires exactly one container image ID"
                )
        elif self.install_root is None or self.container_image_id is not None:
            raise ValueError(
                "mounted_install artifact requires exactly one absolute install root"
            )
        return self


# ---------------------------------------------------------------------------
# Launch configuration
# ---------------------------------------------------------------------------


# The exact pre-digest payload keys of one launch configuration: every
# configuration field except ``configuration_digest`` itself.
_CONFIGURATION_PAYLOAD_KEYS = frozenset(
    {
        "schema_version",
        "profile",
        "source",
        "hosts",
        "artifacts",
        "endpoints",
        "ros_topology",
    }
)


def _configuration_preimage(payload: Mapping[str, Any]) -> bytes:
    """Frame one pre-digest configuration payload for hashing.

    The preimage is the configuration schema ID and NUL separator, then
    the 8-byte big-endian length of the RFC 8785 canonical bytes, then
    those bytes.  Only the exact pre-digest field shape is accepted, so
    the digest is never computed over an ambiguous mapping.
    """
    keys = set(payload)
    if keys != _CONFIGURATION_PAYLOAD_KEYS:
        missing = sorted(_CONFIGURATION_PAYLOAD_KEYS - keys)
        unexpected = sorted(keys - _CONFIGURATION_PAYLOAD_KEYS)
        raise ValueError(
            "configuration digest payload must carry exactly the pre-digest "
            f"configuration keys; missing={missing} unexpected={unexpected}"
        )
    try:
        encoded = rfc8785.dumps(dict(payload))
    except (
        rfc8785.CanonicalizationError,
        TypeError,
        UnicodeError,
        ValueError,
    ) as exc:
        raise ValueError(f"configuration is not canonically encodable: {exc}") from exc
    return (
        f"{CONFIGURATION_SCHEMA}\0".encode("ascii")
        + len(encoded).to_bytes(8, "big")
        + encoded
    )


def configuration_digest(payload: Mapping[str, Any] | LaunchConfiguration) -> str:
    """Digest one pre-digest launch-configuration payload.

    Accepts the exact JSON-ready pre-digest mapping (the same mapping
    ``model_dump(mode="json", exclude={"configuration_digest"})`` yields)
    or a validated ``LaunchConfiguration`` for convenience, and returns
    the frozen SHA-256 of the domain-separated preimage.  A caller mints
    a valid configuration by digesting the payload and passing the digest
    to the strict model; no ``model_construct`` or circular bootstrap is
    involved.
    """
    if isinstance(payload, LaunchConfiguration):
        return payload.configuration_digest
    if not isinstance(payload, Mapping):
        raise TypeError(
            "configuration digest payload must be the pre-digest field "
            "mapping or a LaunchConfiguration"
        )
    return hashlib.sha256(_configuration_preimage(payload)).hexdigest()


class LaunchConfiguration(StrictOperatorContract):
    """One complete, self-digesting operator-launch configuration.

    The configuration carries no executable paths or commands beyond the
    mounted install root; it is deployment data, not a launch plan.
    """

    schema_version: Literal[CONFIGURATION_SCHEMA]
    profile: LaunchProfile
    source: SourceIdentityV1
    hosts: Annotated[list[HostIdentity], Field(min_length=1, max_length=MAX_HOSTS)]
    artifacts: Annotated[
        list[RuntimeArtifactIdentity], Field(min_length=1, max_length=MAX_ARTIFACTS)
    ]
    endpoints: Annotated[list[ServiceEndpoint], Field(min_length=3, max_length=3)]
    ros_topology: RosTopology | None
    configuration_digest: str

    @field_validator("configuration_digest")
    @classmethod
    def _validate_configuration_digest(cls, value: str) -> str:
        return _bounded_sha256(value, label="configuration_digest")

    @model_validator(mode="after")
    def _validate_configuration(self) -> "LaunchConfiguration":
        if self.source.dirty:
            raise ValueError("launch configuration rejects a dirty source identity")
        host_ids = [host.host_id for host in self.hosts]
        if host_ids != sorted(host_ids) or len(set(host_ids)) != len(host_ids):
            raise ValueError("hosts must be strictly sorted and unique by host_id")
        roles = {host.role for host in self.hosts}
        if HostRole.RUNTIME not in roles:
            raise ValueError("launch configuration requires one runtime host")
        if self.profile is LaunchProfile.NATIVE:
            if self.ros_topology is not None:
                raise ValueError("native configuration requires ros_topology=None")
        else:
            if self.ros_topology is None:
                raise ValueError("isaac configuration requires a ros topology")
            if HostRole.SIMULATOR not in roles:
                raise ValueError("isaac configuration requires one simulator host")
        artifact_keys = [
            (artifact.host_id, artifact.kind) for artifact in self.artifacts
        ]
        if (
            artifact_keys != sorted(artifact_keys)
            or len(set(artifact_keys)) != len(artifact_keys)
        ):
            raise ValueError(
                "artifacts must be strictly sorted and unique by (host_id, kind)"
            )
        host_id_set = set(host_ids)
        for artifact in self.artifacts:
            if artifact.host_id not in host_id_set:
                raise ValueError("artifact references an unconfigured host")
            if artifact.source_fingerprint != self.source.fingerprint_sha256:
                raise ValueError(
                    "artifact source fingerprint does not match the "
                    "configuration source"
                )
        services = [endpoint.service for endpoint in self.endpoints]
        if (
            services != sorted(services)
            or len(set(services)) != len(services)
            or set(services) != set(ServiceKind)
        ):
            raise ValueError(
                "endpoints must contain exactly one of each service, sorted by service"
            )
        if self.configuration_digest != configuration_digest(
            self.model_dump(mode="json", exclude={"configuration_digest"})
        ):
            raise ValueError("configuration digest is inconsistent")
        return self


# ---------------------------------------------------------------------------
# Owned resource identities and observations
# ---------------------------------------------------------------------------


class ProcessResourceIdentity(StrictOperatorContract):
    """Exact mechanical identity of one owned process.

    A record authorizes no action by itself; the O1.3+ orchestration
    joins it to host boot identity and absence evidence before any stop.
    """

    kind: Literal[ResourceKind.PROCESS]
    resource_id: str
    component_id: str
    host_id: str
    pid: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]
    start_ticks: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]
    boot_id: str
    process_group_id: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)] | None = None
    log_reference: str | None = None

    @field_validator("resource_id", "component_id", "host_id")
    @classmethod
    def _validate_ids(cls, value: str, info) -> str:
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=MAX_ID_BYTES)

    @field_validator("boot_id")
    @classmethod
    def _validate_boot_id(cls, value: str) -> str:
        return _bounded_boot_id(value, label="boot_id")

    @field_validator("log_reference")
    @classmethod
    def _validate_log_reference(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value, label="log_reference", maximum_bytes=MAX_LOG_REFERENCE_BYTES
        )


class ContainerResourceIdentity(StrictOperatorContract):
    """Exact mechanical identity of one owned container workload."""

    kind: Literal[ResourceKind.CONTAINER]
    resource_id: str
    component_id: str
    host_id: str
    container_id: str
    image_id: str
    launch_instance_label: str
    log_reference: str | None = None

    @field_validator("resource_id", "component_id", "host_id")
    @classmethod
    def _validate_ids(cls, value: str, info) -> str:
        return _bounded_nfc(value, label=info.field_name, maximum_bytes=MAX_ID_BYTES)

    @field_validator("container_id", "image_id")
    @classmethod
    def _validate_id_sha256(cls, value: str, info) -> str:
        return _bounded_sha256(value, label=info.field_name)

    @field_validator("launch_instance_label")
    @classmethod
    def _validate_launch_instance_label(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="launch_instance_label", maximum_bytes=MAX_ID_BYTES
        )

    @field_validator("log_reference")
    @classmethod
    def _validate_log_reference(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(
            value, label="log_reference", maximum_bytes=MAX_LOG_REFERENCE_BYTES
        )


# Frozen discriminator union: the exact ``kind`` literal selects the
# branch, so a record can never carry a foreign resource's fields.
OwnedResourceIdentity = Annotated[
    ProcessResourceIdentity | ContainerResourceIdentity,
    Field(discriminator="kind"),
]


class ResourceObservation(StrictOperatorContract):
    """One bounded presence observation of one recorded resource."""

    resource_id: str
    presence: ResourcePresence
    observed_at_unix_ns: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]

    @field_validator("resource_id")
    @classmethod
    def _validate_resource_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="resource_id", maximum_bytes=MAX_ID_BYTES)


class ReadinessObservation(StrictOperatorContract):
    """One owner-issued or externally observed readiness row.

    The launcher records these facts as observed; it never fabricates
    payloads or readiness evidence.
    """

    component_id: str
    authority: ReadinessAuthority
    state: ReadinessState
    source_schema: str
    owner_id: str | None = None
    owner_epoch: str | None = None
    payload_sha256: str
    observed_at_unix_ns: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]
    reason_code: str | None = None

    @field_validator("component_id")
    @classmethod
    def _validate_component_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="component_id", maximum_bytes=MAX_ID_BYTES)

    @field_validator("source_schema")
    @classmethod
    def _validate_source_schema(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="source_schema", maximum_bytes=MAX_SOURCE_SCHEMA_BYTES
        )

    @field_validator("owner_id")
    @classmethod
    def _validate_owner_id(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="owner_id", maximum_bytes=MAX_ID_BYTES)

    @field_validator("owner_epoch")
    @classmethod
    def _validate_owner_epoch(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_nfc(value, label="owner_epoch", maximum_bytes=MAX_ID_BYTES)

    @field_validator("payload_sha256")
    @classmethod
    def _validate_payload_sha256(cls, value: str) -> str:
        return _bounded_sha256(value, label="payload_sha256")

    @field_validator("reason_code")
    @classmethod
    def _validate_reason_code(cls, value: str | None) -> str | None:
        if value is None:
            return None
        return _bounded_reason_code(value, label="reason_code")


class FailureRecord(StrictOperatorContract):
    """One bounded failure fact with one observation time."""

    reason_code: str
    component_id: str
    detail: str
    occurred_at_unix_ns: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]

    @field_validator("reason_code")
    @classmethod
    def _validate_reason_code(cls, value: str) -> str:
        return _bounded_reason_code(value, label="reason_code")

    @field_validator("component_id")
    @classmethod
    def _validate_component_id(cls, value: str) -> str:
        return _bounded_nfc(value, label="component_id", maximum_bytes=MAX_ID_BYTES)

    @field_validator("detail")
    @classmethod
    def _validate_detail(cls, value: str) -> str:
        return _bounded_nfc(value, label="detail", maximum_bytes=MAX_DETAIL_BYTES)


# ---------------------------------------------------------------------------
# Launch instance status
# ---------------------------------------------------------------------------


class LaunchInstanceStatus(StrictOperatorContract):
    """One machine-readable snapshot of one launch instance.

    The record is an observation fact, never an authority: port or
    process presence alone must never be read as semantic health, and a
    ``stopped`` record is only truthful when absence was observed.
    """

    schema_version: Literal[STATUS_SCHEMA]
    launch_instance_id: str
    record_generation: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]
    configuration: LaunchConfiguration
    configuration_digest: str
    state: LaunchInstanceState
    owned_resources: Annotated[
        list[OwnedResourceIdentity], Field(max_length=MAX_OWNED_RESOURCES)
    ]
    resource_observations: Annotated[
        list[ResourceObservation], Field(max_length=MAX_RESOURCE_OBSERVATIONS)
    ]
    readiness_observations: Annotated[
        list[ReadinessObservation], Field(max_length=MAX_READINESS_OBSERVATIONS)
    ]
    last_failure: FailureRecord | None = None
    updated_at_unix_ns: Annotated[int, Field(ge=1, le=MAX_SAFE_INTEGER)]

    @field_validator("launch_instance_id")
    @classmethod
    def _validate_launch_instance_id(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="launch_instance_id", maximum_bytes=MAX_ID_BYTES
        )

    @field_validator("configuration_digest")
    @classmethod
    def _validate_configuration_digest(cls, value: str) -> str:
        return _bounded_sha256(value, label="configuration_digest")

    @model_validator(mode="after")
    def _validate_status(self) -> "LaunchInstanceStatus":
        if self.configuration_digest != self.configuration.configuration_digest:
            raise ValueError(
                "status configuration digest does not agree with the configuration"
            )
        resource_ids = [resource.resource_id for resource in self.owned_resources]
        if (
            resource_ids != sorted(resource_ids)
            or len(set(resource_ids)) != len(resource_ids)
        ):
            raise ValueError(
                "owned resources must be strictly sorted and unique by resource_id"
            )
        hosts = {host.host_id: host for host in self.configuration.hosts}
        image_ids_by_host = {
            host_id: {
                artifact.container_image_id
                for artifact in self.configuration.artifacts
                if artifact.host_id == host_id
                and artifact.kind is ArtifactKind.CONTAINER_IMAGE
            }
            for host_id in hosts
        }
        for resource in self.owned_resources:
            host = hosts.get(resource.host_id)
            if host is None:
                raise ValueError("owned resource references an unconfigured host")
            if isinstance(resource, ProcessResourceIdentity):
                if resource.boot_id != host.boot_id:
                    raise ValueError(
                        "process boot identity does not match its configured host"
                    )
            elif resource.launch_instance_label != self.launch_instance_id:
                raise ValueError(
                    "container launch-instance label does not match the instance"
                )
            elif resource.image_id not in image_ids_by_host[resource.host_id]:
                raise ValueError(
                    "container image identity does not match a configured artifact"
                )
        observation_ids = [
            observation.resource_id for observation in self.resource_observations
        ]
        if (
            observation_ids != sorted(observation_ids)
            or len(set(observation_ids)) != len(observation_ids)
        ):
            raise ValueError(
                "resource observations must be strictly sorted and "
                "unique by resource_id"
            )
        if observation_ids and observation_ids != resource_ids:
            raise ValueError(
                "resource observations must cover every recorded resource exactly"
            )
        readiness_keys = [
            (row.component_id, row.authority, row.source_schema)
            for row in self.readiness_observations
        ]
        if (
            readiness_keys != sorted(readiness_keys)
            or len(set(readiness_keys)) != len(readiness_keys)
        ):
            raise ValueError(
                "readiness observations must be strictly sorted and unique"
            )
        presence_by_resource = {
            observation.resource_id: observation.presence
            for observation in self.resource_observations
        }
        readiness_states = [row.state for row in self.readiness_observations]
        if self.state is LaunchInstanceState.READY:
            if not self.resource_observations or any(
                presence is not ResourcePresence.PRESENT
                for presence in presence_by_resource.values()
            ):
                raise ValueError(
                    "ready status requires every recorded resource observed present"
                )
            if not readiness_states or any(
                state is not ReadinessState.READY for state in readiness_states
            ):
                raise ValueError(
                    "ready status requires at least one readiness row, all ready"
                )
        elif self.state is LaunchInstanceState.STOPPED:
            if resource_ids and (
                not self.resource_observations
                or any(
                    presence is not ResourcePresence.ABSENT
                    for presence in presence_by_resource.values()
                )
            ):
                raise ValueError(
                    "stopped status requires every recorded resource observed absent"
                )
            if any(state is ReadinessState.READY for state in readiness_states):
                raise ValueError("stopped status requires no readiness row ready")
        return self


# ---------------------------------------------------------------------------
# Operator command result
# ---------------------------------------------------------------------------


class OperatorCommandError(StrictOperatorContract):
    """One typed operator-command failure."""

    code: str
    message: str

    @field_validator("code")
    @classmethod
    def _validate_code(cls, value: str) -> str:
        return _bounded_reason_code(value, label="code")

    @field_validator("message")
    @classmethod
    def _validate_message(cls, value: str) -> str:
        return _bounded_nfc(value, label="message", maximum_bytes=MAX_DETAIL_BYTES)


_SUCCESS_OUTCOMES = frozenset(
    {
        CommandOutcome.STARTED,
        CommandOutcome.ALREADY_RUNNING,
        CommandOutcome.REPORTED,
        CommandOutcome.STOPPING,
        CommandOutcome.STOPPED,
        CommandOutcome.ALREADY_STOPPED,
    }
)
_ERROR_REQUIRED_OUTCOMES = frozenset(
    {CommandOutcome.CONFLICT, CommandOutcome.FAILED}
)


class OperatorCommandResult(StrictOperatorContract):
    """One bounded operator-command outcome.

    A truthful status exists only after a launch has started: success-
    like outcomes carry exactly one status and no error, while
    conflict/failed outcomes carry a typed error.
    """

    schema_version: Literal[RESULT_SCHEMA]
    operation: LaunchOperation
    outcome: CommandOutcome
    status: LaunchInstanceStatus | None = None
    error: OperatorCommandError | None = None

    @model_validator(mode="after")
    def _validate_outcome(self) -> "OperatorCommandResult":
        if self.outcome in _SUCCESS_OUTCOMES:
            if self.status is None:
                raise ValueError(
                    f"{self.outcome.value} outcome requires a launch instance status"
                )
            if self.error is not None:
                raise ValueError(
                    f"{self.outcome.value} outcome cannot carry an error"
                )
        elif self.outcome in _ERROR_REQUIRED_OUTCOMES:
            if self.error is None:
                raise ValueError(
                    f"{self.outcome.value} outcome requires a typed error"
                )
        else:
            raise ValueError("unknown command outcome")
        return self


_ContractDocument = Annotated[
    LaunchConfiguration | LaunchInstanceStatus | OperatorCommandResult,
    Field(discriminator="schema_version"),
]

_CONTRACT_DOCUMENT_ADAPTER = TypeAdapter(_ContractDocument)


# ---------------------------------------------------------------------------
# Pure decisions
# ---------------------------------------------------------------------------

_ALLOWED_TRANSITIONS: dict[
    LaunchInstanceState, frozenset[LaunchInstanceState]
] = {
    LaunchInstanceState.UNAVAILABLE: frozenset(
        {LaunchInstanceState.STARTING, LaunchInstanceState.STOPPED}
    ),
    LaunchInstanceState.STARTING: frozenset(
        {
            LaunchInstanceState.READY,
            LaunchInstanceState.DEGRADED,
            LaunchInstanceState.STOPPING,
        }
    ),
    LaunchInstanceState.READY: frozenset(
        {LaunchInstanceState.DEGRADED, LaunchInstanceState.STOPPING}
    ),
    LaunchInstanceState.DEGRADED: frozenset(
        {
            LaunchInstanceState.STARTING,
            LaunchInstanceState.READY,
            LaunchInstanceState.STOPPING,
        }
    ),
    LaunchInstanceState.STOPPING: frozenset(
        {LaunchInstanceState.STOPPED, LaunchInstanceState.DEGRADED}
    ),
    LaunchInstanceState.STOPPED: frozenset({LaunchInstanceState.STARTING}),
}


def _as_instance_state(
    value: LaunchInstanceState | str,
) -> LaunchInstanceState:
    if isinstance(value, LaunchInstanceState):
        return value
    if isinstance(value, str):
        try:
            return LaunchInstanceState(value)
        except ValueError as exc:
            raise ValueError(
                f"unknown launch instance state: {value!r}"
            ) from exc
    raise TypeError("previous and next must be LaunchInstanceState values")


def can_transition(
    previous: LaunchInstanceState | str,
    next: LaunchInstanceState | str,  # noqa: A002 - frozen O1.2 signature
) -> bool:
    """Pure lifecycle gate for the O1 operator-launch state machine.

    Self-transitions are always permitted.  ``stopping -> stopped`` is
    only lawful when the status DTO proves every recorded resource
    absent: a failed cleanup must be observed (or stay ``degraded``),
    never jump straight to ``stopped``.
    """
    previous_state = _as_instance_state(previous)
    next_state = _as_instance_state(next)
    if previous_state is next_state:
        return True
    return next_state in _ALLOWED_TRANSITIONS[previous_state]


def _all_recorded_resources_absent(status: LaunchInstanceStatus) -> bool:
    resource_ids = [resource.resource_id for resource in status.owned_resources]
    if not resource_ids:
        return True
    observation_ids = [
        observation.resource_id for observation in status.resource_observations
    ]
    if observation_ids != resource_ids:
        return False
    return all(
        observation.presence is ResourcePresence.ABSENT
        for observation in status.resource_observations
    )


def decide_start(
    existing_status_or_none: LaunchInstanceStatus | None,
    requested_configuration_digest: str,
) -> StartDisposition:
    """Pure start disposition for one requested launch configuration.

    No record means create.  The same configuration is idempotent while
    starting/ready, recovered from degraded/unavailable, and conflicts
    while stopping.  A stopped record is replaced by a new launch; a
    different configuration may create only over a stopped record whose
    recorded resources are all observed absent.
    """
    if not _SHA256_RE.fullmatch(requested_configuration_digest):
        raise ValueError(
            "requested_configuration_digest must be one full lowercase SHA-256"
        )
    if existing_status_or_none is None:
        return StartDisposition.CREATE
    status = existing_status_or_none
    if status.configuration_digest == requested_configuration_digest:
        if status.state in (
            LaunchInstanceState.STARTING,
            LaunchInstanceState.READY,
        ):
            return StartDisposition.IDEMPOTENT
        if status.state in (
            LaunchInstanceState.DEGRADED,
            LaunchInstanceState.UNAVAILABLE,
        ):
            return StartDisposition.RECOVER
        if status.state is LaunchInstanceState.STOPPING:
            return StartDisposition.CONFLICT
        return StartDisposition.CREATE
    if status.state is not LaunchInstanceState.STOPPED:
        return StartDisposition.CONFLICT
    if _all_recorded_resources_absent(status):
        return StartDisposition.CREATE
    return StartDisposition.CONFLICT


# ---------------------------------------------------------------------------
# Contract schema
# ---------------------------------------------------------------------------


def _contract_vocabulary() -> dict[str, Any]:
    """Build the single bounded ``x-manyforge-contract`` payload."""
    return {
        "configuration_schema": CONFIGURATION_SCHEMA,
        "status_schema": STATUS_SCHEMA,
        "result_schema": RESULT_SCHEMA,
        "source_identity_schema": SOURCE_IDENTITY_SCHEMA,
        "operations": sorted(item.value for item in LaunchOperation),
        "profiles": sorted(item.value for item in LaunchProfile),
        "host_roles": sorted(item.value for item in HostRole),
        "service_kinds": sorted(item.value for item in ServiceKind),
        "instance_states": sorted(item.value for item in LaunchInstanceState),
        "readiness_states": sorted(item.value for item in ReadinessState),
        "resource_presence": sorted(item.value for item in ResourcePresence),
        "readiness_authorities": sorted(item.value for item in ReadinessAuthority),
        "artifact_kinds": sorted(item.value for item in ArtifactKind),
        "resource_kinds": sorted(item.value for item in ResourceKind),
        "command_outcomes": sorted(item.value for item in CommandOutcome),
        "start_dispositions": sorted(item.value for item in StartDisposition),
        "success_outcomes": sorted(item.value for item in _SUCCESS_OUTCOMES),
        "error_required_outcomes": sorted(
            item.value for item in _ERROR_REQUIRED_OUTCOMES
        ),
        "service_endpoints": {
            service.value: {
                "bind_host": SERVICE_BIND_HOSTS[service.value],
                "port": SERVICE_PORTS[service.value],
            }
            for service in ServiceKind
        },
        "allowed_transitions": {
            previous.value: sorted(
                {previous.value, *(item.value for item in allowed)}
            )
            for previous, allowed in _ALLOWED_TRANSITIONS.items()
        },
    }


def operator_launch_contract_schema() -> dict[str, Any]:
    """Return the closed, deterministic O1.2 operator-launch JSON Schema.

    One TypeAdapter emits the complete discriminated document graph, so
    its top-level ``oneOf``, discriminator, definitions, and local refs
    remain coherent. The frozen identifiers and bounded vocabulary are
    added deterministically without writing a generated file.
    """
    generated = _CONTRACT_DOCUMENT_ADAPTER.json_schema()
    return {
        "$schema": JSON_SCHEMA_DRAFT_2020_12,
        "$id": LAUNCH_CONTRACT_SCHEMA,
        "title": "ManyForge operator-launch contract",
        **generated,
        "x-manyforge-contract": _contract_vocabulary(),
    }
