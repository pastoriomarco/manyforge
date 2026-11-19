"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

from collections import deque
import hashlib
import logging
import math
import os
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
    ResolvedPreviewTcp,
    RecoveryPreflightProjection,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    ProgramOperationQuery,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    SHA256_PATTERN,
    SceneRevisionRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from .activity_history import component_action_record, record_activity
from .global_rule_status import STATE_EVENT_KIND, GlobalRuleStatusModel
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object
from manyforge_behavior.json_scalars import nonnegative_int64, nonnegative_int64_value
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)
from .execution_event_stream import follow_execution_events, open_event_subscription


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_CONTROL_SOCKET_FILE = "execution-control.sock"
EXECUTION_OBSERVATION_SOCKET_FILE = "execution-observation.sock"
# Component actions, the component model and operator-alarm attention.
EXECUTION_COMPONENT_SOCKET_FILE = "execution-component.sock"
ACTIVE_TCP_EVENT_KIND = "active_tcp_changed"
# The host's component model facts in every ``events`` reply and stream
# ``state``: a change of any wakes the control
# state. Signal samples count by value, source and freshness, not by age.
COMPONENT_MODEL_STATE_FIELDS = (
    "cell_latch",
    "component_states",
    "execution_blocks",
    "rule_holds",
    "signal_samples",
)
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v4"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v6"
# The Kernel's selection-request schema, bumped with the removal of the
# runner owner epoch and claim generation (contract 1.9).
PROGRAM_EXECUTION_SELECTION_VERSION = "manyforge.program_execution_selection.v3"
VOLATILE_MOTION_TRACE_VERSION = "manyforge.volatile_motion_trace.v2"
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_REQUEST_KEY_TEXT = re.compile(r"[0-9a-f]{32}")
OPERATION_EVENT_KINDS = frozenset(
    {
        "operation_accepted",
        "control_accepted",
        "stop_event_accepted",
        "stop_event_completed",
        "runtime_scale_accepted",
        "operation_prepared",
        "state_observed",
        # Kernel global rule state rows: folded by the global
        # rule status projection and ingested by the historian unchanged.
        STATE_EVENT_KIND,
        # Active TCP: one row per confirmed change; the Runner's mirror
        # reads it, the historian ingests it unchanged (History event).
        ACTIVE_TCP_EVENT_KIND,
    }
)


# ``read_stop`` reply members (``manyforge.execution_stop_status.v2``).
_STOP_STATUS_MEMBERS = frozenset(
    {
        "accepted_at_ns",
        "applied",
        "completed_at_ns",
        "covered",
        "event_id",
        "request_key",
        "requested",
        "result",
        "schema_version",
        "state",
        "widen_reason",
    }
)
_STOP_EVENT_RESULTS = frozenset(
    {"pending", "no_effect", "stopped", "stopped_escalated", "some_failed"}
)


def _is_request_key(value: Any) -> bool:
    return isinstance(value, str) and _REQUEST_KEY_TEXT.fullmatch(value) is not None


_PLANNING_JOB_MEMBERS = frozenset(
    {"error", "job_id", "kind", "planning_result", "schema_version", "state"}
)
_PLANNING_JOB_STATES = frozenset({"accepted", "planning", "done", "failed", "cancelled"})
# The host bounds a job name as it bounds a pick/place `sequence_id`.
_MAXIMUM_PLANNING_JOB_ID_BYTES = 96


def _require_planning_job_id(job_id: Any) -> str:
    if (
        not isinstance(job_id, str)
        or not job_id
        or len(job_id.encode("utf-8")) > _MAXIMUM_PLANNING_JOB_ID_BYTES
    ):
        raise ValueError("planning job id must be 1 to 96 bytes of text")
    return job_id


def _valid_planning_result(result: Any, expected_kind: str) -> bool:
    """The `planning_result` a done pick/place planning job carries."""

    return (
        isinstance(result, dict)
        and result.get("schema_version") == "manyforge.pick_place_planning_result.v1"
        and result.get("kind") == expected_kind
        and result.get("status") in {"planned", "no_feasible_candidate"}
        and type(result.get("attempted_candidate_count")) is int
        and 0 <= result["attempted_candidate_count"] <= 4096
        and isinstance(result.get("reports"), list)
        and len(result["reports"]) <= result["attempted_candidate_count"]
        and not (
            expected_kind == "pick"
            and result["attempted_candidate_count"] > 0
            and not isinstance(result.get("last_attempted_candidate"), dict)
        )
        and not (
            expected_kind == "place"
            and result.get("last_attempted_candidate") is not None
        )
        and not (result["status"] == "planned" and not isinstance(result.get("plan"), dict))
        and not (result["status"] == "no_feasible_candidate" and result.get("plan") is not None)
    )


def _validated_planning_job(
    response: dict[str, Any], command: str, job_id: str | None = None
) -> dict[str, Any]:
    """Check the `planning_job` of an accepted plan, read or cancel reply.

    A job carries its `planning_result` only when `done` and its typed `error`
    only when `failed`.
    """

    if response.get("ok") is not True:
        return response
    job = response.get("planning_job")
    if (
        response.get("command") != command
        or not isinstance(job, dict)
        or set(job) != _PLANNING_JOB_MEMBERS
        or job.get("schema_version") != "manyforge.planning_job.v1"
        or (job_id is not None and job.get("job_id") != job_id)
        or job.get("kind") not in {"pick", "place"}
        or job.get("state") not in _PLANNING_JOB_STATES
        or (job["state"] == "done") != (job.get("planning_result") is not None)
        or (job["state"] == "failed") != isinstance(job.get("error"), dict)
        or (job["state"] != "failed" and job.get("error") is not None)
        or (
            job["state"] == "done"
            and not _valid_planning_result(job["planning_result"], job["kind"])
        )
    ):
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "pick/place planning job has the wrong kind or shape",
        )
    return response


def _is_counter(value: Any) -> bool:
    return type(value) is int and 1 <= value <= MAXIMUM_SAFE_INTEGER


def _valid_operation_event_identity(event: dict[str, Any]) -> bool:
    operation_id = event.get("operation_id")
    operation_revision = event.get("operation_revision")
    if event.get("record_kind") in {
        "stop_event_accepted",
        "stop_event_completed",
        STATE_EVENT_KIND,
        ACTIVE_TCP_EVENT_KIND,
    }:
        return (
            operation_id is None
            and operation_revision is None
            and event.get("request_key") is None
        )
    return _is_counter(operation_id) and _is_counter(operation_revision)


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
# Mirrors the native operation journal's closed maximum: the bound of the
# resident operations' progress rows.
MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS = 4096
MOTION_PREVIEW_RESPONSE_KEYS = frozenset(
    {"api_version", "command", "ok", "preview_receipt"}
)
MOTION_PREVIEW_RECEIPT_KEYS = frozenset(
    {
        "collision_world_revision",
        "commit_eligible",
        "final_positions_rad",
        "final_tcp_pose",
        "graph_revision",
        "intent_digest",
        "program_binding_id",
        "program_id",
        "program_revision",
        "program_run_id",
        "receipt_digest",
        "request_key",
        "required_capability_id",
        "retained_bundle_id",
        "scene_revision",
        "schema_version",
        "source_joint_names",
        "start_positions_rad",
        "target",
    }
)
MOTION_PREVIEW_V2_RECEIPT_KEYS = MOTION_PREVIEW_RECEIPT_KEYS | {"path_preview", "resolved_tcp"}
MOTION_PATH_PREVIEW_MAX_BYTES = 48 * 1024


