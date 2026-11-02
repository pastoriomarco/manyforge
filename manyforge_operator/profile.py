"""Frozen O1.2 profile-adapter protocol (internal, not a public SDK surface).

This module is the narrow seam between the common operator-launch
orchestration (O1.3+) and the standard/Isaac profile adapters.  It defines
only typed request/result DTOs and protocols:

* the common orchestration owns the lock, the status record, durable
  persistence, and the exact fallback policy;
* an adapter may only acquire profile resources while immediately
  recording their exact identities through the recorder callback;
* an adapter returns copied observations and may request graceful
  semantic/process stop;
* an adapter never owns Apply, Run, graph, scene, alarm, or health truth,
  and this module contains no implementation, subprocess, container,
  socket, HTTP, filesystem, or timing code.

It is intentionally not re-exported from ``manyforge_operator``: the
public package exposes the contract DTOs, enums, schema, and pure
decisions only.
"""

from __future__ import annotations

from typing import Annotated, Protocol

from pydantic import Field, field_validator

from .contracts import (
    MAX_ID_BYTES,
    MAX_OWNED_RESOURCES,
    MAX_READINESS_OBSERVATIONS,
    MAX_RESOURCE_OBSERVATIONS,
    StrictOperatorContract,
    _bounded_nfc,
    FailureRecord,
    LaunchConfiguration,
    LaunchInstanceState,
    OwnedResourceIdentity,
    ReadinessObservation,
    ResourceObservation,
)


class _ProfileInstanceScoped(StrictOperatorContract):
    """Fields shared by every profile request DTO."""

    launch_instance_id: str

    @field_validator("launch_instance_id")
    @classmethod
    def _validate_launch_instance_id(cls, value: str) -> str:
        return _bounded_nfc(
            value, label="launch_instance_id", maximum_bytes=MAX_ID_BYTES
        )


class ProfileStartRequest(_ProfileInstanceScoped):
    """Common-orchestration request to start one profile launch.

    Carries the exact validated configuration: the configuration digest
    and the launch profile are read from it, never repeated here.
    """

    configuration: LaunchConfiguration


class ProfileStartResult(_ProfileInstanceScoped):
    """Copied start-time facts; identities are recorded via the callback."""

    state: LaunchInstanceState
    resource_observations: Annotated[
        list[ResourceObservation], Field(max_length=MAX_RESOURCE_OBSERVATIONS)
    ]
    readiness_observations: Annotated[
        list[ReadinessObservation], Field(max_length=MAX_READINESS_OBSERVATIONS)
    ]
    failure: FailureRecord | None = None


class ProfileObserveRequest(_ProfileInstanceScoped):
    """Common-orchestration request for one copied observation snapshot."""

    owned_resources: Annotated[
        list[OwnedResourceIdentity], Field(max_length=MAX_OWNED_RESOURCES)
    ]


class ProfileObserveResult(_ProfileInstanceScoped):
    """One copied observation snapshot of the running profile."""

    state: LaunchInstanceState
    resource_observations: Annotated[
        list[ResourceObservation], Field(max_length=MAX_RESOURCE_OBSERVATIONS)
    ]
    readiness_observations: Annotated[
        list[ReadinessObservation], Field(max_length=MAX_READINESS_OBSERVATIONS)
    ]
    failure: FailureRecord | None = None


class ProfileGracefulStopRequest(_ProfileInstanceScoped):
    """Common-orchestration request to stop only the recorded resources."""

    owned_resources: Annotated[
        list[OwnedResourceIdentity], Field(max_length=MAX_OWNED_RESOURCES)
    ]


class ProfileGracefulStopResult(_ProfileInstanceScoped):
    """Copied stop-time facts after the graceful stop request."""

    state: LaunchInstanceState
    failure: FailureRecord | None = None


class OwnedResourceRecorder(Protocol):
    """Callback used to record one exact identity at acquisition time."""

    def __call__(self, identity: OwnedResourceIdentity) -> None:
        """Record one freshly acquired owned-resource identity."""


class ProfileAdapter(Protocol):
    """Protocol boundary for standard/Isaac profile adapters.

    No implementation is provided at this slice; the common
    orchestration selects and drives one qualified adapter per profile.
    """

    def start(
        self,
        request: ProfileStartRequest,
        recorder: OwnedResourceRecorder,
    ) -> ProfileStartResult:
        """Acquire this profile's resources, recording identities as acquired."""

    def observe(self, request: ProfileObserveRequest) -> ProfileObserveResult:
        """Return one copied observation snapshot of the running profile."""

    def request_graceful_stop(
        self, request: ProfileGracefulStopRequest
    ) -> ProfileGracefulStopResult:
        """Request graceful stop of only the recorded owned resources."""
