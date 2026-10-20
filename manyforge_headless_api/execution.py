"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

import hashlib
import logging
import math
import re
import socket
import sys
import threading
import time
from contextlib import AbstractContextManager
from pathlib import Path
from typing import Any, Callable, Protocol
import unicodedata

import rfc8785

from .models import (
    MAX_IDENTITY_BYTES,
    FullStopIntentRequest,
    GripperFeedbackObservation,
    IoSignalObservation,
    IoSourceCursor,
    MockIoInputRequest,
    MockIoPublicationError,
    OperationControlRequest,
    OperationRecoveryActionRequest,
    OperationIntentRequest,
    ManualProgramHandover,
    ManualControlAvailability,
    Pose3Request,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    ProgramOperationQuery,
    RuntimeScaleControlRequest,
    OperationRuntimeScaleRequest,
    RuntimeScaleControlSelectorRequest,
    SHA256_PATTERN,
    SceneRevisionRequest,
    GlobalIoRuleRecoveryIntentRequest,
    GlobalIoRuleRecoverySelector,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from .cclc import MAX_INCIDENT_EVENT_PAGE
from .global_rule_status import STATE_EVENT_KIND, GlobalRuleStatusModel
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object
from manyforge_behavior.json_scalars import nonnegative_int64, nonnegative_int64_value
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_CONTROL_SOCKET_FILE = "execution-control.sock"
EXECUTION_OBSERVATION_SOCKET_FILE = "execution-observation.sock"
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v1"
VOLATILE_MOTION_TRACE_VERSION = "manyforge.volatile_motion_trace.v1"
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
OPERATION_EVENT_KINDS = frozenset(
    {
        "operation_accepted",
        "control_accepted",
        "stop_acceptance_projected",
        "stop_terminal_projected",
        "stop_fault_cleared",
        "runtime_scale_accepted",
        "operation_prepared",
        "state_observed",
        "host_replaced",
        # Kernel global rule state rows (contract 64): folded by the global
        # rule status projection and ingested by the historian unchanged.
        STATE_EVENT_KIND,
    }
)


def _valid_operation_event_identity(event: dict[str, Any]) -> bool:
    operation_id = event.get("operation_id")
    operation_revision = event.get("operation_revision")
    if event.get("record_kind") in {
        "stop_acceptance_projected",
        "stop_fault_cleared",
    }:
        request_id = event.get("request_id")
        return (
            operation_id is None
            and operation_revision is None
            and isinstance(request_id, str)
            and bool(request_id)
        )
    if event.get("record_kind") in {"stop_terminal_projected", STATE_EVENT_KIND}:
        return (
            operation_id is None
            and operation_revision is None
            and event.get("request_id") is None
        )
    return (
        isinstance(operation_id, str)
        and bool(operation_id)
        and type(operation_revision) is int
        and 1 <= operation_revision <= MAXIMUM_SAFE_INTEGER
    )


_RETRY_ELIGIBILITY_KEYS = frozenset({"eligible", "cause", "effect", "reason"})
_RETRY_ELIGIBILITY_CAUSES = frozenset(
    {"planning_no_effect", "collision_stop_nominal", "not_eligible"}
)
_RETRY_ELIGIBILITY_EFFECTS = frozenset(
    {"known_no_effect", "effect_observed", "effect_uncertain"}
)
RETRY_ELIGIBILITY_REASONS = frozenset(
    {
        "none",
        "not_retryable_state",
        "failure_cause_not_retryable",
        "dispatch_absence_unproven",
        "effect_uncertain",
        "effect_observed",
        "validation_not_conclusive_collision",
        "stop_owner_active",
        "stronger_authority_active",
        "queued_latch_present",
        "latch_present",
        "stop_not_settled",
        "stop_not_nominal",
        "stop_not_controlled",
        "observation_not_qualified",
        "collision_not_conclusive",
        "operation_in_flight",
        "program_scope_required",
    }
)


def _valid_retry_eligibility(value: object) -> bool:
    return (
        isinstance(value, dict)
        and set(value) == _RETRY_ELIGIBILITY_KEYS
        and type(value["eligible"]) is bool
        and isinstance(value["cause"], str)
        and value["cause"] in _RETRY_ELIGIBILITY_CAUSES
        and isinstance(value["effect"], str)
        and value["effect"] in _RETRY_ELIGIBILITY_EFFECTS
        and isinstance(value["reason"], str)
        and value["reason"] in RETRY_ELIGIBILITY_REASONS
    )


MAX_EXECUTION_PACKET_BYTES = 1024 * 1024
MAX_EXECUTION_RESPONSE_BYTES = 4 * 1024 * 1024
# Mirrors the native operation journal's closed maximum. Released operations
# are removed as their control event is ingested, so this is a concurrent
# owner-state bound rather than lifetime retention.
MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS = 4096
# Operation states in which the target no longer moves under this operation.
_OPERATION_REST_STATES = frozenset(
    {
        "completed",
        "stopped",
        "failed",
        "interrupted",
        "intervention_required",
        "cancelled_pre_dispatch",
    }
)
MOTION_PREVIEW_RESPONSE_KEYS = frozenset(
    {"api_version", "command", "ok", "preview_receipt"}
)
MOTION_PREVIEW_RECEIPT_KEYS = frozenset(
    {
        "collision_world_revision",
        "commit_eligible",
        "component_role_id",
        "final_positions_rad",
        "final_tcp_pose",
        "graph_revision",
        "intent_digest",
        "program_binding_id",
        "program_id",
        "program_revision",
        "program_run_id",
        "receipt_digest",
        "request_id",
        "request_owner_epoch",
        "required_capability_id",
        "retained_bundle_id",
        "scene_revision",
        "schema_version",
        "source_joint_names",
        "start_positions_rad",
        "target_role_id",
    }
)
MOTION_PREVIEW_COLLISION_REVISION_KEYS = frozenset({"content_hash", "revision_id"})
CAPABILITY_COMMAND_PREVIEW_RESPONSE_KEYS = frozenset(
    {"api_version", "command", "ok", "preview_receipt"}
)
CAPABILITY_COMMAND_PREVIEW_RECEIPT_KEYS = frozenset(
    {
        "command_schema_id",
        "command_schema_revision",
        "commit_eligible",
        "component_role_id",
        "coordinate_kind",
        "graph_revision",
        "intent_digest",
        "max_position",
        "min_position",
        "program_binding_id",
        "program_id",
        "program_revision",
        "program_run_id",
        "receipt_digest",
        "request_id",
        "request_owner_epoch",
        "required_capability_id",
        "retained_bundle_id",
        "scene_revision",
        "schema_version",
        "source_coordinate_name",
        "source_kind",
        "target_position",
        "target_role_id",
    }
)


class ExecutionPort(Protocol):
    def diagnostics(self) -> dict[str, Any]: ...

    def identity(self) -> dict[str, Any]: ...

    def targets(self) -> dict[str, Any]: ...

    def control_state(self) -> dict[str, Any]: ...

    def query_target(self, request: TargetObservationQuery) -> dict[str, Any]: ...

    def set_mock_io_input(self, request: MockIoInputRequest) -> dict[str, Any]: ...

    def observation(self) -> dict[str, Any]: ...

    def open_observation_session(self) -> "ExecutionObservationSession": ...

    def submit(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def preview_motion(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def read_manual_handover(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def preview_capability_command(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def read(self, operation_id: str) -> dict[str, Any]: ...

    def plan_pick(
        self,
        request: PickSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def plan_place(
        self,
        request: PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def reset(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def acknowledge_completed(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def stop(
        self,
        request: StopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def full_stop(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def recognized_malformed_stop(
        self,
        *,
        request_id: str,
        recognized_route: str,
        reason: str,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def stop_fault(self) -> dict[str, Any]: ...

    def recover_stop_fault(
        self,
        request: StopFaultRecoveryIntentRequest | GlobalIoRuleRecoveryIntentRequest,
    ) -> dict[str, Any]: ...

    def set_runtime_scale_control(
        self,
        request: RuntimeScaleControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def get_runtime_scale_control(
        self, selector: RuntimeScaleControlSelectorRequest
    ) -> dict[str, Any]: ...

    def events(
        self,
        owner_epoch: str,
        after_sequence: int,
        *,
        limit: int = 16,
    ) -> dict[str, Any]: ...

    def read_volatile_motion_trace(self, allowance_bytes: int) -> dict[str, Any]: ...


def _motion_preview_scene_revision_matches(
    observed: object, expected: dict[str, Any]
) -> bool:
    """Return whether a motion receipt names the intent's Scene or a later one.

    The native owner may latch ``before_planning`` observations and compute the
    preview on a newer owner-minted revision of the same Scene. The receipt is
    explanatory evidence; approval still requires the current revision.
    """
    if observed == expected:
        return True
    try:
        parsed = SceneRevisionRequest.model_validate(observed)
    except ValueError:
        return False
    return (
        parsed.model_dump(mode="json") == observed
        and parsed.scene_id == expected["scene_id"]
        and parsed.revision_id > expected["revision_id"]
    )


def _canonical_json(value: dict[str, Any]) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            "execution_command_invalid",
            f"execution command is not canonically encodable: {exc}",
        ) from exc


def _component_condition_facts(snapshot: dict[str, Any]) -> dict[str, Any]:
    """The CCLC snapshot members the lifecycle-cause alarm adapter reads.

    Wait timing (`waiting_since_ns`, `delayed`) is presentation only and is
    not forwarded, so it neither changes a condition nor crosses the native
    boundary as an unsafe integer. An open incident crosses with its
    identity, cause, vendor codes and summary; its detection time stays in
    the incident record.
    """

    try:
        progress = snapshot.get("lifecycle_progress")
        incident = None if progress is None else progress.get("incident")
        return {
            "component_lifecycle_state": snapshot["component_lifecycle_state"],
            "controllable_component_id": snapshot["controllable_component_id"],
            "domains": [
                {
                    "control_domain_id": domain["control_domain_id"],
                    # Core `resource_ids()` for either domain kind.
                    "resources": list(domain["resources"]),
                }
                for domain in snapshot["domains"]
            ],
            "graph_revision": snapshot["graph_revision"],
            "lifecycle_progress": (
                None
                if progress is None
                else {
                    **{
                        key: progress[key]
                        for key in ("blocking_cause_class", "blocking_reason", "phase")
                    },
                    "incident": (
                        None
                        if incident is None
                        else {
                            key: incident[key]
                            for key in (
                                "cause_class",
                                "incident_id",
                                "summary",
                                "vendor_codes",
                            )
                        }
                    ),
                }
            ),
            "lifecycle_target": snapshot.get("lifecycle_target"),
            "retained_bundle_id": snapshot["retained_bundle_id"],
        }
    except (KeyError, TypeError) as exc:
        raise HeadlessApiError(
            "cclc_open_response_invalid",
            f"CCLC component snapshot lacks a condition field: {exc}",
        ) from exc


def _decode_execution_response(payload: bytes, flags: int) -> dict[str, Any]:
    if flags & socket.MSG_TRUNC or not payload:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response is empty or oversized",
        )
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response is not UTF-8",
        ) from exc
    response = strict_json_object(text, label="execution command response")
    try:
        canonical_response = rfc8785.dumps(response)
    except (
        rfc8785.CanonicalizationError,
        UnicodeError,
        ValueError,
    ) as exc:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response is not canonically encodable",
        ) from exc
    if canonical_response != payload:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response is not canonical JSON",
        )
    if response.get("api_version") != EXECUTION_RESPONSE_VERSION:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response version is unsupported",
        )
    if type(response.get("ok")) is not bool:
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "execution response has no boolean result",
        )
    return response


class ExecutionObservationSession(
    AbstractContextManager["ExecutionObservationSession"]
):
    """One demand-driven native observation session; no timer or state cache."""

    def __init__(
        self,
        run_root: PinnedRunRoot,
        socket_path: Path,
    ) -> None:
        self._run_root = run_root
        self._socket_path = socket_path
        self._address_context: Any | None = None
        self._socket: socket.socket | None = None
        self._close_lock = threading.Lock()

    def __enter__(self) -> "ExecutionObservationSession":
        if self._socket is not None:
            raise RuntimeError("execution observation session is already open")
        try:
            context = self._run_root.socket_address(self._socket_path.name)
            socket_address = context.__enter__()
            client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            client.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_RCVBUF,
                MAX_EXECUTION_RESPONSE_BYTES,
            )
            # A short receive timeout only makes cancellation observable. It
            # never causes another request or owner read.
            client.settimeout(0.25)
            client.connect(socket_address)
        except Exception:
            if "client" in locals():
                client.close()
            if "context" in locals():
                context.__exit__(*sys.exc_info())
            raise
        self._address_context = context
        self._socket = client
        return self

    def close(self) -> None:
        with self._close_lock:
            client = self._socket
            self._socket = None
        if client is not None:
            client.close()
        context = self._address_context
        self._address_context = None
        if context is not None:
            context.__exit__(None, None, None)

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def next(
        self,
        cursor: dict[str, Any] | None,
        cancelled: threading.Event,
    ) -> dict[str, Any]:
        if cursor is not None and (
            set(cursor) != {"owner_epoch", "source_epoch", "source_sequence"}
            or not isinstance(cursor.get("owner_epoch"), str)
            or not cursor["owner_epoch"]
            or not isinstance(cursor.get("source_epoch"), str)
            or not cursor["source_epoch"]
            or type(cursor.get("source_sequence")) is not int
            or not 0 <= cursor["source_sequence"] <= MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "execution_observation_cursor_invalid",
                "execution observation cursor is invalid",
            )
        command = {
            "api_version": EXECUTION_COMMAND_VERSION,
            "after_owner_epoch": None if cursor is None else cursor["owner_epoch"],
            "after_source_epoch": None if cursor is None else cursor["source_epoch"],
            "after_source_sequence": (
                None if cursor is None else cursor["source_sequence"]
            ),
            "command": "observation",
        }
        encoded = _canonical_json(command)
        client = self._socket
        if client is None:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution observation session is closed",
            )
        try:
            sent = client.send(encoded)
            if sent != len(encoded):
                raise HeadlessApiError(
                    "execution_command_transport_failure",
                    "execution observation request was truncated",
                )
            while not cancelled.is_set():
                try:
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAX_EXECUTION_RESPONSE_BYTES
                    )
                    response = _decode_execution_response(payload, flags)
                    if response.get("ok") is True and (
                        response.get("command") != "observation"
                        or not isinstance(response.get("observation"), dict)
                    ):
                        raise HeadlessApiError(
                            "execution_command_response_invalid",
                            "native execution observation response is invalid",
                        )
                    return response
                except socket.timeout:
                    continue
        except HeadlessApiError:
            raise
        except (OSError, TimeoutError) as exc:
            if cancelled.is_set():
                raise HeadlessApiError(
                    "execution_observation_cancelled",
                    "execution observation demand ended",
                ) from exc
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution observation transport failed: {exc}",
            ) from exc
        raise HeadlessApiError(
            "execution_observation_cancelled",
            "execution observation demand ended",
        )


def _sparse_request_with_complete_context(request: Any) -> dict[str, Any]:
    """Project the public request context accepted by the native owner.

    Stop selectors deliberately omit unused selector kinds and an absent
    reason.  Session/client sequence is Program-cycle audit metadata and is
    consumed before this native boundary; the native request context remains
    the closed four-field DTO, including a nullable correlation identity.
    """

    payload = request.model_dump(mode="json", exclude_none=True)
    payload["request_context"] = request.request_context.model_dump(
        mode="json",
        include={
            "request_id",
            "request_owner_epoch",
            "source_label",
            "correlation_id",
        },
    )
    return payload


class ExecutionCommandClient:
    """Forward intents and observations without caching execution truth."""

    def __init__(
        self,
        run_root: Path,
        *,
        timeout_seconds: float = 10.0,
        attribution_transport: str = "http",
    ) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        if attribution_transport not in {"http", "internal"}:
            raise ValueError("execution attribution transport is unsupported")
        try:
            self._run_root = PinnedRunRoot(run_root)
        except PrivateTransportCapabilityError as error:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution run capability is invalid: {error}",
            ) from error
        self._socket_path = self._run_root.path / EXECUTION_SOCKET_FILE
        self._control_socket_path = self._run_root.path / EXECUTION_CONTROL_SOCKET_FILE
        self._observation_socket_path = (
            self._run_root.path / EXECUTION_OBSERVATION_SOCKET_FILE
        )
        self._timeout_seconds = timeout_seconds
        self._attribution_transport = attribution_transport

    def _exchange_on(
        self, command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_EXECUTION_PACKET_BYTES:
            raise HeadlessApiError(
                "execution_command_oversized",
                "execution command exceeds its byte bound",
            )
        try:
            with self._run_root.socket_address(socket_path.name) as socket_address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAX_EXECUTION_RESPONSE_BYTES,
                    )
                    client.settimeout(self._timeout_seconds)
                    client.connect(socket_address)
                    sent = client.send(encoded)
                    if sent != len(encoded):
                        raise HeadlessApiError(
                            "execution_command_transport_failure",
                            "execution command packet was truncated",
                        )
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAX_EXECUTION_RESPONSE_BYTES
                    )
        except HeadlessApiError:
            raise
        except PrivateTransportCapabilityError as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution command socket is unavailable: {exc}",
            ) from exc
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution transport failed: {exc}",
            ) from exc
        return _decode_execution_response(payload, flags)

    def stop_program_run(
        self,
        request: ProgramRunStopIntentRequest,
    ) -> dict[str, Any]:
        """Submit the private Program-owner selector on the Stop lane."""

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "Program-run Stop is available only to the internal owner transport",
            )
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(None),
                "command": "stop_program_run",
                "request": _sparse_request_with_complete_context(request),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "stop_program_run"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Program-run Stop acceptance does not match its request",
            )
        return response

    def release_program_run_stop_fence(
        self,
        program_run_id: str,
        stop_id: str,
        stop_generation: int,
    ) -> dict[str, Any]:
        """Reopen one exact internally owned Program-run Stop boundary."""

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "Program-run Stop release is available only to the internal owner transport",
            )
        for label, value in (
            ("Program-run identity", program_run_id),
            ("Stop identity", stop_id),
        ):
            if (
                not isinstance(value, str)
                or not value
                or unicodedata.normalize("NFC", value) != value
                or len(value.encode("utf-8")) > 2_048
            ):
                raise ValueError(f"{label} must be bounded non-empty NFC text")
        if (
            type(stop_generation) is not int
            or not 1 <= stop_generation <= MAXIMUM_SAFE_INTEGER
        ):
            raise ValueError("Stop generation is outside its safe bound")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(None),
                "command": "release_program_run_stop_fence",
                "program_run_id": program_run_id,
                "stop_generation": stop_generation,
                "stop_id": stop_id,
            }
        )
        if response.get("ok") is True and response.get("command") != (
            "release_program_run_stop_fence"
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Program-run Stop release response changed its command identity",
            )
        return response

    def stop_status(self, stop_id: str) -> dict[str, Any]:
        """Read one exact aggregate Stop result for the internal Program owner.

        Served beside Stop on the independent control socket, so Stop
        settlement never waits behind ordinary or planning traffic.
        """

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "Stop-status read is available only to the internal owner transport",
            )
        if (
            not stop_id
            or unicodedata.normalize("NFC", stop_id) != stop_id
            or len(stop_id.encode("utf-8")) > 2_048
        ):
            raise ValueError("Stop identity must be bounded non-empty NFC text")
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(None),
                "command": "read_stop",
                "stop_id": stop_id,
            }
        )
        if response.get("ok") is not True:
            return response
        status = response.get("stop_status")
        if (
            response.get("command") != "read_stop"
            or not isinstance(status, dict)
            or set(status) - {"terminal_projection"}
            != {
                "request_id",
                "schema_version",
                "state",
                "stop_generation",
                "stop_id",
            }
            # The additive forensic projection state (SX-2226): one string.
            or status.get("terminal_projection", "pending")
            not in ("pending", "recorded", "failed")
            or status.get("schema_version") != "manyforge.execution_stop_status.v1"
            or status.get("stop_id") != stop_id
            or not isinstance(status.get("request_id"), str)
            or not status["request_id"]
            or status.get("state")
            not in {
                "accepted",
                "stopping",
                "certifying",
                "succeeded",
                "partial",
                "failed",
            }
            or type(status.get("stop_generation")) is not int
            or not 1 <= status["stop_generation"] <= MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native aggregate Stop status has an invalid envelope",
            )
        return response

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._socket_path)

    def _exchange_control(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._control_socket_path)

    def _request_attribution(self, trusted_principal_id: str | None) -> dict[str, Any]:
        if trusted_principal_id is not None and (
            not trusted_principal_id
            or unicodedata.normalize("NFC", trusted_principal_id)
            != trusted_principal_id
            or len(trusted_principal_id.encode("utf-8")) > 256
        ):
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "trusted execution principal must be bounded non-empty NFC text",
            )
        if self._attribution_transport == "internal":
            if trusted_principal_id is not None:
                raise HeadlessApiError(
                    "execution_request_attribution_invalid",
                    "internal execution requests cannot claim an external principal",
                )
            return {
                "transport": "internal",
                "assurance": "unverified",
                "principal_id": None,
            }
        return {
            "transport": "http",
            "assurance": (
                "externally_authenticated"
                if trusted_principal_id is not None
                else "unverified"
            ),
            "principal_id": trusted_principal_id,
        }

    @staticmethod
    def _validate_projection_response(
        command: str, response: dict[str, Any]
    ) -> dict[str, Any]:
        if response.get("ok") is True and response.get("command") != command:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                f"execution {command} response has the wrong command kind",
            )
        return response

    def _read_projection_on(self, command: str, socket_path: Path) -> dict[str, Any]:
        return self._validate_projection_response(
            command,
            self._exchange_on(
                {"api_version": EXECUTION_COMMAND_VERSION, "command": command},
                socket_path,
            ),
        )

    def _read_projection(self, command: str) -> dict[str, Any]:
        return self._validate_projection_response(
            command,
            self._exchange(
                {"api_version": EXECUTION_COMMAND_VERSION, "command": command}
            ),
        )

    def diagnostics(self) -> dict[str, Any]:
        """Return the explicit aggregate diagnostic projection.

        The retained native wire spelling remains ``status`` for compatibility;
        ordinary runtime observers do not use it.
        """
        return self._read_projection("status")

    def identity(self) -> dict[str, Any]:
        return self._read_projection("identity")

    def targets(self) -> dict[str, Any]:
        return self._read_projection("targets")

    def control_state(self) -> dict[str, Any]:
        return self._read_projection("control_state")

    def query_operation(self, request: ProgramOperationQuery) -> dict[str, Any]:
        response = self.require_success(
            self._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "command": "query_operation",
                    "query": request.model_dump(mode="json"),
                }
            )
        )
        operation = response.get("operation")
        if (
            response.get("command") != "query_operation"
            or response.get("owner_epoch") != request.owner_epoch
            or response.get("target_role_id") != request.target_role_id
            or not isinstance(operation, dict)
            or operation.get("program_run_id") != request.program_run_id
            or operation.get("program_binding_id") != request.program_binding_id
            or not isinstance(operation.get("operation_id"), str)
            or not operation["operation_id"]
            or len(operation["operation_id"].encode("utf-8")) > MAX_IDENTITY_BYTES
            or type(operation.get("operation_revision")) is not int
            or not 1 <= operation["operation_revision"] <= MAXIMUM_SAFE_INTEGER
            or not isinstance(operation.get("participants"), list)
            or len(operation["participants"]) != 1
            or not isinstance(operation["participants"][0], dict)
        ):
            raise HeadlessApiError(
                "execution_operation_context_invalid",
                "operation context does not match its exact query",
            )
        return response

    def set_operation_runtime_scale(
        self,
        request: OperationRuntimeScaleRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self.require_success(
            self._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "attribution": self._request_attribution(trusted_principal_id),
                    "command": "set_operation_runtime_scale",
                    "request": request.model_dump(mode="json"),
                }
            )
        )
        accepted = response.get("accepted")
        if (
            response.get("command") != "set_operation_runtime_scale"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
            or accepted.get("operation_id") != request.operation_id
            or accepted.get("operation_revision") != request.expected_operation_revision
            or type(accepted.get("runtime_scale_generation")) is not int
            or accepted["runtime_scale_generation"]
            != request.expected_runtime_scale_generation + 1
            or accepted.get("requested_scale") != request.scale
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "operation scaling acceptance does not match its request",
            )
        return response

    def query_target(self, request: TargetObservationQuery) -> dict[str, Any]:
        response = self.require_success(
            self._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "command": "query_target",
                    "query": request.model_dump(mode="json", exclude_none=True),
                }
            )
        )
        observation = response.get("target_observation")
        expected = request.model_dump(mode="json", exclude_none=True)
        expected["schema_version"] = "manyforge.target_observation.v1"
        if request.kind == "frame_transform":
            self._require_frame_transform(response, observation, expected)
            return response
        if request.kind == "io_signal":
            self._require_io_signal(response, observation, expected)
            return response
        if (
            response.get("command") != "query_target"
            or not isinstance(observation, dict)
            or set(observation)
            != set(expected) | {"source_epoch", "source_sequence", "value"}
            or any(observation.get(key) != value for key, value in expected.items())
            or not isinstance(observation.get("source_epoch"), str)
            or not observation["source_epoch"]
            or len(observation["source_epoch"].encode("utf-8")) > 256
            or unicodedata.normalize("NFC", observation["source_epoch"])
            != observation["source_epoch"]
            or type(observation.get("source_sequence")) is not int
            or not 1 <= observation["source_sequence"] <= MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "execution_target_observation_invalid",
                "native target query returned a mismatched observation",
            )
        try:
            if request.kind == "frame_pose":
                pose = Pose3Request.model_validate(observation["value"])
                orientation = pose.orientation
                norm_squared = sum(
                    value * value
                    for value in (
                        orientation.x,
                        orientation.y,
                        orientation.z,
                        orientation.w,
                    )
                )
                if abs(norm_squared - 1.0) > 1e-6:
                    raise ValueError(
                        "native frame orientation is not a unit quaternion"
                    )
            else:
                GripperFeedbackObservation.model_validate(observation["value"])
        except ValueError as error:
            raise HeadlessApiError(
                "execution_target_observation_invalid",
                "native target query returned invalid typed evidence",
            ) from error
        return response

    def set_mock_io_input(self, request: MockIoInputRequest) -> dict[str, Any]:
        """Set one installed Mock input on the ordinary command socket.

        A simulation action, not a hardware command: the host contract carries
        the internal, unverified, no-principal attribution. Success echoes the
        same `target_observation` projection as `query_target` plus the model
        receipt: `model_value` (the accepted value), `published_cursor` (the
        record it was delivered as, or null while the source is not
        publishing, e.g. Disabled) and `publication_error` only when the model
        accepted it but ingress failed. A null cursor is not a failure.
        """

        response = self.require_success(
            self._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "command": "set_mock_io_input",
                    "attribution": {
                        "transport": "internal",
                        "assurance": "unverified",
                        "principal_id": None,
                    },
                    "request": request.model_dump(mode="json"),
                }
            )
        )
        expected = {
            "schema_version": "manyforge.target_observation.v1",
            "owner_epoch": request.owner_epoch,
            "program_binding_id": request.program_binding_id,
            "target_role_id": request.target_role_id,
            "kind": "io_signal",
            "resource_role_id": request.resource_role_id,
        }
        self._require_io_signal(
            response,
            response.get("target_observation"),
            expected,
            command="set_mock_io_input",
        )
        self._require_mock_receipt(response, request)
        return response

    @staticmethod
    def _require_mock_receipt(response: dict[str, Any], request: MockIoInputRequest) -> None:
        try:
            if (
                {"model_value", "published_cursor"} - set(response)
                or set(response) - {"api_version", "ok", "command", "target_observation",
                                    "model_value", "published_cursor", "publication_error"}
            ):
                raise ValueError("Mock receipt fields are not exact")
            if response["model_value"] is not request.value:
                raise ValueError("Mock receipt model value is not the requested value")
            cursor = response["published_cursor"]
            if cursor is not None:
                cursor = IoSourceCursor.model_validate(cursor)
                if cursor.owner_epoch != request.owner_epoch:
                    raise ValueError("Mock receipt cursor names another owner epoch")
                source_id = response["target_observation"]["value"]["source_id"]
                if source_id is not None and cursor.source_id != source_id:
                    raise ValueError("Mock receipt cursor names another source")
            if "publication_error" in response:
                if cursor is not None:
                    raise ValueError("a published Mock value carries no publication error")
                MockIoPublicationError.model_validate(response["publication_error"])
        except (TypeError, ValueError) as error:
            raise HeadlessApiError(
                "execution_target_observation_invalid",
                "native Mock input receipt is mismatched or invalid",
            ) from error

    @staticmethod
    def _require_io_signal(
        response: dict[str, Any],
        observation: Any,
        expected: dict[str, Any],
        *,
        command: str = "query_target",
    ) -> None:
        """An I/O answer echoes its query, names the owner evaluation time and
        carries one typed signal value; its sample identity lives in the
        value, never at the outer level."""

        try:
            if (
                response.get("command") != command
                or not isinstance(observation, dict)
                or set(observation) != set(expected) | {"evaluated_at_ns", "value"}
                or any(observation.get(key) != value for key, value in expected.items())
            ):
                raise ValueError("I/O signal answer does not echo its query")
            nonnegative_int64(observation["evaluated_at_ns"])
            IoSignalObservation.model_validate(observation["value"])
        except (TypeError, ValueError) as error:
            raise HeadlessApiError(
                "execution_target_observation_invalid",
                "native I/O signal query returned a mismatched or invalid answer",
            ) from error

    @staticmethod
    def _require_frame_transform(
        response: dict[str, Any], observation: Any, expected: dict[str, Any]
    ) -> None:
        """A transform answer echoes its query and carries one evaluation:
        the pose, the owner sample time, the scene revision and the evidence
        of every crossed frame path. It has no source sequence."""

        try:
            if (
                response.get("command") != "query_target"
                or not isinstance(observation, dict)
                or set(observation)
                != set(expected)
                | {
                    "evaluated_at_ns",
                    "resolution_evidence_digests",
                    "scene_revision",
                    "value",
                }
                or any(observation.get(key) != value for key, value in expected.items())
            ):
                raise ValueError("frame transform answer does not echo its query")
            nonnegative_int64(observation["evaluated_at_ns"])
            SceneRevisionRequest.model_validate(observation["scene_revision"])
            digests = observation["resolution_evidence_digests"]
            if (
                not isinstance(digests, list)
                or any(
                    not isinstance(digest, str) or re.fullmatch(SHA256_PATTERN, digest) is None
                    for digest in digests
                )
                or digests != sorted(set(digests))
            ):
                raise ValueError("frame transform evidence is not a sorted digest set")
            pose = Pose3Request.model_validate(observation["value"])
            orientation = pose.orientation
            norm_squared = sum(
                value * value
                for value in (orientation.x, orientation.y, orientation.z, orientation.w)
            )
            if abs(norm_squared - 1.0) > 1e-6:
                raise ValueError("frame transform orientation is not a unit quaternion")
        except (KeyError, TypeError, ValueError) as error:
            raise HeadlessApiError(
                "execution_target_observation_invalid",
                "native frame transform returned a mismatched or invalid answer",
            ) from error

    def observation(self) -> dict[str, Any]:
        # A long-poll observation session may legitimately occupy the
        # dedicated single-client streaming lane until its source changes.
        # One-shot snapshots are already supported by the ordinary command
        # lane and must not queue behind that stream.
        return self._read_projection("observation")

    def open_observation_session(self) -> ExecutionObservationSession:
        return ExecutionObservationSession(
            self._run_root,
            self._observation_socket_path,
        )

    @staticmethod
    def _raise_error_response(response: dict[str, Any]) -> None:
        error = response.get("error")
        if (
            response.get("ok") is False
            and isinstance(error, dict)
            and isinstance(error.get("code"), str)
            and error["code"]
            and isinstance(error.get("message"), str)
            and error["message"]
            and isinstance(error.get("details", {}), dict)
        ):
            raise HeadlessApiError(
                error["code"], error["message"], details=error.get("details")
            )
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "native execution owner returned an invalid error response",
        )

    @classmethod
    def require_success(cls, response: dict[str, Any]) -> dict[str, Any]:
        """Raise the existing stable owner error for a direct typed caller."""

        if response.get("ok") is not True:
            cls._raise_error_response(response)
        return response

    @staticmethod
    def _event_runtime_fault_snapshot(
        response: dict[str, Any], expected_owner_epoch: str
    ) -> dict[str, Any]:
        """Validate common provenance before interpreting page success/failure.

        The Headless owner validates the complete closed A/B fact schema before
        mutation. This transport never infers a condition from diagnostic text.
        """
        owner_epoch = response.get("owner_epoch")
        faults = response.get("runtime_faults")
        if (
            response.get("command") != "events"
            or not isinstance(owner_epoch, str)
            or not 0 < len(owner_epoch.encode("utf-8")) <= 2_048
            or unicodedata.normalize("NFC", owner_epoch) != owner_epoch
            or owner_epoch != expected_owner_epoch
            or type(response.get("execution_owner_ready")) is not bool
            or not isinstance(faults, dict)
            or set(faults) != {"operation_journal_write_fault", "program_runner_loss"}
            or any(
                fact is not None
                and (
                    not isinstance(fact, dict)
                    or fact.get("request_owner_epoch") != owner_epoch
                )
                for fact in faults.values()
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native runtime fault snapshot has invalid provenance",
            )
        return {"owner_epoch": owner_epoch, "runtime_faults": faults,
                "execution_owner_ready": response["execution_owner_ready"]}

    def events(
        self,
        owner_epoch: str,
        after_sequence: int,
        *,
        limit: int = 16,
    ) -> dict[str, Any]:
        if (
            not owner_epoch
            or len(owner_epoch.encode("utf-8")) > 2_048
            or type(after_sequence) is not int
            or not 0 <= after_sequence <= MAXIMUM_SAFE_INTEGER
            or type(limit) is not int
            or not 1 <= limit <= 16
        ):
            raise ValueError("operation event query exceeds its fixed bounds")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "after_sequence": after_sequence,
                "command": "events",
                "limit": limit,
                "owner_epoch": owner_epoch,
            }
        )
        runtime_snapshot = self._event_runtime_fault_snapshot(response, owner_epoch)
        try:
            if response.get("ok") is not True:
                self._raise_error_response(response)
            page = response.get("event_page")
            admission_generation = response.get("admission_generation")
            if (
                response.get("command") != "events"
                or type(admission_generation) is not int
                or not 1 <= admission_generation <= MAXIMUM_SAFE_INTEGER
                or not isinstance(page, dict)
                or page.get("schema_version") != "manyforge.execution_event_page.v1"
                or page.get("owner_epoch") != owner_epoch
                or type(page.get("next_after_sequence")) is not int
                or not after_sequence <= page["next_after_sequence"] <= MAXIMUM_SAFE_INTEGER
                or type(page.get("has_more")) is not bool
                or not isinstance(page.get("events"), list)
                or len(page["events"]) > limit
                or (page["has_more"] and len(page["events"]) != limit)
            ):
                raise HeadlessApiError(
                    "execution_command_response_invalid",
                    "native execution event page has an invalid envelope",
                )
            previous = after_sequence
            for event in page["events"]:
                try:
                    nonnegative_int64(
                        event.get("occurred_at_ns") if isinstance(event, dict) else None
                    )
                except ValueError as error:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "native execution event timestamp is invalid",
                    ) from error
                if (
                    not isinstance(event, dict)
                    or event.get("owner_epoch") != owner_epoch
                    or type(event.get("sequence")) is not int
                    or not previous < event["sequence"] <= MAXIMUM_SAFE_INTEGER
                    or event.get("record_kind") not in OPERATION_EVENT_KINDS
                    or (
                        event.get("request_id") is not None
                        and (
                            not isinstance(event.get("request_id"), str)
                            or not event["request_id"]
                            or len(event["request_id"].encode("utf-8")) > 2_048
                        )
                    )
                    or not _valid_operation_event_identity(event)
                    or not isinstance(event.get("occurred_at_wall"), str)
                    or not event["occurred_at_wall"]
                    or len(event["occurred_at_wall"].encode("utf-8")) > 128
                    or unicodedata.normalize("NFC", event["occurred_at_wall"])
                    != event["occurred_at_wall"]
                    or type(event.get("effect_pending")) is not bool
                    or not isinstance(event.get("payload"), dict)
                    or not isinstance(event.get("payload_digest"), str)
                ):
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "native execution event page contains an invalid record",
                    )
                previous = event["sequence"]
            if page["next_after_sequence"] != previous:
                raise HeadlessApiError(
                    "execution_command_response_invalid",
                    "native execution event cursor does not match its records",
                )
            progress = response.get("operation_progress")
            if not isinstance(progress, list) or len(progress) > MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS:
                raise HeadlessApiError("execution_command_response_invalid", "native operation progress exceeds its bound")
            seen: set[str] = set()
            for row in progress:
                if (
                    not isinstance(row, dict)
                    or set(row) != {"owner_epoch", "operation_id", "operation_revision", "progress"}
                    or row["owner_epoch"] != owner_epoch
                    or not isinstance(row["operation_id"], str)
                    or not 0 < len(row["operation_id"].encode("utf-8")) <= 2_048
                    or row["operation_id"] in seen
                    or type(row["operation_revision"]) is not int
                    or not 1 <= row["operation_revision"] <= MAXIMUM_SAFE_INTEGER
                    or type(row["progress"]) not in (float, int)
                    or not math.isfinite(row["progress"])
                    or not 0 <= row["progress"] <= 1
                ):
                    raise HeadlessApiError("execution_command_response_invalid", "native operation progress is invalid")
                seen.add(row["operation_id"])
            page["operation_progress"] = progress
            page["admission_generation"] = admission_generation
            page["runtime_faults"] = runtime_snapshot["runtime_faults"]
            page["execution_owner_ready"] = runtime_snapshot["execution_owner_ready"]
            # Parsed by the global rule status projection (absent: unknown).
            page["interrupt_rule_catalog"] = response.get("interrupt_rule_catalog")
            page["interrupt_rule_state_baseline"] = response.get(
                "interrupt_rule_state_baseline"
            )
            return page
        except Exception as error:
            error.runtime_fault_snapshot = runtime_snapshot
            raise

    def submit(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "submit",
                "operation": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "submit"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution acceptance does not match the submitted request",
            )
        return response

    def read_manual_handover(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Read exact Kernel cleanup input; this does not grant manual admission."""
        operation = request.model_dump(mode="json")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "read_manual_handover",
                "operation": operation,
            }
        )
        if response.get("ok") is not True:
            return response
        if (
            set(response)
            != {
                "api_version",
                "command",
                "ok",
                "handover",
                "availability",
                "intent_digest",
                "request_id",
                "request_owner_epoch",
            }
            or response.get("command") != "read_manual_handover"
            or response.get("request_id") != request.request_context.request_id
            or response.get("request_owner_epoch")
            != request.request_context.request_owner_epoch
            or response.get("intent_digest")
            != "sha256:" + hashlib.sha256(_canonical_json(operation)).hexdigest()
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "manual handover response differs from its request",
            )
        try:
            availability = ManualControlAvailability.model_validate(
                response["availability"]
            )
            if availability.cleanup_required and response["handover"] is None:
                raise ValueError("manual cleanup requires an exact held Program")
        except ValueError as error:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "manual availability response is malformed",
            ) from error
        if response["handover"] is not None:
            try:
                parsed = ManualProgramHandover.model_validate(response["handover"])
            except ValueError as error:
                raise HeadlessApiError(
                    "execution_command_response_invalid",
                    "manual handover response is malformed",
                ) from error
            if parsed.context.program_run_id == request.program_run_id:
                raise HeadlessApiError(
                    "execution_command_response_identity_mismatch",
                    "manual operation cannot be its own Program parent",
                )
        return response

    def preview_motion(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        operation = request.model_dump(mode="json")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "preview_motion",
                "operation": operation,
            }
        )
        if response.get("ok") is not True:
            return response
        receipt = response.get("preview_receipt")
        participant = operation["participants"][0]
        expected_intent_digest = (
            "sha256:" + hashlib.sha256(_canonical_json(operation)).hexdigest()
        )
        expected_scene = operation["expected_scene_revision"]
        if (
            set(response) != MOTION_PREVIEW_RESPONSE_KEYS
            or not isinstance(receipt, dict)
            or set(receipt) != MOTION_PREVIEW_RECEIPT_KEYS
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview response has an undeclared shape",
            )
        if (
            response.get("command") != "preview_motion"
            or receipt.get("schema_version")
            != "manyforge.direct_control_motion_preview.v1"
            or receipt.get("commit_eligible") is not True
            or receipt.get("intent_digest") != expected_intent_digest
            or receipt.get("request_id") != operation["request_context"]["request_id"]
            or receipt.get("request_owner_epoch")
            != operation["request_context"]["request_owner_epoch"]
            or receipt.get("program_id") != operation["program_id"]
            or receipt.get("program_revision") != operation["program_revision"]
            or receipt.get("program_binding_id") != operation["program_binding_id"]
            or receipt.get("program_run_id") != operation["program_run_id"]
            or receipt.get("graph_revision") != operation["expected_graph_revision"]
            or receipt.get("retained_bundle_id")
            != operation["expected_retained_bundle_id"]
            or not _motion_preview_scene_revision_matches(
                receipt.get("scene_revision"), expected_scene
            )
            or receipt.get("component_role_id") != participant["component_role_id"]
            or receipt.get("target_role_id") != participant["target_role_id"]
            or receipt.get("required_capability_id")
            != participant["required_capability_id"]
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "motion preview receipt does not match its submitted intent",
            )
        source_names = receipt.get("source_joint_names")
        submitted_motion = participant["payload"].get("motion")
        submitted_source_names = (
            submitted_motion.get("joint_names")
            if isinstance(submitted_motion, dict)
            and submitted_motion.get("kind") == "joint_goal"
            else None
        )
        start_positions = receipt.get("start_positions_rad")
        final_positions = receipt.get("final_positions_rad")
        collision_revision = receipt.get("collision_world_revision")
        if (
            not isinstance(source_names, list)
            or not source_names
            or not all(isinstance(name, str) and name for name in source_names)
            or len(source_names) != len(set(source_names))
            or (
                submitted_source_names is not None
                and source_names != submitted_source_names
            )
            or not isinstance(start_positions, list)
            or not isinstance(final_positions, list)
            or len(start_positions) != len(source_names)
            or len(final_positions) != len(source_names)
            or not all(
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and math.isfinite(value)
                for value in [*start_positions, *final_positions]
            )
            or not isinstance(collision_revision, dict)
            or set(collision_revision) != MOTION_PREVIEW_COLLISION_REVISION_KEYS
            or type(collision_revision.get("revision_id")) is not int
            or collision_revision["revision_id"] < 1
            or collision_revision["revision_id"] > MAXIMUM_SAFE_INTEGER
            or not isinstance(collision_revision.get("content_hash"), str)
            or not collision_revision["content_hash"]
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview receipt is malformed",
            )
        digest_input = dict(receipt)
        receipt_digest = digest_input.pop("receipt_digest", None)
        expected_receipt_digest = (
            "sha256:" + hashlib.sha256(_canonical_json(digest_input)).hexdigest()
        )
        if receipt_digest != expected_receipt_digest:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview receipt digest is invalid",
            )
        try:
            Pose3Request.model_validate(receipt.get("final_tcp_pose"))
        except ValueError as exc:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview pose is malformed",
            ) from exc
        return response

    def preview_capability_command(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        operation = request.model_dump(mode="json")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "preview_capability_command",
                "operation": operation,
            }
        )
        if response.get("ok") is not True:
            return response
        receipt = response.get("preview_receipt")
        participant = operation["participants"][0]
        payload = participant["payload"]
        command_value = payload.get("command_value")
        position = (
            command_value.get("position") if isinstance(command_value, dict) else None
        )
        expected_intent_digest = (
            "sha256:" + hashlib.sha256(_canonical_json(operation)).hexdigest()
        )
        if (
            set(response) != CAPABILITY_COMMAND_PREVIEW_RESPONSE_KEYS
            or not isinstance(receipt, dict)
            or set(receipt) != CAPABILITY_COMMAND_PREVIEW_RECEIPT_KEYS
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native capability-command preview has an undeclared shape",
            )
        if (
            response.get("command") != "preview_capability_command"
            or receipt.get("schema_version")
            != "manyforge.direct_control_capability_command_preview.v1"
            or receipt.get("commit_eligible") is not True
            or receipt.get("intent_digest") != expected_intent_digest
            or receipt.get("request_id") != operation["request_context"]["request_id"]
            or receipt.get("request_owner_epoch")
            != operation["request_context"]["request_owner_epoch"]
            or receipt.get("program_id") != operation["program_id"]
            or receipt.get("program_revision") != operation["program_revision"]
            or receipt.get("program_binding_id") != operation["program_binding_id"]
            or receipt.get("program_run_id") != operation["program_run_id"]
            or receipt.get("graph_revision") != operation["expected_graph_revision"]
            or receipt.get("retained_bundle_id")
            != operation["expected_retained_bundle_id"]
            or receipt.get("scene_revision") != operation["expected_scene_revision"]
            or receipt.get("component_role_id") != participant["component_role_id"]
            or receipt.get("target_role_id") != participant["target_role_id"]
            or receipt.get("required_capability_id")
            != participant["required_capability_id"]
            or receipt.get("command_schema_id") != payload.get("command_schema_id")
            or receipt.get("command_schema_revision")
            != payload.get("command_schema_revision")
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "capability-command preview does not match its submitted intent",
            )
        coordinate_kind = receipt.get("coordinate_kind")
        source_kind = receipt.get("source_kind")
        expected_target: object = None
        if isinstance(position, dict):
            if position.get("kind") == "angular_rad":
                expected_target = position.get("value_rad")
            elif position.get("kind") == "linear_m":
                expected_target = position.get("value_m")
        min_position = receipt.get("min_position")
        max_position = receipt.get("max_position")
        target_position = receipt.get("target_position")
        bounded_model_resource = (
            source_kind == "model_role"
            and all(
                isinstance(value, (int, float))
                and not isinstance(value, bool)
                and math.isfinite(value)
                for value in [min_position, max_position, target_position]
            )
            and min_position <= target_position <= max_position
        )
        unbounded_component_local_resource = (
            source_kind == "component_local"
            and min_position is None
            and max_position is None
            and isinstance(target_position, (int, float))
            and not isinstance(target_position, bool)
            and math.isfinite(target_position)
        )
        if (
            not isinstance(position, dict)
            or coordinate_kind != position.get("kind")
            or target_position != expected_target
            or not isinstance(receipt.get("source_coordinate_name"), str)
            or not receipt["source_coordinate_name"]
            or not (bounded_model_resource or unbounded_component_local_resource)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native capability-command preview is malformed",
            )
        digest_input = dict(receipt)
        receipt_digest = digest_input.pop("receipt_digest", None)
        expected_receipt_digest = (
            "sha256:" + hashlib.sha256(_canonical_json(digest_input)).hexdigest()
        )
        if receipt_digest != expected_receipt_digest:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native capability-command preview digest is invalid",
            )
        return response

    def read(self, operation_id: str) -> dict[str, Any]:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read",
                "operation_id": operation_id,
            }
        )
        operation = response.get("operation")
        if response.get("ok") is True and (
            response.get("command") != "read"
            or not isinstance(operation, dict)
            or operation.get("operation_id") != operation_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution observation does not match the requested operation",
            )
        if response.get("ok") is True and type(response.get("resident")) is not bool:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution observation has no exact residency projection",
            )
        return response

    def read_volatile_motion_trace(self, allowance_bytes: int) -> dict[str, Any]:
        """Return the native volatile motion-trace object, unchanged.

        Diagnostic only: ``allowance_bytes`` caps the encoded trace object the
        native host may return. Only the envelope, the object shape and the
        schema version are checked; failures raise ``HeadlessApiError``.
        """
        if type(allowance_bytes) is not int or allowance_bytes <= 0:
            raise ValueError("trace allowance must be a positive integer")
        response = self.require_success(
            self._exchange(
                {
                    "api_version": EXECUTION_COMMAND_VERSION,
                    "attribution": self._request_attribution(None),
                    "command": "read_volatile_motion_trace",
                    "allowance_bytes": allowance_bytes,
                }
            )
        )
        trace = response.get("trace")
        if (
            response.get("command") != "read_volatile_motion_trace"
            or not isinstance(trace, dict)
            or trace.get("schema_version") != VOLATILE_MOTION_TRACE_VERSION
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native volatile motion trace response is invalid",
            )
        return trace

    def operation_recovery(self, operation_id: str) -> dict[str, Any]:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "get_operation_recovery",
                "operation_id": operation_id,
            }
        )
        assessment = response.get("recovery_assessment")
        participants = response.get("participants")
        if response.get("ok") is True and (
            response.get("command") != "get_operation_recovery"
            or not isinstance(assessment, dict)
            or assessment.get("schema_version")
            != "manyforge.kernel_recovery_assessment.v1"
            or type(assessment.get("assessment_revision")) is not int
            or assessment["assessment_revision"] < 1
            or type(assessment.get("control_command_epoch")) is not int
            or type(assessment.get("execution_id")) is not int
            or not isinstance(assessment.get("allowed_actions"), list)
            or (
                "retry_eligibility" in assessment
                and not _valid_retry_eligibility(assessment["retry_eligibility"])
            )
            or type(response.get("operation_revision")) is not int
            or response["operation_revision"] < 1
            or not isinstance(participants, list)
            or not participants
            or (
                response.get("stop_id") is None
                and response.get("stop_generation") is not None
            )
            or (
                response.get("stop_id") is not None
                and (
                    not isinstance(response.get("stop_id"), str)
                    or not response["stop_id"]
                    or type(response.get("stop_generation")) is not int
                    or response["stop_generation"] < 1
                )
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native operation recovery assessment is invalid",
            )
        return response

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        request: OperationRecoveryActionRequest,
    ) -> dict[str, Any]:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        if (
            type(expected_operation_revision) is not int
            or not 1 <= expected_operation_revision <= MAXIMUM_SAFE_INTEGER
        ):
            raise ValueError("expected operation revision is invalid")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "apply_operation_recovery",
                "expected_operation_revision": expected_operation_revision,
                "operation_id": operation_id,
                "request": request.model_dump(mode="json"),
            }
        )
        result = response.get("result")
        if response.get("ok") is True and (
            response.get("command") != "apply_operation_recovery"
            or not isinstance(result, dict)
            or result.get("request_id") != request.request_id
            or result.get("action") != request.action
            or type(result.get("accepted")) is not bool
            or not isinstance(result.get("resulting_assessment"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "native operation recovery result changed request identity",
            )
        return response

    def _plan_pick_place(
        self,
        command: str,
        request: PickSequencePlanningRequest | PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": command,
                "intent": request.model_dump(mode="json"),
            }
        )
        result = response.get("planning_result")
        expected_kind = "pick" if command.endswith("pick") else "place"
        if response.get("ok") is True:
            if (
                response.get("command") != command
                or not isinstance(result, dict)
                or result.get("schema_version")
                != "manyforge.pick_place_planning_result.v1"
                or result.get("sequence_id") != request.sequence_id
                or result.get("kind") != expected_kind
                or result.get("selected_object_id") != request.selected_object_id
                or result.get("status") not in {"planned", "no_feasible_candidate"}
                or type(result.get("attempted_candidate_count")) is not int
                or not 0 <= result["attempted_candidate_count"] <= 4096
                or not isinstance(result.get("reports"), list)
                or len(result["reports"]) > result["attempted_candidate_count"]
                or (
                    expected_kind == "pick"
                    and result["attempted_candidate_count"] > 0
                    and not isinstance(result.get("last_attempted_candidate"), dict)
                )
                or (
                    expected_kind == "place"
                    and result.get("last_attempted_candidate") is not None
                )
                or (
                    result["status"] == "planned"
                    and not isinstance(result.get("plan"), dict)
                )
                or (
                    result["status"] == "no_feasible_candidate"
                    and result.get("plan") is not None
                )
            ):
                raise HeadlessApiError(
                    "execution_command_response_identity_mismatch",
                    "pick/place planning result does not match its submitted intent",
                )
        return response

    def plan_pick(
        self,
        request: PickSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._plan_pick_place(
            "plan_pick",
            request,
            trusted_principal_id=trusted_principal_id,
        )

    def plan_place(
        self,
        request: PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._plan_pick_place(
            "plan_place",
            request,
            trusted_principal_id=trusted_principal_id,
        )

    def _control(
        self,
        command: str,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": command,
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != command
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
            or accepted.get("operation_id") != request.operation_id
            or accepted.get("control") != command
            or accepted.get("accepted_against_revision")
            != request.expected_operation_revision
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution control acceptance does not match its request",
            )
        return response

    def reset(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._control(
            "reset", request, trusted_principal_id=trusted_principal_id
        )

    def acknowledge_completed(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._control(
            "acknowledge_completed",
            request,
            trusted_principal_id=trusted_principal_id,
        )

    def stop(
        self,
        request: StopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "stop",
                "request": _sparse_request_with_complete_context(request),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution Stop acceptance does not match its request",
            )
        return response

    def full_stop(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "full_stop",
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "full_stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
            or accepted.get("resolution_reason") != "explicit_full_stop"
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Full Stop acceptance does not match its request",
            )
        return response

    def recognized_malformed_stop(
        self,
        *,
        request_id: str,
        recognized_route: str,
        reason: str,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "malformed_stop",
                "reason": reason,
                "recognized_route": recognized_route,
                "request_id": request_id,
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request_id
            or accepted.get("resolution_disposition") != "promoted_to_full"
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "recognized malformed Stop acceptance does not match its adapter identity",
            )
        return response

    @staticmethod
    def _valid_stop_fault_offer(offer: object) -> bool:
        if offer is None:
            return True
        if not isinstance(offer, dict) or set(offer) != {
            "configuration_expectations",
            "latch",
            "recovery_unavailable_reason",
        }:
            return False
        unavailable_reason = offer.get("recovery_unavailable_reason")
        if unavailable_reason is not None and (
            not isinstance(unavailable_reason, str) or not unavailable_reason
        ):
            return False
        latch = offer.get("latch")
        expectations = offer.get("configuration_expectations")
        if not isinstance(latch, dict) or not isinstance(expectations, list):
            return False
        if set(latch) != {
            "captured_operation_ids",
            "captured_resources",
            "cell_id",
            "clear_projection",
            "evidence_refs",
            "graph_revision",
            "latch_generation",
            "latch_id",
            "persistence_degraded",
            "reason_code",
            "recovery_assessment_revision",
            "recovery_status",
            "retained_bundle_id",
            "state",
            "stop_certificate",
            "stop_generation",
            "stop_id",
            "terminal_projection",
            "trigger",
        }:
            return False
        required_text = (
            "cell_id",
            "graph_revision",
            "latch_id",
            "reason_code",
            "retained_bundle_id",
            "stop_id",
        )
        if any(
            not isinstance(latch.get(key), str) or not latch[key]
            for key in required_text
        ):
            return False
        if any(
            type(latch.get(key)) is not int
            or not 1 <= latch[key] <= MAXIMUM_SAFE_INTEGER
            for key in (
                "latch_generation",
                "recovery_assessment_revision",
                "stop_generation",
            )
        ):
            return False
        if (
            type(latch.get("persistence_degraded")) is not bool
            or not isinstance(latch.get("captured_operation_ids"), list)
            or not isinstance(latch.get("captured_resources"), list)
            or not isinstance(latch.get("evidence_refs"), list)
            or not isinstance(latch.get("terminal_projection"), dict)
            or not isinstance(latch.get("clear_projection"), dict)
            or latch.get("state") not in {"latched", "recovery_pending"}
            or latch.get("recovery_status")
            not in {
                "waiting_for_observation",
                "waiting_for_quiescence",
                "waiting_for_configuration",
                "waiting_for_external_reconciliation",
                "waiting_for_durable_record",
                "ready",
                "recovery_pending",
            }
        ):
            return False
        for key in ("captured_operation_ids", "captured_resources"):
            rows = latch[key]
            if (
                any(not isinstance(item, str) or not item for item in rows)
                or rows != sorted(rows)
                or len(rows) != len(set(rows))
            ):
                return False
        certificate = latch.get("stop_certificate")
        if certificate is not None and (
            not isinstance(certificate, dict)
            or set(certificate)
            != {
                "stop_certificate_id",
                "stop_certificate_revision",
                "stop_generation",
                "stop_id",
            }
            or any(
                not isinstance(certificate.get(key), str) or not certificate[key]
                for key in ("stop_certificate_id", "stop_id")
            )
            or any(
                type(certificate.get(key)) is not int
                or not 1 <= certificate[key] <= MAXIMUM_SAFE_INTEGER
                for key in ("stop_certificate_revision", "stop_generation")
            )
            or certificate["stop_id"] != latch["stop_id"]
            or certificate["stop_generation"] != latch["stop_generation"]
        ):
            return False
        for key in ("terminal_projection", "clear_projection"):
            projection = latch[key]
            if (
                not isinstance(projection, dict)
                or set(projection) != {"error", "retry_generation", "state"}
                or projection.get("state")
                not in {"not_applicable", "pending", "recorded", "failed"}
            ):
                return False
        for evidence in latch["evidence_refs"]:
            if (
                not isinstance(evidence, dict)
                or set(evidence) != {"evidence_digest", "kind"}
                or not isinstance(evidence.get("evidence_digest"), str)
                or not evidence["evidence_digest"].startswith("sha256:")
                or evidence.get("kind")
                not in {
                    "settlement",
                    "late_quiescence",
                    "configuration",
                    "external_reconciliation",
                    "backend_or_lifecycle_reset",
                }
            ):
                return False
        prior = ""
        for row in expectations:
            if (
                not isinstance(row, dict)
                or set(row)
                != {
                    "active_control_configuration_generation",
                    "active_control_configuration_id",
                    "control_domain_id",
                }
                or not isinstance(row.get("control_domain_id"), str)
                or not row["control_domain_id"]
                or row["control_domain_id"] <= prior
                or not isinstance(row.get("active_control_configuration_id"), str)
                or not row["active_control_configuration_id"]
                or type(row.get("active_control_configuration_generation")) is not int
                or not 1
                <= row["active_control_configuration_generation"]
                <= MAXIMUM_SAFE_INTEGER
            ):
                return False
            prior = row["control_domain_id"]
        if certificate is None:
            return (
                not expectations
                and unavailable_reason is None
                and latch["state"] == "latched"
                and latch["recovery_status"] == "waiting_for_observation"
                and latch["terminal_projection"]["state"] == "pending"
                and latch["clear_projection"]["state"] == "not_applicable"
            )
        # A certified latch carries either a non-empty configuration proof or
        # a named reason why recovery is unavailable; never an empty proof.
        return bool(expectations) != (unavailable_reason is not None)

    @staticmethod
    def _valid_global_rule_recovery_offers(offers: object) -> bool:
        """SX-2206: one closed offer per actually latched global rule."""

        if not isinstance(offers, list):
            return False
        for offer in offers:
            if not isinstance(offer, dict) or set(offer) != {
                "current_condition",
                "latch_cause",
                "post_latch_disposition",
                "selector",
            }:
                return False
            condition = offer["current_condition"]
            if (condition is not None and type(condition) is not bool) or (
                offer["latch_cause"] not in ("condition", "source_loss")
            ) or offer["post_latch_disposition"] not in (
                "auto_clear_on_signal_release",
                "await_operator_ack",
            ):
                return False
            try:
                GlobalIoRuleRecoverySelector.model_validate(offer["selector"])
            except ValueError:
                return False
        return True

    def stop_fault(self) -> dict[str, Any]:
        # An in-memory Stop-progression read: served on the control socket.
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "get_stop_fault",
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "get_stop_fault"
            or not self._valid_stop_fault_offer(response.get("stop_fault"))
            or not self._valid_global_rule_recovery_offers(
                response.get("global_rule_recovery_offers", [])
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Stop-fault response is invalid",
            )
        return response

    def recover_stop_fault(
        self,
        request: StopFaultRecoveryIntentRequest | GlobalIoRuleRecoveryIntentRequest,
    ) -> dict[str, Any]:
        request_payload = request.model_dump(mode="json")
        global_rule = isinstance(request, GlobalIoRuleRecoveryIntentRequest)
        if request.reason is None and not global_rule:
            # The original Stop/certificate request bytes stay unchanged.
            request_payload.pop("reason")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "recover_stop_fault",
                "request": request_payload,
            }
        )
        result = response.get("result")
        if response.get("ok") is True and (
            response.get("command") != "recover_stop_fault"
            or not isinstance(result, dict)
            or result.get("request_id") != request.request_context.request_id
            or type(result.get("accepted")) is not bool
            or (
                not self._valid_global_rule_recovery_result(request, result)
                if global_rule
                else not isinstance(result.get("latch"), dict)
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Stop-fault recovery result does not match its request",
            )
        return response

    @staticmethod
    def _valid_global_rule_recovery_result(
        request: GlobalIoRuleRecoveryIntentRequest, result: dict[str, Any]
    ) -> bool:
        if set(result) != {
            "accepted",
            "context",
            "error",
            "replay_status",
            "request_id",
            "rule",
        }:
            return False
        # The exact requested selector context (SX-2216), also for a replay of
        # a historical request after the current Applied context changed.
        if result["context"] != request.selector.context.model_dump(mode="json"):
            return False
        rule = result["rule"]
        if rule is None:
            return result["accepted"] is False
        return (
            isinstance(rule, dict)
            and set(rule) == {"rule_id", "state", "triggering_acquisition"}
            and rule["rule_id"] == request.selector.rule_id
            and rule["state"] in ("clear", "tripped", "latched")
        )

    def set_runtime_scale_control(
        self,
        request: RuntimeScaleControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "set_runtime_scale_control",
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        selector = request.selector.model_dump(mode="json")
        if response.get("ok") is True and (
            response.get("command") != "set_runtime_scale_control"
            or not isinstance(accepted, dict)
            or accepted.get("request_id") != request.request_context.request_id
            or accepted.get("selector") != selector
            or accepted.get("requested_scale") != request.scale
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "runtime-scale control acceptance does not match its request",
            )
        return response

    def get_runtime_scale_control(
        self, selector: RuntimeScaleControlSelectorRequest
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "get_runtime_scale_control",
                "selector": selector.model_dump(mode="json"),
            }
        )
        status = response.get("runtime_scale_control")
        if response.get("ok") is True and (
            response.get("command") != "get_runtime_scale_control"
            or not isinstance(status, dict)
            or status.get("selector") != selector.model_dump(mode="json")
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "runtime-scale control observation does not match its selector",
            )
        return response


class ExecutionEventPump:
    """Move durable native facts to the C++ historian without owning truth."""

    def __init__(
        self,
        core: HeadlessCorePort,
        execution: ExecutionPort,
        *,
        poll_seconds: float = 0.1,
        condition_poll_seconds: float = 0.5,
        page_limit: int = 16,
        cclc: Any | None = None,
        component_condition_poll_seconds: float = 1.0,
    ) -> None:
        if not 0.01 <= poll_seconds <= 5.0:
            raise ValueError("operation event polling period is outside [0.01, 5]")
        if not 0.05 <= condition_poll_seconds <= 5.0:
            raise ValueError("operation condition polling period is outside [0.05, 5]")
        if not 0.1 <= component_condition_poll_seconds <= 5.0:
            raise ValueError("component condition polling period is outside [0.1, 5]")
        if not 1 <= page_limit <= 16:
            raise ValueError("operation event page limit is outside [1, 16]")
        self._core = core
        self._execution = execution
        self._poll_seconds = poll_seconds
        self._condition_poll_seconds = condition_poll_seconds
        self._page_limit = page_limit
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_error: str | None = None
        self._health_lock = threading.Lock()
        self._change_observer: Callable[[], None] | None = None
        self._settled_observer: Callable[[], None] | None = None
        self._progress_observer: Callable[[list[dict[str, Any]]], None] | None = None
        self._progress_rows: dict[str, dict[str, Any]] = {}
        self._progress_dirty = False
        self._progress_sent_at = float("-inf")
        # Global rule states for the GUI, folded from the same pages.
        self._global_rules = GlobalRuleStatusModel()
        self._global_rules_observer: (
            Callable[[list[dict[str, Any]] | None], None] | None
        ) = None
        self._global_rules_sent: list[dict[str, Any]] | None = None
        self._global_rules_failing = False
        self._alarm_refresh_failing = False
        self._cclc = cclc
        self._component_condition_poll_seconds = component_condition_poll_seconds
        self._next_component_condition_poll_at = float("-inf")
        # The short retry floor: no lifecycle read starts before it, even
        # while a read is pending.
        self._next_component_condition_retry_at = float("-inf")
        # The core's `component_lifecycle_observation_pending`, as last
        # reported by the alarm-source refresh this pump offers each
        # iteration: the lifecycle source needs an observation of the active
        # configuration, so the next read does not wait the ordinary period.
        self._component_lifecycle_observation_pending = False
        self._component_conditions_failing = False
        # CCLC incident history (addendum 2 contract item 5): cursor per CCLC
        # owner epoch; the core deduplicates by (incident id, event kind).
        self._incident_event_owner_epoch: str | None = None
        self._incident_event_after_sequence = 0
        self._next_incident_event_poll_at = float("-inf")
        self._incident_events_failing = False

    def set_progress_observer(
        self, observer: Callable[[list[dict[str, Any]]], None] | None,
    ) -> None:
        self._progress_observer = observer

    def _capture_progress(self, rows: list[dict[str, Any]]) -> None:
        # The native page carries a complete bounded latest-only projection,
        # including an empty list when no manual operation is executing.
        current = {row["operation_id"]: row for row in rows}
        if self._progress_rows != current:
            self._progress_rows = current
            self._progress_dirty = True

    def _publish_progress(self, now: float) -> None:
        # Reuse the event pump's existing wake: no poller, timer or owner read.
        # The native client validates the bound before accepting each page.
        if not self._progress_dirty or now - self._progress_sent_at < 0.2:
            return
        self._progress_dirty = False
        self._progress_sent_at = now
        if self._progress_observer is not None:
            try:
                self._progress_observer(list(self._progress_rows.values()))
            except Exception:
                logging.getLogger(__name__).exception("Operation progress observer failed")

    def set_global_rules_observer(
        self, observer: Callable[[list[dict[str, Any]] | None], None] | None,
    ) -> None:
        self._global_rules_observer = observer

    def _observe_global_rules(self, owner_epoch: str, page: dict[str, Any]) -> None:
        # A malformed rule wire makes only this projection unknown; the
        # historian feed continues (the Runner mirror's own recapture rule).
        try:
            self._global_rules.observe(
                owner_epoch,
                page.get("interrupt_rule_catalog"),
                page.get("interrupt_rule_state_baseline"),
                page["events"],
            )
            if self._global_rules_failing:
                self._global_rules_failing = False
                logging.getLogger(__name__).info("global rule status recovered")
        except Exception:
            if not self._global_rules_failing:
                self._global_rules_failing = True
                logging.getLogger(__name__).warning(
                    "global rule status is unreadable", exc_info=True
                )
        self._publish_global_rules()

    def _publish_global_rules(self) -> None:
        rows = self._global_rules.rows()
        if rows == self._global_rules_sent:
            return
        self._global_rules_sent = rows
        if self._global_rules_observer is not None:
            try:
                self._global_rules_observer(rows)
            except Exception:
                logging.getLogger(__name__).exception("Global rule status observer failed")

    def set_change_observer(self, observer: Callable[[], None] | None) -> None:
        if observer is not None and not callable(observer):
            raise TypeError("execution event change observer must be callable")
        self._change_observer = observer

    def set_settled_observer(self, observer: Callable[[], None] | None) -> None:
        """Observe any operation of any target coming to rest (see below)."""
        if observer is not None and not callable(observer):
            raise TypeError("execution event settled observer must be callable")
        self._settled_observer = observer

    def _notify_motion_settled(self) -> None:
        observer = self._settled_observer
        if observer is None:
            return
        try:
            observer()
        except Exception:
            # Like _notify_change: a missed refresh is repaired by the next
            # meaningful control change and never backpressures ingestion.
            pass

    def _refresh_alarm_sources(self) -> None:
        """Keep the alarm sources within their currency bound (spec 530 §11).

        Every pump iteration offers the refresh; the core performs it at most
        once per period R and otherwise returns without its lock. A failed
        refresh is not an operation-feed failure: the motion-attention read
        reports sources older than the bound C as not reconciled.
        """

        try:
            result = self._core.refresh_alarm_sources()
            # Spec 530 §7.1: a configuration change is respected without
            # polling. The core reports, without its lock, that the lifecycle
            # source's quality became unknown (a registry replacement by any
            # reader's refresh, or an unreachable CCLC) and no complete
            # observation of the active configuration has been accepted since.
            self._component_lifecycle_observation_pending = (
                isinstance(result, dict)
                and result.get("component_lifecycle_observation_pending") is True
            )
        except Exception:
            if not self._alarm_refresh_failing:
                self._alarm_refresh_failing = True
                logging.getLogger(__name__).warning(
                    "alarm source refresh failed", exc_info=True
                )
            return
        if self._alarm_refresh_failing:
            self._alarm_refresh_failing = False
            logging.getLogger(__name__).info("alarm source refresh recovered")

    def _observe_component_lifecycle_conditions(self) -> None:
        """Hand CCLC component snapshots to the lifecycle-cause alarm adapter.

        Spec 530 §3.3 (2026-10-08): the C++ adapter derives the safety-stop
        condition and submits only changed ones. This
        bounded read runs at most once per period and only while the active
        configuration names components or the lifecycle source is registered.
        A configuration without controlled components has no CCLC owner (an
        Apply to it removes the CCLC service), so its empty scope is observed
        by the core alone, fenced to that configuration: the removed
        components' conditions return to normal and the source quality is
        reconciled without contacting CCLC. With components in scope, CCLC
        being unreachable keeps every raised condition at unknown quality; a
        component whose snapshot is unavailable keeps its conditions
        unchanged. The core judges each observation against the configuration
        Manager publishes at that moment and fences each snapshot to its
        retained bundle and graph revision.

        Unknown quality leaves the whole Alarm Lifecycle Owner unreconciled,
        which blocks motion admission. An Apply that replaces the CCLC owner
        makes it briefly unreachable, so until a complete observation of the
        current scope is accepted the read repeats at the pump cadence
        (bounded below by 0.1 s) instead of waiting a whole period after the
        owner returns. The same short cadence applies from the moment the
        core reports a pending lifecycle observation (a registry replacement
        after an Apply, seen by any reader's refresh), so the read of the new
        configuration does not wait out a period started under the old one.
        An observation with an unread component (`incomplete`) is not
        accepted either and is repeated at the same short cadence.
        """

        if self._cclc is None:
            return
        now = time.monotonic()
        if now < self._next_component_condition_retry_at:
            return
        if (
            not self._component_lifecycle_observation_pending
            and now < self._next_component_condition_poll_at
        ):
            return
        self._next_component_condition_retry_at = now + min(
            self._component_condition_poll_seconds, max(self._poll_seconds, 0.1)
        )
        self._next_component_condition_poll_at = (
            self._next_component_condition_retry_at
        )
        try:
            scope = self._core.component_lifecycle_condition_scope()
            component_ids = scope.get("component_ids")
            source_registered = scope.get("source_registered")
            retained_bundle_id = scope.get("retained_bundle_id")
            graph_revision = scope.get("graph_revision")
            if (
                not isinstance(component_ids, list)
                or not all(isinstance(value, str) and value for value in component_ids)
                or type(source_registered) is not bool
                or (retained_bundle_id is None) != (graph_revision is None)
                or not all(
                    value is None or (isinstance(value, str) and value)
                    for value in (retained_bundle_id, graph_revision)
                )
            ):
                raise HeadlessApiError(
                    "headless_component_lifecycle_invalid",
                    "component lifecycle condition scope is invalid",
                )
            if not component_ids and not source_registered:
                # Nothing in scope and no source to reconcile.
                self._component_lifecycle_observation_pending = False
                self._next_component_condition_poll_at = (
                    now + self._component_condition_poll_seconds
                )
                return
            if not component_ids:
                # No controlled components: no CCLC owner to read. A scope read
                # before any publication has nothing to fence to; retry.
                if retained_bundle_id is None:
                    return
                result = self._core.observe_empty_component_lifecycle_configuration(
                    retained_bundle_id=retained_bundle_id,
                    graph_revision=graph_revision,
                )
                if result.get("status") in {"observed", "empty"}:
                    self._component_lifecycle_observation_pending = False
                    self._next_component_condition_poll_at = (
                        now + self._component_condition_poll_seconds
                    )
                return
            try:
                identity = self._cclc.owner_identity()
                snapshots: list[dict[str, Any]] = []
                unavailable: list[str] = []
                for component_id in component_ids:
                    try:
                        snapshots.append(
                            _component_condition_facts(
                                self._cclc.component_snapshot(component_id)
                            )
                        )
                    except HeadlessApiError as error:
                        if error.code == "cclc_owner_unavailable":
                            raise
                        unavailable.append(component_id)
                # A replaced owner between reads would mix two epochs.
                if self._cclc.owner_identity() != identity:
                    return
            except HeadlessApiError as error:
                if error.code != "cclc_owner_unavailable":
                    raise
                self._core.mark_component_lifecycle_source_unknown()
                return
            result = self._core.observe_component_lifecycle_conditions(
                cclc_owner_epoch=identity["request_owner_epoch"],
                snapshots_json=_canonical_json(snapshots).decode("utf-8"),
                unavailable_component_ids=unavailable,
            )
            # `scope_changed` (an Apply between the scope read and the
            # observation, or a snapshot CCLC took under the superseded
            # configuration), `incomplete` (a component's snapshot could not
            # be read, so quality is unchanged) and `configuration_unavailable`
            # (no fresh, verified Manager publication to judge against)
            # completed nothing; only an accepted observation waits the full
            # period.
            if result.get("status") in {"observed", "empty"}:
                self._component_lifecycle_observation_pending = False
                self._next_component_condition_poll_at = (
                    now + self._component_condition_poll_seconds
                )
        except Exception:
            if not self._component_conditions_failing:
                self._component_conditions_failing = True
                logging.getLogger(__name__).warning(
                    "component lifecycle condition observation failed",
                    exc_info=True,
                )
            return
        if self._component_conditions_failing:
            self._component_conditions_failing = False
            logging.getLogger(__name__).info(
                "component lifecycle condition observation recovered"
            )

    def _record_cclc_incident_events(self) -> None:
        """Copy CCLC incident history events into the historian.

        Same cadence and bounded client as the component-condition read. One
        page per period; a full page is followed immediately by the next one.
        Only new events are recorded: the cursor follows the CCLC owner epoch
        and restarts from zero when it changes, and the core answers an event
        it already holds as a duplicate, so a lost cursor never records one
        twice. A rejected (full-queue) event is retried from the same cursor.
        """

        if self._cclc is None:
            return
        now = time.monotonic()
        if now < self._next_incident_event_poll_at:
            return
        self._next_incident_event_poll_at = now + self._component_condition_poll_seconds
        logger = logging.getLogger(__name__)
        try:
            try:
                identity = self._cclc.owner_identity()["request_owner_epoch"]
                if identity != self._incident_event_owner_epoch:
                    self._incident_event_owner_epoch = identity
                    self._incident_event_after_sequence = 0
                page = self._cclc.incident_events(
                    self._incident_event_after_sequence, MAX_INCIDENT_EVENT_PAGE
                )
                # A replaced owner between reads would mix two journals.
                if self._cclc.owner_identity()["request_owner_epoch"] != identity:
                    return
            except HeadlessApiError as error:
                if error.code == "cclc_owner_unavailable":
                    return
                raise
            for event in page["events"]:
                try:
                    result = self._core.record_cclc_incident_event(
                        cclc_owner_epoch=identity,
                        event_json=_canonical_json(event).decode("utf-8"),
                    )
                except HeadlessApiError as error:
                    if error.code not in {
                        "headless_cclc_incident_event_invalid",
                        "headless_source_invalid",
                        "historian_event_invalid",
                    }:
                        raise
                    # Never block later incidents behind one unrecordable row.
                    logger.error(
                        "CCLC incident event %s was not recordable: %s",
                        event["event_sequence"],
                        error,
                    )
                    result = {"status": "invalid"}
                if result.get("status") == "rejected":
                    return
                self._incident_event_after_sequence = event["event_sequence"]
            if len(page["events"]) == MAX_INCIDENT_EVENT_PAGE:
                self._next_incident_event_poll_at = now
        except Exception:
            if not self._incident_events_failing:
                self._incident_events_failing = True
                logger.warning("CCLC incident event recording failed", exc_info=True)
            return
        if self._incident_events_failing:
            self._incident_events_failing = False
            logger.info("CCLC incident event recording recovered")

    def _notify_change(self) -> None:
        observer = self._change_observer
        if observer is None:
            return
        try:
            observer()
        except Exception:
            # Diagnostics must not backpressure event ingestion. The next
            # source revision or reconnect repairs a missed presentation wake.
            pass

    @property
    def last_error(self) -> str | None:
        with self._health_lock:
            return self._last_error

    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run,
            name="manyforge-operation-event-pump",
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=1.5)
            if thread.is_alive():
                raise RuntimeError("operation event pump did not stop in time")
        self._thread = None

    def _record_error(self, error: Exception | None) -> bool:
        with self._health_lock:
            previous = self._last_error
            if error is None:
                self._last_error = None
            else:
                code = getattr(error, "code", None)
                prefix = f"{type(error).__name__}: "
                if isinstance(code, str) and code:
                    prefix += f"{code}: "
                self._last_error = prefix + str(error)
            current = self._last_error
        if previous == current:
            return False
        logger = logging.getLogger(__name__)
        if current is not None:
            logger.error("Execution event pump unavailable: %s", current)
        else:
            logger.info("Execution event pump recovered")
        return True

    @staticmethod
    def _control_projection_key(event: dict[str, Any]) -> tuple[Any, ...]:
        """Return only operation facts that can change the control shell.

        Native journal rows record lifecycle transitions, not progress-only
        samples. The events response carries a separate in-memory progress
        projection without rebuilding the cold control projection. Lifecycle,
        Stop, runtime-scale, and fault meaning remain in this small key.
        """

        record_kind = event["record_kind"]
        if record_kind in {
            "stop_acceptance_projected",
            "stop_terminal_projected",
            "stop_fault_cleared",
        }:
            # Stop projections are sparse, globally scoped owner facts.  Their
            # exact digest is the simplest complete change identity.
            return (record_kind, event["payload_digest"])

        payload = event["payload"]
        operation = payload["operation_state"]

        def selected(row: dict[str, Any], *names: str) -> tuple[Any, ...]:
            return tuple(row.get(name) for name in names)

        scale = payload.get("runtime_scale_status")
        scale_key = (
            None
            if not isinstance(scale, dict)
            else (
                *selected(
                    scale,
                    "runtime_scale_generation",
                    "state",
                    "requested_scale",
                    "effective_scale",
                ),
                tuple(
                    (
                        *selected(
                            participant,
                            "participant_id",
                            "state",
                            "accepted_generation",
                            "commanded_scale",
                            "applied_scale",
                            "supported",
                        ),
                        rfc8785.dumps(participant.get("error")),
                    )
                    for participant in scale.get("participants", ())
                ),
            )
        )
        return (
            record_kind,
            *selected(
                operation,
                "operation_id",
                "operation_revision",
                "state",
                "active_stop_id",
                "active_stop_generation",
            ),
            tuple(
                (
                    *selected(participant, "participant_id", "state"),
                    rfc8785.dumps(participant.get("terminal_error")),
                )
                for participant in operation.get("participants", ())
            ),
            scale_key,
        )

    @staticmethod
    def _operation_is_settled(event: dict[str, Any]) -> bool:
        """Whether this event leaves its operation's robot at rest.

        The control projection ignores joint samples, so the rest pose after a
        motion would otherwise stay at whatever sample the last unrelated
        control change captured (for example another robot's operation end).
        """

        if event["record_kind"] == "stop_terminal_projected":
            return True
        payload = event.get("payload")
        operation = payload.get("operation_state") if isinstance(payload, dict) else None
        return isinstance(operation, dict) and operation.get("state") in _OPERATION_REST_STATES

    @staticmethod
    def _operation_is_released(event: dict[str, Any]) -> bool:
        if event["record_kind"] != "control_accepted":
            return False
        result = event["payload"].get("result")
        return isinstance(result, dict) and result.get("control") in {
            "acknowledge_completed",
            "reset",
        }

    @staticmethod
    def _idle_owner_epoch(response: dict[str, Any]) -> str | None:
        """Return the typed epoch of an idle owner, if the response proves it."""
        if response.get("ok") is not False:
            return None
        error = response.get("error")
        details = error.get("details") if isinstance(error, dict) else None
        owner_epoch = details.get("owner_epoch") if isinstance(details, dict) else None
        if (
            not isinstance(error, dict)
            or error.get("code") != "execution_owner_not_ready"
            or not isinstance(error.get("message"), str)
            or not error["message"]
            or not isinstance(details, dict)
            or set(details) != {"owner_epoch"}
            or not isinstance(owner_epoch, str)
            or not owner_epoch
            or len(owner_epoch.encode("utf-8")) > 2_048
            or unicodedata.normalize("NFC", owner_epoch) != owner_epoch
        ):
            return None
        return owner_epoch

    def _observe_runtime_fault_snapshot(
        self, snapshot: dict[str, Any], expected_owner_epoch: str
    ) -> None:
        if snapshot["owner_epoch"] != expected_owner_epoch:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "runtime fault observation changed the current native owner",
            )
        result = self._core.observe_runtime_fault_conditions(
            source_owner_epoch=expected_owner_epoch,
            current_runtime_faults_json=_canonical_json(
                snapshot["runtime_faults"]
            ).decode("utf-8"),
        )
        if not isinstance(result, dict) or type(result.get("changed")) is not bool:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "runtime fault observer returned no changed decision",
            )
        if result["changed"]:
            self._notify_change()

    def _run(self) -> None:
        owner_epoch: str | None = None
        after_sequence = 0
        source_high_water_epoch: str | None = None
        source_high_water = 0
        condition_cursor: tuple[str, int] | None = None
        next_condition_poll_at = 0.0
        current_condition_json: str | None = None
        last_source_identity: tuple[str, str | None, str] | None = None
        operation_control_keys: dict[str, tuple[Any, ...]] = {}
        control_projection_changed = False
        admission_generation: int | None = None
        source_mode = "active"

        def condition_snapshot_kwargs(
            source_owner_epoch: str, through_source_sequence: int
        ) -> dict[str, str]:
            nonlocal condition_cursor, current_condition_json
            nonlocal next_condition_poll_at
            now = time.monotonic()
            cursor = (source_owner_epoch, through_source_sequence)
            if cursor == condition_cursor and now < next_condition_poll_at:
                return {}
            response = ExecutionCommandClient.require_success(
                self._execution.stop_fault()
            )
            offer = response["stop_fault"]
            latch = offer["latch"] if isinstance(offer, dict) else None
            condition_cursor = cursor
            next_condition_poll_at = now + self._condition_poll_seconds
            current_condition_json = _canonical_json(latch).decode("utf-8")
            return {"current_stop_fault_json": current_condition_json}

        def publish_if_changed(
            source_owner_epoch: str,
            source_mode: str,
        ) -> None:
            nonlocal control_projection_changed, last_source_identity
            identity = (
                source_owner_epoch,
                current_condition_json,
                source_mode,
            )
            if identity != last_source_identity or control_projection_changed:
                last_source_identity = identity
                control_projection_changed = False
                self._notify_change()

        while not self._stop.is_set():
            self._refresh_alarm_sources()
            self._observe_component_lifecycle_conditions()
            self._record_cclc_incident_events()
            try:
                if owner_epoch is None:
                    identity_response = self._execution.identity()
                    if identity_response.get("ok") is not True:
                        next_epoch = self._idle_owner_epoch(identity_response)
                        if next_epoch is None:
                            ExecutionCommandClient._raise_error_response(identity_response)
                        # An empty product still has the stable Kernel/journal.
                        # Keep the existing idle identity poll, then use events.
                        source_mode = "idle"
                    else:
                        source_mode = "active"
                        status = identity_response.get("identity")
                        next_epoch = (
                            status.get("owner_epoch") if isinstance(status, dict) else None
                        )
                        if not isinstance(next_epoch, str) or not next_epoch:
                            raise HeadlessApiError(
                                "execution_command_response_invalid",
                                "execution status has no owner epoch",
                            )
                    if source_high_water_epoch != next_epoch:
                        source_high_water_epoch = next_epoch
                        source_high_water = 0
                        after_sequence = 0
                        self._global_rules.reset()
                        operation_control_keys.clear()
                        self._progress_rows.clear()
                        self._progress_dirty = True
                    else:
                        after_sequence = source_high_water
                    owner_epoch = next_epoch
                page = self._execution.events(
                    owner_epoch,
                    after_sequence,
                    limit=self._page_limit,
                )
                source_mode = "active" if page["execution_owner_ready"] else "idle"
                self._observe_runtime_fault_snapshot(
                    {"owner_epoch": page["owner_epoch"],
                     "runtime_faults": page["runtime_faults"]},
                    owner_epoch,
                )
                next_admission_generation = page["admission_generation"]
                if admission_generation != next_admission_generation:
                    admission_generation = next_admission_generation
                    control_projection_changed = True
                page_had_events = bool(page["events"])
                self._observe_global_rules(owner_epoch, page)
                for event in page["events"]:
                    self._core.ingest_operation_event(
                        source_owner_epoch=owner_epoch,
                        source_sequence=event["sequence"],
                        record_kind=event["record_kind"],
                        request_id=event["request_id"],
                        operation_id=event["operation_id"],
                        operation_revision=event["operation_revision"],
                        occurred_at_ns=nonnegative_int64(event["occurred_at_ns"]),
                        occurred_at_wall=event["occurred_at_wall"],
                        effect_pending=event["effect_pending"],
                        payload_json=_canonical_json(event["payload"]).decode("utf-8"),
                        payload_digest=event["payload_digest"],
                    )
                    after_sequence = event["sequence"]
                    source_high_water = after_sequence
                    if event["record_kind"] == STATE_EVENT_KIND:
                        # Rule state is not a control projection fact; the
                        # global rule status model already folded it.
                        continue
                    operation_id = event.get("operation_id")
                    if isinstance(operation_id, str):
                        key = self._control_projection_key(event)
                        if (
                            operation_id not in operation_control_keys
                            and len(operation_control_keys)
                            >= MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS
                        ):
                            raise HeadlessApiError(
                                "execution_control_projection_capacity_invalid",
                                "native resident operations exceed the closed bound",
                            )
                        if operation_control_keys.get(operation_id) != key:
                            operation_control_keys[operation_id] = key
                            control_projection_changed = True
                            if self._operation_is_settled(event):
                                self._notify_motion_settled()
                        if self._operation_is_released(event):
                            operation_control_keys.pop(operation_id, None)
                    else:
                        # The only operation-less events are sparse Stop facts.
                        control_projection_changed = True
                        if self._operation_is_settled(event):
                            self._notify_motion_settled()
                if page["next_after_sequence"] != after_sequence:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "operation event pump cursor diverged from its page",
                    )
                self._capture_progress(page["operation_progress"])
                self._publish_progress(time.monotonic())
                if not page["has_more"]:
                    self._core.complete_operation_event_snapshot(
                        source_owner_epoch=owner_epoch,
                        through_source_sequence=after_sequence,
                        **condition_snapshot_kwargs(owner_epoch, after_sequence),
                    )
                    publish_if_changed(owner_epoch, source_mode)
                    if self._record_error(None):
                        self._notify_change()
                    if source_mode == "idle":
                        owner_epoch = None
                    self._stop.wait(self._poll_seconds)
                elif page_had_events:
                    # Do not delay a sparse lifecycle/Stop/fault transition
                    # behind a larger History catch-up.  The downstream cold
                    # combiner still retains only the newest requested value.
                    publish_if_changed(owner_epoch, source_mode)
            except Exception as error:
                runtime_snapshot = getattr(error, "runtime_fault_snapshot", None)
                if runtime_snapshot is not None and owner_epoch is not None:
                    try:
                        self._observe_runtime_fault_snapshot(runtime_snapshot, owner_epoch)
                    except Exception as fault_error:
                        error = RuntimeError(
                            f"{type(error).__name__}: {error}; "
                            "runtime fault observation failed: "
                            f"{type(fault_error).__name__}: {fault_error}"
                        )
                self._progress_rows.clear()
                self._progress_dirty = True
                self._publish_progress(time.monotonic())
                self._global_rules.unknown()
                self._publish_global_rules()
                if owner_epoch is not None:
                    try:
                        quality = self._core.mark_operation_event_source_unknown(
                            source_owner_epoch=owner_epoch,
                        )
                        if quality.get("status") == "marked_unknown":
                            self._notify_change()
                    except Exception as quality_error:
                        error = RuntimeError(
                            f"{type(error).__name__}: {error}; "
                            "operation source-quality update failed: "
                            f"{type(quality_error).__name__}: {quality_error}"
                        )
                if self._record_error(error):
                    self._notify_change()
                owner_epoch = None
                after_sequence = source_high_water
                self._stop.wait(self._poll_seconds)


__all__ = [
    "ExecutionCommandClient",
    "ExecutionEventPump",
    "ExecutionObservationSession",
    "ExecutionPort",
]