def _motion_path_preview_valid(value: Any, expected_names: list[str] | None = None, *, diagnostic: bool = False) -> bool:
    """Validate bounded, display-only path evidence; unknown is never clear."""
    def finite_number(value: Any) -> bool:
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            return False
        try:
            return math.isfinite(value)
        except OverflowError:
            return False

    keys = {"source_joint_names", "pairs", "samples"}
    if diagnostic:
        keys |= {"schema_version", "kind", "reason"}
    if not isinstance(value, dict) or set(value) != keys:
        return False
    if diagnostic and (value["schema_version"] != "manyforge.motion_preview_diagnostic.v2"
                       or value["kind"] not in ("validated_path", "collision_disabled_path", "collision_disabled_ik_pose", "collision_disabled_ik_path")
                       or not isinstance(value["reason"], str) or not value["reason"]):
        return False
    names, pairs, samples = value["source_joint_names"], value["pairs"], value["samples"]
    if (not isinstance(names, list) or not 1 <= len(names) <= 256
            or not all(isinstance(n, str) and n for n in names)
            or len(set(names)) != len(names)
            or (expected_names is not None and names != expected_names)
            or not isinstance(pairs, list) or len(pairs) > 64
            or not isinstance(samples, list) or not 1 <= len(samples) <= 32):
        return False
    def valid_body(value: Any) -> bool:
        if not isinstance(value, str) or not value:
            return False
        try:
            value.encode("utf-8", errors="strict")
        except UnicodeError:
            return False
        return True

    pair_keys = []
    for pair in pairs:
        if not isinstance(pair, dict) or set(pair) != {"body_a", "body_b"}:
            return False
        a, b = pair["body_a"], pair["body_b"]
        if not valid_body(a) or not valid_body(b) or a >= b:
            return False
        pair_keys.append((a, b))
    if pair_keys != sorted(set(pair_keys)):
        return False
    previous = -1.0
    for sample in samples:
        sample_keys = {"time_from_start_s", "positions_rad", "contacts", "status"}
        if diagnostic:
            sample_keys.add("clearance_violations")
        if not isinstance(sample, dict) or set(sample) != sample_keys:
            return False
        timestamp, positions, contacts = sample["time_from_start_s"], sample["positions_rad"], sample["contacts"]
        if (not finite_number(timestamp) or timestamp < 0 or timestamp <= previous
                or sample["status"] not in ("checked", "unknown")
                or not isinstance(positions, list) or len(positions) != len(names)
                or not all(finite_number(n) for n in positions)
                or not isinstance(contacts, list) or len(contacts) > len(pairs)):
            return False
        previous = timestamp
        seen = set()
        for contact in contacts:
            if not isinstance(contact, dict) or set(contact) != {"pair_index", "disposition"}:
                return False
            index = contact["pair_index"]
            if (type(index) is not int or not 0 <= index < len(pairs) or index in seen
                    or contact["disposition"] not in ("allowed", "refused", "unknown")):
                return False
            seen.add(index)
        if diagnostic:
            violations = sample["clearance_violations"]
            if not isinstance(violations, list) or len(violations) > len(pairs):
                return False
            seen_violations = set()
            for violation in violations:
                if not isinstance(violation, dict) or set(violation) != {"pair_index", "distance_m", "required_clearance_m"}:
                    return False
                index = violation["pair_index"]
                distance, required = violation["distance_m"], violation["required_clearance_m"]
                if (type(index) is not int or not 0 <= index < len(pairs) or index in seen_violations
                        or not finite_number(distance) or not finite_number(required)
                        or not 0 < distance < required):
                    return False
                seen_violations.add(index)
    if diagnostic and ((value["kind"] == "collision_disabled_ik_pose" and len(samples) != 1)
                       or (value["kind"] == "collision_disabled_ik_path" and len(samples) < 2)):
        return False
    return True


MOTION_PREVIEW_COLLISION_REVISION_KEYS = frozenset({"content_hash", "revision_id"})
CAPABILITY_COMMAND_PREVIEW_RESPONSE_KEYS = frozenset(
    {"api_version", "command", "ok", "preview_receipt"}
)
CAPABILITY_COMMAND_PREVIEW_RECEIPT_KEYS = frozenset(
    {
        "command_schema_id",
        "command_schema_revision",
        "commit_eligible",
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
        "request_key",
        "required_capability_id",
        "retained_bundle_id",
        "schema_version",
        "source_coordinate_name",
        "source_kind",
        "target_position",
        "target",
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

    def subscribe_events(self, after_cursor: int) -> Any: ...

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

    def read(self, operation_id: int) -> dict[str, Any]: ...

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

    def read_planning_job(self, job_id: str) -> dict[str, Any]: ...

    def cancel_planning_job(self, job_id: str) -> dict[str, Any]: ...

    def release_planning_job(self, job_id: str) -> dict[str, Any]: ...

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

    def stop_all(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def halt(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def recognized_stop(
        self,
        *,
        request_key: str,
        recognized_route: str,
        reason: str,
        source_label: str | None,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def component_model(self) -> dict[str, Any]: ...

    def component_action(self, component_id: str, action: str) -> dict[str, Any]: ...

    def component_recovery_preflight(self, component_id: str) -> dict[str, Any]: ...

    def component_recover_enable(self, component_id: str, confirmation: dict[str, Any]) -> dict[str, Any]: ...

    def set_io_output(
        self, signal_id: str, value: bool | float, *, origin: str
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
        after_sequence: int,
        *,
        limit: int = 16,
    ) -> dict[str, Any]: ...

    def read_volatile_motion_trace(self, allowance_bytes: int) -> dict[str, Any]: ...


def _canonical_json(value: dict[str, Any]) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            "execution_command_invalid",
            f"execution command is not canonically encodable: {exc}",
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
        operation = "resolve_socket"
        try:
            context = self._run_root.socket_address(self._socket_path.name)
            socket_address = context.__enter__()
            operation = "socket"
            client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            operation = "setsockopt"
            client.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_RCVBUF,
                MAX_EXECUTION_RESPONSE_BYTES,
            )
            # A short receive timeout only makes cancellation observable. It
            # never causes another request or owner read.
            operation = "settimeout"
            client.settimeout(0.25)
            operation = "connect"
            client.connect(socket_address)
        except Exception as exc:
            logging.getLogger(__name__).warning(
                "Native observation transport failed: syscall=%s errno=%s peer_socket=%s exception_type=%s",
                operation, getattr(exc, "errno", None), self._socket_path.name, type(exc).__name__,
            )
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
            set(cursor) != {"application_start_id", "source_epoch", "source_sequence"}
            or not isinstance(cursor.get("application_start_id"), str)
            or not cursor["application_start_id"]
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
            "after_application_start_id": (
                None if cursor is None else cursor["application_start_id"]
            ),
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
        operation = "send"
        try:
            sent = client.send(encoded)
            if sent != len(encoded):
                raise HeadlessApiError(
                    "execution_command_transport_failure",
                    "execution observation request was truncated",
                )
            while not cancelled.is_set():
                try:
                    operation = "recvmsg"
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
            logging.getLogger(__name__).warning(
                "Native observation transport failed: syscall=%s errno=%s peer_socket=%s exception_type=%s",
                operation, getattr(exc, "errno", None), self._socket_path.name, type(exc).__name__,
            )
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
    reason. The native request context is ``{request_key, source_label}``
    with the key Headless formed; ``A`` was checked at the edge and is not
    forwarded.
    """

    payload = request.model_dump(mode="json", exclude_none=True)
    payload["request_context"] = request.request_context.model_dump(
        mode="json",
        include={"request_key", "source_label"},
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
        # ``timeout_seconds`` limits the connection only; see `_exchange_on`.
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
        self._component_socket_path = (
            self._run_root.path / EXECUTION_COMPONENT_SOCKET_FILE
        )
        self._timeout_seconds = timeout_seconds
        self._attribution_transport = attribution_transport

    def _receive_timeout(self, command: dict[str, Any]) -> float | None:
        """The limit on sending and receiving once connected: none, the
        request waits for its answer (OD-214)."""

        del command
        return None

    def _exchange_on(
        self, command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_EXECUTION_PACKET_BYTES:
            raise HeadlessApiError(
                "execution_command_oversized",
                "execution command exceeds its byte bound",
            )
        operation = "resolve_socket"
        try:
            with self._run_root.socket_address(socket_path.name) as socket_address:
                operation = "socket"
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    operation = "setsockopt"
                    client.setsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_RCVBUF,
                        MAX_EXECUTION_RESPONSE_BYTES,
                    )
                    operation = "settimeout"
                    client.settimeout(self._timeout_seconds)
                    operation = "connect"
                    client.connect(socket_address)
                    # Only the connection is limited (OD-214): the request
                    # then waits for its answer. A host exit closes the
                    # socket; a late host is the Manager's status probe's.
                    operation = "settimeout"
                    client.settimeout(self._receive_timeout(command))
                    operation = "send"
                    sent = client.send(encoded)
                    if sent != len(encoded):
                        raise HeadlessApiError(
                            "execution_command_transport_failure",
                            "execution command packet was truncated",
                        )
                    operation = "recvmsg"
                    payload, _ancillary, flags, _address = client.recvmsg(
                        MAX_EXECUTION_RESPONSE_BYTES
                    )
                    operation = "validate_socket"
        except HeadlessApiError:
            raise
        except PrivateTransportCapabilityError as exc:
            logging.getLogger(__name__).warning(
                "Native execution transport failed: syscall=%s errno=%s peer_socket=%s exception_type=%s",
                operation, getattr(exc, "errno", None), socket_path.name, type(exc).__name__,
            )
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution command socket is unavailable: {exc}",
            ) from exc
        except (OSError, TimeoutError) as exc:
            logging.getLogger(__name__).warning(
                "Native execution transport failed: syscall=%s errno=%s peer_socket=%s exception_type=%s",
                operation, getattr(exc, "errno", None), socket_path.name, type(exc).__name__,
            )
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution transport failed: {exc}",
            ) from exc
        return _decode_execution_response(payload, flags)

    def stop_program_run(
        self,
        request: ProgramRunStopIntentRequest,
    ) -> dict[str, Any]:
        """Submit the private Program-owner selector on the Stop lane.

        Only the Program runner calls it (Forced Program End); the Kernel
        accepts the program-run selector only from internal callers.
        """

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
        if response.get("ok") is True and (
            response.get("command") != "stop_program_run"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Program-run Stop acceptance has the wrong kind or shape",
            )
        return response

    def stop_program_selection(
        self,
        request: Any,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Stop (level 1) of named groups of one Program run, on the Stop lane.

        Headless sends it straight to the Kernel with the operator's
        attribution (contract 1.8; K-10 accepts non-internal callers).
        """

        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "stop_program_selection",
                "request": {
                    "schema_version": PROGRAM_EXECUTION_SELECTION_VERSION,
                    "request_context": {
                        "request_key": request.request_key,
                        "source_label": request.source_label,
                    },
                    "program_run_id": request.program_run_id,
                    "group_ids": list(request.group_ids),
                },
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "stop_program_selection"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Program selection Stop acceptance has the wrong kind or shape",
            )
        return response

    def release_halt(
        self,
        program_run_id: int,
        expected_hold_revision: int,
    ) -> dict[str, Any]:
        """Execute after Halt: lower the run's freeze at one hold revision."""

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "Halt release is available only to the internal owner transport",
            )
        if not _is_counter(program_run_id):
            raise ValueError("Program-run number is outside its safe bound")
        if (
            type(expected_hold_revision) is not int
            or not 0 <= expected_hold_revision <= MAXIMUM_SAFE_INTEGER
        ):
            raise ValueError("hold revision is outside its safe bound")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(None),
                "command": "release_halt",
                "expected_hold_revision": expected_hold_revision,
                "program_run_id": program_run_id,
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "release_halt"
            # scope is null when the run is not the Kernel's current run (no effect).
            or "scope" not in response
            or not isinstance(response["scope"], (dict, type(None)))
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Halt release response has the wrong command kind or shape",
            )
        return response

    def read_stop(self, event_id: int) -> dict[str, Any]:
        """Read one Stop event's record for the internal Program owner.

        Served beside Stop on the independent control socket, so waiting on
        a Stop never queues behind ordinary or planning traffic.
        """

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "Stop-status read is available only to the internal owner transport",
            )
        if not _is_counter(event_id):
            raise ValueError("Stop event id is outside its safe bound")
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(None),
                "command": "read_stop",
                "event_id": event_id,
            }
        )
        if response.get("ok") is not True:
            return response
        status = response.get("stop_status")
        if (
            response.get("command") != "read_stop"
            or not isinstance(status, dict)
            or set(status) != _STOP_STATUS_MEMBERS
            or status.get("schema_version") != "manyforge.execution_stop_status.v2"
            or status.get("event_id") != event_id
            or not _is_request_key(status.get("request_key"))
            or status.get("state") not in {"pending", "complete"}
            or status.get("result") not in _STOP_EVENT_RESULTS
            or not isinstance(status.get("covered"), list)
            or (status["state"] == "complete")
            != (status.get("completed_at_ns") is not None)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native Stop event status has an invalid envelope",
            )
        return response

    def subscribe_events(self, after_cursor: int):
        """Open the event subscription stream strictly after ``after_cursor``;
        a context manager yielding the subscription."""

        return open_event_subscription(self._run_root, after_cursor)

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._socket_path)

    def _exchange_control(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._control_socket_path)

    def _exchange_component(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._component_socket_path)

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
            or not isinstance(operation, dict)
            or not _is_counter(operation.get("operation_id"))
            or type(operation.get("operation_revision")) is not int
            or not 1 <= operation["operation_revision"] <= MAXIMUM_SAFE_INTEGER
            or not isinstance(operation.get("participants"), list)
            or len(operation["participants"]) != 1
            or not isinstance(operation["participants"][0], dict)
        ):
            raise HeadlessApiError(
                "execution_operation_context_invalid",
                "operation context response is malformed",
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
        expected["schema_version"] = "manyforge.target_observation.v3"
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
            or observation.get("schema_version") != expected["schema_version"]
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
                "native target query returned a malformed observation",
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
            "schema_version": "manyforge.target_observation.v3",
            "program_binding_id": request.program_binding_id,
            "kind": "io_signal",
            "signal": request.signal.model_dump(mode="json"),
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
                or observation.get("schema_version") != expected["schema_version"]
            ):
                raise ValueError("I/O signal answer has the wrong kind or shape")
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
                or observation.get("schema_version") != expected["schema_version"]
            ):
                raise ValueError("frame transform answer has the wrong kind or shape")
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
            and (
                "producer" not in error
                or (isinstance(error.get("producer"), str) and error["producer"])
            )
        ):
            raise HeadlessApiError(
                error["code"], error["message"], details=error.get("details"),
                producer=error.get("producer", "kernel"),
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
    def _event_runtime_fault_snapshot(response: dict[str, Any]) -> dict[str, Any]:
        """Validate common provenance before interpreting page success/failure.

        The Headless owner validates the complete closed A/B fact schema before
        mutation. This transport never infers a condition from diagnostic text.
        """
        faults = response.get("runtime_faults")
        if (
            response.get("command") != "events"
            or type(response.get("execution_owner_ready")) is not bool
            or not isinstance(faults, dict)
            or set(faults)
            != {"operation_journal_write_fault", "program_runner_loss", "kernel_internal_fault"}
            or any(
                fact is not None and not isinstance(fact, dict)
                for fact in faults.values()
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution host fault snapshot has invalid provenance",
            )
        return {"runtime_faults": faults,
                "execution_owner_ready": response["execution_owner_ready"]}

    def events(
        self,
        after_sequence: int,
        *,
        limit: int = 16,
    ) -> dict[str, Any]:
        if (
            type(after_sequence) is not int
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
            }
        )
        return self.parse_events_reply(response, after_sequence, limit)

    @classmethod
    def parse_events_reply(
        cls,
        response: dict[str, Any],
        after_sequence: int,
        limit: int,
    ) -> dict[str, Any]:
        """Validate one ``events`` reply (also one the event stream rebuilt)
        and return its page with the reply's facts."""

        runtime_snapshot = cls._event_runtime_fault_snapshot(response)
        try:
            if response.get("ok") is not True:
                cls._raise_error_response(response)
            page = response.get("event_page")
            admission_generation = response.get("admission_generation")
            if (
                response.get("command") != "events"
                or type(admission_generation) is not int
                or not 1 <= admission_generation <= MAXIMUM_SAFE_INTEGER
                or not isinstance(page, dict)
                or page.get("schema_version") != "manyforge.execution_event_page.v3"
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
                    or type(event.get("sequence")) is not int
                    or not previous < event["sequence"] <= MAXIMUM_SAFE_INTEGER
                    or event.get("record_kind") not in OPERATION_EVENT_KINDS
                    or (
                        event.get("request_key") is not None
                        and not _is_request_key(event["request_key"])
                    )
                    or not _valid_operation_event_identity(event)
                    or not isinstance(event.get("occurred_at_wall"), str)
                    or not event["occurred_at_wall"]
                    or len(event["occurred_at_wall"].encode("utf-8")) > 128
                    or unicodedata.normalize("NFC", event["occurred_at_wall"])
                    != event["occurred_at_wall"]
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
            seen: set[int] = set()
            for row in progress:
                if (
                    not isinstance(row, dict)
                    or set(row) != {"operation_id", "operation_revision", "progress"}
                    or not _is_counter(row["operation_id"])
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
            page["active_tcp_baseline"] = response.get("active_tcp_baseline")
            # Stream `state` only (OD-205 (b)): the Kernel's operations table
            # and Stop facts from memory; absent from an `events` reply.
            operations = response.get("operations")
            if operations is not None and (
                not isinstance(operations, dict)
                or type(operations.get("change")) is not int
                or not 0 <= operations["change"] <= MAXIMUM_SAFE_INTEGER
                or not isinstance(operations.get("faulted"), list)
                or not isinstance(operations.get("resident"), list)
                or type(operations.get("resident_total")) is not int
                or not len(operations["resident"])
                <= operations["resident_total"]
                <= MAXIMUM_SAFE_INTEGER
            ):
                raise HeadlessApiError(
                    "execution_command_response_invalid",
                    "native operations table is invalid",
                )
            page["operations"] = operations
            page["last_stop_event_id"] = response.get("last_stop_event_id")
            page["stop_progress_sequence"] = response.get("stop_progress_sequence")
            page["component_model"] = {
                name: response.get(name) for name in COMPONENT_MODEL_STATE_FIELDS
            }
            samples = page["component_model"]["signal_samples"]
            if samples is not None:
                page["component_model"]["signal_samples"] = [
                    {key: value for key, value in sample.items() if key != "age_ns"}
                    for sample in samples
                ]
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
        if response.get("ok") is True and (
            response.get("command") != "submit"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution acceptance has the wrong kind or shape",
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
                "request_key",
            }
            or response.get("command") != "read_manual_handover"
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "manual handover response has the wrong kind or shape",
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
                    "execution_command_response_invalid",
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
        participant = operation["participants"][0]
        if response.get("ok") is not True:
            diagnostic = response.get("diagnostic_preview")
            if "diagnostic_preview" in response:
                motion = participant["payload"].get("motion")
                expected = motion.get("joint_names") if isinstance(motion, dict) and motion.get("kind") == "joint_goal" else None
                try:
                    valid = (not ({"preview_receipt", "receipt_digest", "token", "approval_token", "commit_eligible"} & response.keys())
                             and _motion_path_preview_valid(diagnostic, expected, diagnostic=True)
                             and len(_canonical_json(response)) <= MOTION_PATH_PREVIEW_MAX_BYTES)
                except (ValueError, TypeError, HeadlessApiError):
                    valid = False
                if not valid:
                    response = {key: value for key, value in response.items() if key != "diagnostic_preview"}
            # A failed preview is never an approval receipt, even if the native
            # response contains undeclared receipt or token fields.
            forbidden = {"preview_receipt", "receipt_digest", "token", "approval_token", "commit_eligible"}
            if forbidden.intersection(response):
                response = {key: value for key, value in response.items() if key not in forbidden}
            return response
        receipt = response.get("preview_receipt")
        version = receipt.get("schema_version") if isinstance(receipt, dict) else None
        receipt_keys = MOTION_PREVIEW_V2_RECEIPT_KEYS
        if (
            set(response) != MOTION_PREVIEW_RESPONSE_KEYS
            or not isinstance(receipt, dict)
            or set(receipt) != receipt_keys
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview response has an undeclared shape",
            )
        if (
            response.get("command") != "preview_motion"
            or receipt.get("target") != participant["target"]
            or version != "manyforge.direct_control_motion_preview.v5"
            or receipt.get("commit_eligible") is not True
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "motion preview response has the wrong kind or is not commit eligible",
            )
        source_names = receipt.get("source_joint_names")
        submitted_motion = participant["payload"].get("motion")
        try:
            resolved_tcp = ResolvedPreviewTcp.model_validate(receipt["resolved_tcp"])
        except ValueError as exc:
            raise HeadlessApiError(
                "execution_command_response_invalid", "native motion preview TCP is malformed"
            ) from exc
        tcp_selector = participant["payload"].get("tcp_endpoint")
        if (
            (resolved_tcp.source == "active_tcp" and tcp_selector != {"kind": "current"})
            or (resolved_tcp.source == "joint_flange_fallback" and
                (not isinstance(submitted_motion, dict) or submitted_motion.get("kind") != "joint_goal"
                 or tcp_selector != {"kind": "current"}))
            or (resolved_tcp.source == "explicit_entry" and
                tcp_selector != {"kind": "entry", **resolved_tcp.entry.model_dump(mode="json")})
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid", "native motion preview TCP provenance is invalid"
            )
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
        if (
            not _motion_path_preview_valid(receipt["path_preview"], source_names)
            or len(_canonical_json(response)) > MOTION_PATH_PREVIEW_MAX_BYTES
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native motion preview path is malformed or oversized",
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
            or receipt.get("target") != participant["target"]
            or receipt.get("schema_version")
            != "manyforge.direct_control_capability_command_preview.v3"
            or receipt.get("commit_eligible") is not True
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "capability-command preview has the wrong kind or is not commit eligible",
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

    def read(self, operation_id: int) -> dict[str, Any]:
        if not _is_counter(operation_id):
            raise ValueError("operation_id must be an operation number")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read",
                "operation_id": operation_id,
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "read"
            or not isinstance(response.get("operation"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution observation has the wrong kind or shape",
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
        if response.get("command") != "read_volatile_motion_trace":
            raise HeadlessApiError("execution_command_response_invalid", "native trace command differs")
        if "trace_file" in response:
            return self._read_motion_trace_file(response["trace_file"], allowance_bytes)
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

    def _read_motion_trace_file(self, metadata: Any, allowance_bytes: int) -> dict[str, Any]:
        directory = os.environ.get("MANYFORGE_MOTION_TRACE_DIR", "")
        if (not directory or not isinstance(metadata, dict)
                or set(metadata) != {"schema_version", "file", "bytes", "sha256", "status"}
                or metadata.get("schema_version") != "manyforge.motion_trace_file.v2"
                or not isinstance(metadata.get("file"), str)
                or re.fullmatch(r"trace-[0-9a-f]{64}\.json", metadata["file"]) is None
                or type(metadata.get("bytes")) is not int
                or not 0 < metadata["bytes"] <= min(allowance_bytes, 5 * 1024 * 1024)
                or metadata.get("sha256") != "sha256:" + metadata["file"][6:-5]
                or not isinstance(metadata.get("status"), str)
                or metadata.get("status") not in {"complete", "partial"}):
            raise HeadlessApiError("motion_trace_file_invalid", "native trace file metadata is invalid or disk traces are unconfigured")
        try:
            payload = PinnedRunRoot(Path(directory)).read_file(metadata["file"], metadata["bytes"])
        except PrivateTransportCapabilityError as error:
            raise HeadlessApiError("motion_trace_file_unavailable", str(error)) from error
        if (len(payload) != metadata["bytes"]
                or "sha256:" + hashlib.sha256(payload).hexdigest() != metadata["sha256"]):
            raise HeadlessApiError("motion_trace_file_invalid", "trace file size or digest differs")
        try:
            trace = strict_json_object(payload.decode("utf-8", errors="strict"), label="motion trace file")
            if (rfc8785.dumps(trace) != payload
                    or trace.get("schema_version") != VOLATILE_MOTION_TRACE_VERSION):
                raise ValueError("trace schema or canonical bytes differ")
        except (HeadlessApiError, ValueError, UnicodeError, rfc8785.CanonicalizationError) as error:
            raise HeadlessApiError("motion_trace_file_invalid", "trace file content is invalid") from error
        return trace

    def operation_recovery(self, operation_id: int) -> dict[str, Any]:
        if not _is_counter(operation_id):
            raise ValueError("operation_id must be an operation number")
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
            != "manyforge.kernel_recovery_assessment.v2"
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
                response.get("stop_event_id") is not None
                and not _is_counter(response.get("stop_event_id"))
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native operation recovery assessment is invalid",
            )
        return response

    def apply_operation_recovery(
        self,
        operation_id: int,
        expected_operation_revision: int,
        request: OperationRecoveryActionRequest,
    ) -> dict[str, Any]:
        if not _is_counter(operation_id):
            raise ValueError("operation_id must be an operation number")
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
            or type(result.get("accepted")) is not bool
            or not isinstance(result.get("resulting_assessment"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native operation recovery result has the wrong kind or shape",
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
        # Answered at acceptance (OD-213 (1)): the job is read, cancelled and
        # released by name; its result arrives with a read once it is done.
        _validated_planning_job(response, command, request.sequence_id)
        if response.get("ok") is True and (
            response["planning_job"]["state"] != "accepted"
            or response["planning_job"]["kind"]
            != ("pick" if command.endswith("pick") else "place")
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "pick/place planning was not answered with an accepted job",
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

    def _planning_job_command(self, command: str, job_id: str) -> dict[str, Any]:
        # Served on the control socket, memory only: never behind planning.
        return self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": command,
                "job_id": _require_planning_job_id(job_id),
            }
        )

    def read_planning_job(self, job_id: str) -> dict[str, Any]:
        """One pick/place planning job's state, and its result once done."""

        return _validated_planning_job(
            self._planning_job_command("read_planning_job", job_id),
            "read_planning_job",
            job_id,
        )

    def cancel_planning_job(self, job_id: str) -> dict[str, Any]:
        """Cancel this planning job only; returns its state after the cancel."""

        return _validated_planning_job(
            self._planning_job_command("cancel_planning_job", job_id),
            "cancel_planning_job",
            job_id,
        )

    def release_planning_job(self, job_id: str) -> dict[str, Any]:
        """Drop a finished planning job (done, failed or cancelled)."""

        response = self._planning_job_command("release_planning_job", job_id)
        if response.get("ok") is True and (
            response.get("command") != "release_planning_job"
            or set(response) != {"api_version", "command", "ok"}
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "planning job release response has the wrong command kind or shape",
            )
        return response

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
        if response.get("ok") is True and (
            response.get("command") != command
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution control acceptance has the wrong kind or shape",
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
        if response.get("ok") is True and (
            response.get("command") != "stop"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution Stop acceptance has the wrong kind or shape",
            )
        return response

    def _cell_wide_stop(
        self,
        command: str,
        request: FullStopIntentRequest,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": command,
                "request": request.model_dump(mode="json"),
            }
        )
        if response.get("ok") is True and (
            response.get("command") != command
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                f"{command} acceptance has the wrong kind or shape",
            )
        return response

    def stop_all(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Stop all: every operation in the cell; holds every group."""

        return self._cell_wide_stop("stop_all", request, trusted_principal_id)

    def halt(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Halt: Stop all plus the Program frozen; names no run."""

        return self._cell_wide_stop("halt", request, trusted_principal_id)

    def cell_stop(self, request: FullStopIntentRequest) -> dict[str, Any]:
        """The Program node's cell Stop (``kernel.cell_stop``): internal only."""

        if self._attribution_transport != "internal":
            raise HeadlessApiError(
                "execution_request_attribution_invalid",
                "cell Stop is available only to the internal owner transport",
            )
        return self._cell_wide_stop("cell_stop", request, None)

    def recognized_stop(
        self,
        *,
        request_key: str,
        recognized_route: str,
        reason: str,
        source_label: str | None,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """A Stop recognized on its route but not forwarded as asked (its
        body cannot be read, or it names another application start or none):
        the Kernel widens it by the route. ``source_label`` is the caller's
        validated label, or None."""

        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "recognized_stop",
                "reason": reason,
                "recognized_route": recognized_route,
                "request_key": request_key,
                "source_label": source_label,
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "recognized_stop"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "recognized Stop acceptance has the wrong kind or shape",
            )
        return response

    def component_model(self) -> dict[str, Any]:
        """Every ComponentState, the CellLatch, every
        SignalSample, the rule holds and the required signal ids, as the
        execution host publishes them (an in-memory read)."""
        return self._read_projection_on(
            "read_component_model", self._component_socket_path
        )

    def component_action(self, component_id: str, action: str) -> dict[str, Any]:
        """Hand one action to the core component coordinator.

        The reply only accepts or refuses (`component_action_in_progress`,
        `component_action_unsupported`, `component_not_found`); the result
        arrives in the component's `last_action`.
        """
        response = self._exchange_component(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "component_action",
                "request": {"component_id": component_id, "action": action},
            }
        )
        return self._validate_projection_response("component_action", response)

    def component_recovery_preflight(self, component_id: str) -> dict[str, Any]:
        response = self._exchange_component({
            "api_version": EXECUTION_COMMAND_VERSION,
            "command": "component_recovery_preflight",
            "request": {"component_id": component_id},
        })
        self._validate_projection_response("component_recovery_preflight", response)
        if response.get("ok") is True:
            try:
                projection = RecoveryPreflightProjection.model_validate(response.get("preflight"))
            except ValueError as error:
                raise HeadlessApiError("execution_command_response_invalid", "recovery preflight is malformed") from error
            if projection.components[-1].component_id != component_id:
                raise HeadlessApiError("execution_command_response_invalid", "recovery preflight names another component")
        return response

    def component_recover_enable(self, component_id: str, confirmation: dict[str, Any]) -> dict[str, Any]:
        response = self._exchange_component({
            "api_version": EXECUTION_COMMAND_VERSION,
            "command": "component_action",
            "request": {"component_id": component_id, "action": "enable", "recovery_preflight": confirmation},
        })
        return self._validate_projection_response("component_action", response)

    def set_io_output(
        self, signal_id: str, value: bool | float, *, origin: str
    ) -> dict[str, Any]:
        """One output write; the reply carries SetOutputResult.
        `origin` is `manual` (refused while motion executes) or `program`."""
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "set_io_output",
                "request": {"signal_id": signal_id, "value": value, "origin": origin},
            }
        )
        result = response.get("result")
        if response.get("ok") is True and (
            response.get("command") != "set_io_output"
            or not isinstance(result, dict)
            or result.get("outcome") not in ("succeeded", "failed", "unknown")
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "set_io_output result has the wrong kind or shape",
            )
        return response

    def set_runtime_scale_control(
        self,
        request: RuntimeScaleControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        # Beside Stop on the control socket: the execution host queues it on
        # its speed-scaling lane, never behind planning or Submit.
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._request_attribution(trusted_principal_id),
                "command": "set_runtime_scale_control",
                "request": request.model_dump(mode="json"),
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "set_runtime_scale_control"
            or not isinstance(response.get("accepted"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "runtime-scale control acceptance has the wrong kind or shape",
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
        if response.get("ok") is True and (
            response.get("command") != "get_runtime_scale_control"
            or not isinstance(response.get("runtime_scale_control"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "runtime-scale control observation has the wrong kind or shape",
            )
        return response


class StreamedExecutionEvents:
    """The event pump's source on the subscription stream.

    One follower thread keeps the subscription (reconnect from the last
    cursor, dead link after three missed heartbeats) and buffers its rows;
    the latest ``state`` and ``progress`` are kept as received. ``identity``
    and ``events`` answer from that buffer in the shapes of the request/reply
    commands, so the pump's ingestion is unchanged and no command is polled;
    ``wait`` returns as soon as a new message arrived. A row stays buffered
    until a later ``events`` call names a cursor at or after it, so a failed
    ingestion re-reads it. The buffer is bounded: a full buffer holds the
    follower (the host then drops and the follower resumes the subscription,
    losing nothing).
    """

    MAX_BUFFERED_EVENTS = 4_096

    def __init__(
        self,
        subscribe: Callable[[int], Any],
        stop: threading.Event,
        application_start_id: str,
    ) -> None:
        self._subscribe = subscribe
        self._stop = stop
        self._changed = threading.Condition()
        # One Kernel and one journal per application start: the stream names
        # no owner, so this start is the event source's identity.
        self._application_start_id = application_start_id
        self._state: dict[str, Any] | None = None
        self._progress: list[Any] = []
        self._events: deque[dict[str, Any]] = deque()
        self._connected = False
        self._version = 0
        self._seen_version = 0
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        self._thread = threading.Thread(
            target=follow_execution_events,
            args=(self._subscribe, self._on_message, self._stop),
            kwargs={"on_disconnect": self._on_disconnect},
            name="manyforge-operation-event-stream",
            # Holds no durable work (the pump owns ingestion and joins it on
            # shutdown); a leaked follower never blocks interpreter exit.
            daemon=True,
        )
        self._thread.start()

    def join(self, timeout: float) -> None:
        thread = self._thread
        if thread is not None:
            thread.join(timeout=timeout)
        self._thread = None

    def _on_message(self, message: dict[str, Any]) -> None:
        with self._changed:
            kind = message["type"]
            if kind == "snapshot":
                self._state = message["state"]
                self._progress = []
                self._connected = True
            elif kind == "state":
                self._state = message["state"]
            elif kind == "progress":
                self._progress = message["operation_progress"]
            elif kind == "event":
                while len(self._events) >= self.MAX_BUFFERED_EVENTS:
                    if self._stop.is_set():
                        return
                    self._changed.wait(0.1)
                self._events.append(message["event"])
            self._version += 1
            self._changed.notify_all()

    def _on_disconnect(self, _error: BaseException) -> None:
        with self._changed:
            self._connected = False
            self._version += 1
            self._changed.notify_all()

    def _require_connected(self) -> dict[str, Any]:
        if not self._connected or self._state is None:
            raise HeadlessApiError(
                "execution_owner_unavailable", "execution event stream is not connected"
            )
        return self._state

    def identity(self) -> dict[str, Any]:
        """The start in the ``identity`` reply shapes the pump reads
        (ready: the identity; no product: ``execution_owner_not_ready``)."""

        with self._changed:
            state = self._require_connected()
            if state.get("execution_owner_ready") is True:
                return {
                    "ok": True,
                    "identity": {"application_start_id": self._application_start_id},
                }
            return {
                "ok": False,
                "error": {
                    "code": "execution_owner_not_ready",
                    "message": "the managed execution owner has no active physical product",
                    "details": {"application_start_id": self._application_start_id},
                },
            }

    def events(self, after_sequence: int, *, limit: int = 16) -> dict[str, Any]:
        """The buffered rows strictly after ``after_sequence`` as one validated
        ``events`` page with the latest stream facts."""

        with self._changed:
            state = self._require_connected()
            while self._events and self._events[0]["sequence"] <= after_sequence:
                self._events.popleft()
            self._changed.notify_all()  # Room for a held follower.
            rows = [self._events[index] for index in range(min(limit, len(self._events)))]
            has_more = len(self._events) > limit
            response = {
                **state,
                "api_version": EXECUTION_RESPONSE_VERSION,
                "command": "events",
                "event_page": {
                    "events": rows,
                    "has_more": has_more,
                    "next_after_sequence": rows[-1]["sequence"] if rows else after_sequence,
                    "schema_version": "manyforge.execution_event_page.v3",
                },
                "ok": True,
                "operation_progress": list(self._progress),
            }
        return ExecutionCommandClient.parse_events_reply(response, after_sequence, limit)

    def wait(self, timeout: float) -> None:
        """Until a new stream message, ``stop`` or ``timeout``."""

        with self._changed:
            self._changed.wait_for(
                lambda: self._version != self._seen_version or self._stop.is_set(),
                timeout=timeout,
            )
            self._seen_version = self._version


class ExecutionEventPump:
    """Move durable native facts to the C++ historian without owning truth."""

    def __init__(
        self,
        core: HeadlessCorePort,
        execution: ExecutionPort,
        *,
        application_start_id: str,
        poll_seconds: float = 0.1,
        page_limit: int = 16,
    ) -> None:
        if not 0.01 <= poll_seconds <= 5.0:
            raise ValueError("operation event polling period is outside [0.01, 5]")
        if not 1 <= page_limit <= 16:
            raise ValueError("operation event page limit is outside [1, 16]")
        self._core = core
        self._execution = execution
        self._poll_seconds = poll_seconds
        self._stop = threading.Event()
        # Identity, pages and wake-ups come from the event
        # subscription; the request/reply `events` command is never polled.
        self._stream = StreamedExecutionEvents(
            execution.subscribe_events, self._stop, application_start_id
        )
        self._page_limit = page_limit
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
        # Each component's last recorded `last_action`; None until the first
        # model of this Headless (a baseline, not recorded again).
        self._recorded_actions: dict[str, Any] | None = None
        # OD-215: the Kernel journal's (queue full, storage failure) levels
        # from the latest runtime faults, for the disk check's own thread
        # (one tuple assignment; no lock).
        self.journal_disk_signals: tuple[bool, bool] = (False, False)

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

    def _observe_global_rules(self, page: dict[str, Any]) -> None:
        # A malformed rule wire makes only this projection unknown; the
        # historian feed continues (the Runner mirror's own recapture rule).
        try:
            self._global_rules.observe(
                page.get("interrupt_rule_catalog"),
                page.get("interrupt_rule_state_baseline"),
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
        """Keep the alarm sources within their currency bound.

        Every pump iteration offers the refresh; the core performs it at most
        once per period R and otherwise returns without its lock. A failed
        refresh is not an operation-feed failure: the motion-attention read
        reports sources older than the bound C as not reconciled.
        """

        try:
            self._core.refresh_alarm_sources()
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
        self._stream.start()
        self._thread = threading.Thread(
            target=self._run,
            name="manyforge-operation-event-pump",
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        self._stream.join(timeout=1.0)
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

    def _record_finished_actions(self, model: dict[str, Any] | None) -> None:
        """One History event per component action the host finished.

        The host publishes each finished action once as the component's
        ``last_action``, whoever asked for it; a new one (it carries its own
        start and finish times) is recorded once, repeated models are not.
        """

        states = (model or {}).get("component_states") or ()
        current = {
            state["component_id"]: state["last_action"]
            for state in states
            if isinstance(state, dict) and isinstance(state.get("last_action"), dict)
        }
        recorded, self._recorded_actions = self._recorded_actions, current
        if recorded is None:
            return
        for component_id, last in current.items():
            if recorded.get(component_id) != last:
                record_activity(
                    self._core,
                    component_action_record(
                        component_id, last["action"], last["outcome"], last.get("error")
                    ),
                )

    @staticmethod
    def _idle_application_start_id(response: dict[str, Any]) -> str | None:
        """Return the start of an idle owner, if the response proves it."""
        if response.get("ok") is not False:
            return None
        error = response.get("error")
        details = error.get("details") if isinstance(error, dict) else None
        start_id = details.get("application_start_id") if isinstance(details, dict) else None
        if (
            not isinstance(error, dict)
            or error.get("code") != "execution_owner_not_ready"
            or not isinstance(error.get("message"), str)
            or not error["message"]
            or not isinstance(details, dict)
            or set(details) != {"application_start_id"}
            or not isinstance(start_id, str)
            or not start_id
            or len(start_id.encode("utf-8")) > 2_048
            or unicodedata.normalize("NFC", start_id) != start_id
        ):
            return None
        return start_id

    def _observe_runtime_fault_snapshot(self, snapshot: dict[str, Any]) -> None:
        fault = snapshot["runtime_faults"].get("operation_journal_write_fault")
        if isinstance(fault, dict):
            error = fault.get("error")
            self.journal_disk_signals = (
                fault.get("queue_full") is True,
                isinstance(error, dict)
                and error.get("code") == "operation_journal_storage_failure",
            )
        else:
            self.journal_disk_signals = (False, False)
        result = self._core.observe_runtime_fault_conditions(
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

    def _observe_component_faults(self, model: dict[str, Any]) -> None:
        """OD-247: one operator alarm per component in fault.

        Passes every component whose published state shows a fault, except
        the simulators (`sim_reset`), as they are left out of the cell latch
        too. The core raises and returns the alarms on edges only. A model
        without component states (no host) is not observed.
        """

        states = model.get("component_states")
        if not isinstance(states, list):
            return
        faults = [
            {
                "component_id": state["component_id"],
                "fault": {
                    key: value
                    for key, value in state["fault"].items()
                    if key != "since_ns"
                },
            }
            for state in states
            if isinstance(state.get("fault"), dict)
            and "sim_reset" not in state.get("capabilities", ())
        ]
        result = self._core.observe_component_faults(
            faults_json=_canonical_json(faults).decode("utf-8"),
        )
        if not isinstance(result, dict) or type(result.get("changed")) is not bool:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "component fault observer returned no changed decision",
            )
        if result["changed"]:
            self._notify_change()

    def _observe_operation_table(self, operations: dict[str, Any]) -> None:
        # OD-205 (b): the operation alarms follow the Kernel's memory table:
        # its counter and faulted list (the resident list is the runner's).
        alarm_table = {
            name: operations[name]
            for name in ("change", "faulted", "faulted_total")
            if name in operations
        }
        result = self._core.observe_operation_table(
            operations_json=_canonical_json(alarm_table).decode("utf-8"),
        )
        if not isinstance(result, dict) or type(result.get("changed")) is not bool:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "operations table observer returned no changed decision",
            )
        if result["changed"]:
            self._notify_change()

    def _run(self) -> None:
        application_start_id: str | None = None
        after_sequence = 0
        source_read = False
        source_high_water = 0
        last_source_identity: tuple[str, str] | None = None
        # The panel's wake key from the stream `state` (Kernel memory, OD-205
        # (b)): the operations change counter, the Stop facts and the robots'
        # Active TCP. Rows are History only.
        stream_facts: tuple[Any, ...] | None = None
        control_projection_changed = False
        admission_generation: int | None = None
        component_model: dict[str, Any] | None = None
        source_mode = "active"
        source = self._stream

        def publish_if_changed(
            source_start_id: str,
            source_mode: str,
        ) -> None:
            nonlocal control_projection_changed, last_source_identity
            identity = (source_start_id, source_mode)
            if identity != last_source_identity or control_projection_changed:
                last_source_identity = identity
                control_projection_changed = False
                self._notify_change()

        while not self._stop.is_set():
            self._refresh_alarm_sources()
            try:
                if application_start_id is None:
                    identity_response = source.identity()
                    if identity_response.get("ok") is not True:
                        next_start_id = self._idle_application_start_id(identity_response)
                        if next_start_id is None:
                            ExecutionCommandClient._raise_error_response(identity_response)
                        # An empty product still has the stable Kernel/journal.
                        # Keep the existing idle identity poll, then use events.
                        source_mode = "idle"
                    else:
                        source_mode = "active"
                        status = identity_response.get("identity")
                        next_start_id = (
                            status.get("application_start_id")
                            if isinstance(status, dict) else None
                        )
                        if not isinstance(next_start_id, str) or not next_start_id:
                            raise HeadlessApiError(
                                "execution_command_response_invalid",
                                "execution status has no application start id",
                            )
                    # One host per start: the first identity read starts the
                    # event series; a later one resumes at the high water.
                    if not source_read:
                        source_read = True
                        source_high_water = 0
                        after_sequence = 0
                        self._global_rules.reset()
                        stream_facts = None
                        self._progress_rows.clear()
                        self._progress_dirty = True
                    else:
                        after_sequence = source_high_water
                    application_start_id = next_start_id
                page = source.events(after_sequence, limit=self._page_limit)
                source_mode = "active" if page["execution_owner_ready"] else "idle"
                self._observe_runtime_fault_snapshot(
                    {"runtime_faults": page["runtime_faults"]}
                )
                next_admission_generation = page["admission_generation"]
                if admission_generation != next_admission_generation:
                    admission_generation = next_admission_generation
                    control_projection_changed = True
                # The host publishes component states on change;
                # they reach the stream as a ``state`` message, no row.
                if component_model != page["component_model"]:
                    component_model = page["component_model"]
                    control_projection_changed = True
                    self._record_finished_actions(component_model)
                    self._observe_component_faults(component_model)
                page_had_events = bool(page["events"])
                self._observe_global_rules(page)
                operations = page["operations"]
                if operations is not None:
                    self._observe_operation_table(operations)
                next_stream_facts = (
                    operations["change"] if operations is not None else None,
                    page["last_stop_event_id"],
                    page["stop_progress_sequence"],
                    _canonical_json(page["active_tcp_baseline"]),
                )
                if next_stream_facts != stream_facts:
                    previous, stream_facts = stream_facts, next_stream_facts
                    control_projection_changed = True
                    # An operation changed or a participant ended its Stop:
                    # refresh the rest pose (idempotent).
                    if previous is None or previous[0] != next_stream_facts[0] or (
                        previous[2] != next_stream_facts[2]
                    ):
                        self._notify_motion_settled()
                for event in page["events"]:
                    self._core.ingest_operation_event(
                        source_sequence=event["sequence"],
                        record_kind=event["record_kind"],
                        request_key=event["request_key"],
                        operation_id=event["operation_id"],
                        operation_revision=event["operation_revision"],
                        occurred_at_ns=nonnegative_int64(event["occurred_at_ns"]),
                        occurred_at_wall=event["occurred_at_wall"],
                        payload_json=_canonical_json(event["payload"]).decode("utf-8"),
                        payload_digest=event["payload_digest"],
                    )
                    after_sequence = event["sequence"]
                    source_high_water = after_sequence
                if page["next_after_sequence"] != after_sequence:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "operation event pump cursor diverged from its page",
                    )
                self._capture_progress(page["operation_progress"])
                self._publish_progress(time.monotonic())
                if not page["has_more"]:
                    self._core.complete_operation_event_snapshot(
                        through_source_sequence=after_sequence,
                    )
                    publish_if_changed(application_start_id, source_mode)
                    if self._record_error(None):
                        self._notify_change()
                    if source_mode == "idle":
                        application_start_id = None
                    # Wakes on the next stream message; the other sources
                    # this loop observes keep the period.
                    self._stream.wait(self._poll_seconds)
                elif page_had_events:
                    # Do not delay a sparse lifecycle/Stop/fault transition
                    # behind a larger History catch-up.  The downstream cold
                    # combiner still retains only the newest requested value.
                    publish_if_changed(application_start_id, source_mode)
            except Exception as error:
                runtime_snapshot = getattr(error, "runtime_fault_snapshot", None)
                if runtime_snapshot is not None and application_start_id is not None:
                    try:
                        self._observe_runtime_fault_snapshot(runtime_snapshot)
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
                if application_start_id is not None:
                    try:
                        quality = self._core.mark_operation_event_source_unknown()
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
                application_start_id = None
                after_sequence = source_high_water
                self._stop.wait(self._poll_seconds)


__all__ = [
    "ExecutionCommandClient",
    "ExecutionEventPump",
    "ExecutionObservationSession",
    "ExecutionPort",
    "StreamedExecutionEvents",
]
