"""Headless's thin adapter over the gateway core's execution-host calls.

Every request/reply command goes through one ``manyforge_headless_py.Gateway``
call (one connection per request, GIL released, only the connect limited;
OD-214). The core owns the wire: envelope, attribution, lane socket, version
and the command kind a reply names. This module keeps only what decides an
outcome or reshapes a reply into what the routes return. The event pump
follows the core's ``ExecutionEvents`` subscription. The viewer's long-lived
observation session (``execution-observation.sock``) stays here.
"""

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
    FullStopIntentRequest,
    MockIoInputRequest,
    OperationControlRequest,
    OperationIntentRequest,
    Pose3Request,
    ResolvedPreviewTcp,
    RecoveryPreflightProjection,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from .activity_history import component_action_record, record_activity
from .global_rule_status import GlobalRuleStatusModel
from .motion_trace_history import MotionTraceCapture
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object
from manyforge_behavior.json_scalars import nonnegative_int64
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


_LOGGER = logging.getLogger(__name__)

EXECUTION_OBSERVATION_SOCKET_FILE = "execution-observation.sock"
# The host's component model facts in every stream ``state``: a change of any
# wakes the control state. Signal samples count by value, source and
# freshness, not by age.
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


MAX_EXECUTION_RESPONSE_BYTES = 4 * 1024 * 1024
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

    def read_volatile_motion_trace(self, allowance_bytes: int) -> dict[str, Any]: ...

    def capture_motion_trace_file(self, allowance_bytes: int) -> dict[str, Any]: ...


def _canonical_json(value: Any) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            "execution_command_invalid",
            f"execution command is not canonically encodable: {exc}",
        ) from exc


def _decode_observation_response(payload: bytes, flags: int) -> dict[str, Any]:
    """One packet of the viewer's observation session (not a gateway call)."""

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
                    response = _decode_observation_response(payload, flags)
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


def _motion_trace_file_metadata(metadata: Any, allowance_bytes: int) -> dict[str, Any]:
    """The native trace-file metadata, validated. No metadata (disk traces
    unconfigured) is refused like invalid metadata."""
    if (not isinstance(metadata, dict)
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
    return metadata


def gateway_api_error(error: Any) -> HeadlessApiError:
    """A core ``GatewayError`` as the stable Headless error (same code,
    message, details and producer)."""

    return HeadlessApiError(
        error.code, error.message, details=error.details, producer=error.producer
    )


class ExecutionCommandClient:
    """Forward intents and observations through the gateway core without
    caching execution truth.

    ``gateway`` is the shared ``manyforge_headless_py.Gateway``;
    ``observation_run_root`` is the execution run root of the viewer's
    observation session, the only connection this adapter opens itself.
    """

    def __init__(
        self,
        gateway: Any,
        *,
        observation_run_root: Path | None = None,
    ) -> None:
        import manyforge_headless_py as core  # type: ignore[import-not-found]

        self._gateway = gateway
        self._gateway_error: type[Exception] = core.GatewayError
        self._observation_root: PinnedRunRoot | None = None
        if observation_run_root is not None:
            try:
                self._observation_root = PinnedRunRoot(observation_run_root)
            except PrivateTransportCapabilityError as error:
                raise HeadlessApiError(
                    "execution_owner_unavailable",
                    f"native execution run capability is invalid: {error}",
                ) from error

    def _host(
        self,
        command: str,
        members: dict[str, Any] | None = None,
        trusted_principal_id: str | None = None,
    ) -> dict[str, Any]:
        """One host call; the reply as the host sent it (ok true or false)."""

        try:
            return self._gateway.host(command, members or {}, trusted_principal_id)
        except self._gateway_error as error:
            _LOGGER.warning(
                "Execution host call failed: command=%s code=%s message=%s",
                command, error.code, error.message,
            )
            raise gateway_api_error(error) from error

    @staticmethod
    def _raise_error_response(response: dict[str, Any]) -> None:
        error = response["error"]
        raise HeadlessApiError(
            error["code"], error["message"], details=error.get("details"),
            producer=error.get("producer", "kernel"),
        )

    @classmethod
    def require_success(cls, response: dict[str, Any]) -> dict[str, Any]:
        """Raise the existing stable owner error for a direct typed caller."""

        if response.get("ok") is not True:
            cls._raise_error_response(response)
        return response

    def diagnostics(self) -> dict[str, Any]:
        """Return the explicit aggregate diagnostic projection.

        The retained native wire spelling remains ``status`` for compatibility;
        ordinary runtime observers do not use it.
        """
        return self._host("status")

    def identity(self) -> dict[str, Any]:
        return self._host("identity")

    def targets(self) -> dict[str, Any]:
        return self._host("targets")

    def control_state(self) -> dict[str, Any]:
        return self._host("control_state")

    def observation(self) -> dict[str, Any]:
        # A one-shot snapshot on the ordinary command lane: it never queues
        # behind the long-poll observation session's own lane.
        return self._host("observation")

    def open_observation_session(self) -> ExecutionObservationSession:
        if self._observation_root is None:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "no execution run root is configured for observation",
            )
        return ExecutionObservationSession(
            self._observation_root,
            self._observation_root.path / EXECUTION_OBSERVATION_SOCKET_FILE,
        )

    def query_target(self, request: TargetObservationQuery) -> dict[str, Any]:
        return self.require_success(
            self._host(
                "query_target",
                {"query": request.model_dump(mode="json", exclude_none=True)},
            )
        )

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

        return self.require_success(
            self._host("set_mock_io_input", {"request": request.model_dump(mode="json")})
        )

    def submit(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._host(
            "submit", {"operation": request.model_dump(mode="json")}, trusted_principal_id
        )

    def read_manual_handover(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Read exact Kernel cleanup input; this does not grant manual admission."""
        return self._host(
            "read_manual_handover",
            {"operation": request.model_dump(mode="json")},
            trusted_principal_id,
        )

    def preview_motion(
        self,
        request: OperationIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        operation = request.model_dump(mode="json")
        response = self._host("preview_motion", {"operation": operation}, trusted_principal_id)
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
            receipt.get("target") != participant["target"]
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
        response = self._host("preview_capability_command", {"operation": operation}, trusted_principal_id)
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
            receipt.get("target") != participant["target"]
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
        return self._host("read", {"operation_id": operation_id})

    def read_volatile_motion_trace(self, allowance_bytes: int) -> dict[str, Any]:
        """Return the native volatile motion-trace object, unchanged.

        Diagnostic only: ``allowance_bytes`` caps the encoded trace object the
        native host may return. A trace written to disk is read and checked
        here; failures raise ``HeadlessApiError``.
        """
        if type(allowance_bytes) is not int or allowance_bytes <= 0:
            raise ValueError("trace allowance must be a positive integer")
        response = self.require_success(
            self._host("read_volatile_motion_trace", {"allowance_bytes": allowance_bytes})
        )
        if "trace_file" in response:
            return self._read_motion_trace_file(response["trace_file"], allowance_bytes)
        return response["trace"]

    def capture_motion_trace_file(self, allowance_bytes: int) -> dict[str, Any]:
        """Have the native host write one snapshot to its trace store and
        return the file's metadata (``manyforge.motion_trace_file.v2``)
        without reading the file back.

        The host copies the volatile ring under the trace's own lock and
        writes the file on its motion-trace lane; this call waits for that
        answer, so only an ordinary worker makes it. Failures raise
        ``HeadlessApiError`` with the native code.
        """
        if type(allowance_bytes) is not int or allowance_bytes <= 0:
            raise ValueError("trace allowance must be a positive integer")
        response = self.require_success(
            self._host("read_volatile_motion_trace", {"allowance_bytes": allowance_bytes})
        )
        return _motion_trace_file_metadata(response.get("trace_file"), allowance_bytes)

    def _read_motion_trace_file(self, metadata: Any, allowance_bytes: int) -> dict[str, Any]:
        directory = os.environ.get("MANYFORGE_MOTION_TRACE_DIR", "")
        _motion_trace_file_metadata(metadata if directory else None, allowance_bytes)
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


    def plan_pick(
        self,
        request: PickSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        # Answered at acceptance (OD-213 (1)): the job is read, cancelled and
        # released by name; its result arrives with a read once it is done.
        return self._host(
            "plan_pick", {"intent": request.model_dump(mode="json")}, trusted_principal_id
        )

    def plan_place(
        self,
        request: PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._host(
            "plan_place", {"intent": request.model_dump(mode="json")}, trusted_principal_id
        )

    def read_planning_job(self, job_id: str) -> dict[str, Any]:
        """One pick/place planning job's state, and its result once done.

        Served on the control socket, memory only: never behind planning."""

        return self._host("read_planning_job", {"job_id": _require_planning_job_id(job_id)})

    def cancel_planning_job(self, job_id: str) -> dict[str, Any]:
        """Cancel this planning job only; returns its state after the cancel."""

        return self._host("cancel_planning_job", {"job_id": _require_planning_job_id(job_id)})

    def release_planning_job(self, job_id: str) -> dict[str, Any]:
        """Drop a finished planning job (done, failed or cancelled)."""

        return self._host("release_planning_job", {"job_id": _require_planning_job_id(job_id)})

    def reset(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._host(
            "reset", {"request": request.model_dump(mode="json")}, trusted_principal_id
        )

    def acknowledge_completed(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._host(
            "acknowledge_completed",
            {"request": request.model_dump(mode="json")},
            trusted_principal_id,
        )

    def stop(
        self,
        request: StopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._host(
            "stop",
            {"request": _sparse_request_with_complete_context(request)},
            trusted_principal_id,
        )

    def stop_all(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Stop all: every operation in the cell; holds every group."""

        return self._host(
            "stop_all", {"request": request.model_dump(mode="json")}, trusted_principal_id
        )

    def halt(
        self,
        request: FullStopIntentRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        """Halt: Stop all plus the Program frozen; names no run."""

        return self._host(
            "halt", {"request": request.model_dump(mode="json")}, trusted_principal_id
        )

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

        return self._host(
            "recognized_stop",
            {
                "reason": reason,
                "recognized_route": recognized_route,
                "request_key": request_key,
                "source_label": source_label,
            },
            trusted_principal_id,
        )

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

        return self._host(
            "stop_program_selection",
            {
                "request": {
                    "schema_version": PROGRAM_EXECUTION_SELECTION_VERSION,
                    "request_context": {
                        "request_key": request.request_key,
                        "source_label": request.source_label,
                    },
                    "program_run_id": request.program_run_id,
                    "group_ids": list(request.group_ids),
                },
            },
            trusted_principal_id,
        )

    def component_model(self) -> dict[str, Any]:
        """Every ComponentState, the CellLatch, every
        SignalSample, the rule holds and the required signal ids, as the
        execution host publishes them (an in-memory read)."""
        return self._host("read_component_model")

    def component_action(self, component_id: str, action: str) -> dict[str, Any]:
        """Hand one action to the core component coordinator.

        The reply only accepts or refuses (`component_action_in_progress`,
        `component_action_unsupported`, `component_not_found`); the result
        arrives in the component's `last_action`.
        """
        return self._host(
            "component_action",
            {"request": {"component_id": component_id, "action": action}},
        )

    def component_recovery_preflight(self, component_id: str) -> dict[str, Any]:
        response = self._host(
            "component_recovery_preflight", {"request": {"component_id": component_id}}
        )
        if response.get("ok") is True:
            try:
                projection = RecoveryPreflightProjection.model_validate(response.get("preflight"))
            except ValueError as error:
                raise HeadlessApiError("execution_command_response_invalid", "recovery preflight is malformed") from error
            if projection.components[-1].component_id != component_id:
                raise HeadlessApiError("execution_command_response_invalid", "recovery preflight names another component")
        return response

    def component_recover_enable(self, component_id: str, confirmation: dict[str, Any]) -> dict[str, Any]:
        return self._host(
            "component_action",
            {"request": {"component_id": component_id, "action": "enable",
                         "recovery_preflight": confirmation}},
        )

    def set_io_output(
        self, signal_id: str, value: bool | float, *, origin: str
    ) -> dict[str, Any]:
        """One output write; the reply carries SetOutputResult.
        `origin` is `manual` (refused while motion executes) or `program`."""
        return self._host(
            "set_io_output",
            {"request": {"signal_id": signal_id, "value": value, "origin": origin}},
        )

    def set_runtime_scale_control(
        self,
        request: RuntimeScaleControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        # Beside Stop on the control socket: the execution host queues it on
        # its speed-scaling lane, never behind planning or Submit.
        return self._host(
            "set_runtime_scale_control",
            {"request": request.model_dump(mode="json")},
            trusted_principal_id,
        )

    def get_runtime_scale_control(
        self, selector: RuntimeScaleControlSelectorRequest
    ) -> dict[str, Any]:
        return self._host(
            "get_runtime_scale_control", {"selector": selector.model_dump(mode="json")}
        )


def _events_page(
    state: dict[str, Any],
    rows: list[dict[str, Any]],
    *,
    has_more: bool,
    after_sequence: int,
    progress: list[Any],
) -> dict[str, Any]:
    """The pump's page: the buffered rows with the latest stream ``state``
    facts. Signal samples count without their age."""

    component_model = {name: state.get(name) for name in COMPONENT_MODEL_STATE_FIELDS}
    samples = component_model["signal_samples"]
    if samples is not None:
        component_model["signal_samples"] = [
            {key: value for key, value in sample.items() if key != "age_ns"}
            for sample in samples
        ]
    return {
        "events": rows,
        "has_more": has_more,
        "next_after_sequence": rows[-1]["sequence"] if rows else after_sequence,
        "operation_progress": progress,
        "admission_generation": state["admission_generation"],
        "runtime_faults": state["runtime_faults"],
        "execution_owner_ready": state["execution_owner_ready"],
        # Parsed by the global rule status projection (absent: unknown).
        "interrupt_rule_catalog": state.get("interrupt_rule_catalog"),
        "interrupt_rule_state_baseline": state.get("interrupt_rule_state_baseline"),
        "active_tcp_baseline": state.get("active_tcp_baseline"),
        # The Kernel's operations table and Stop facts from memory (OD-205 (b)).
        "operations": state.get("operations"),
        "last_stop_event_id": state.get("last_stop_event_id"),
        "stop_progress_sequence": state.get("stop_progress_sequence"),
        "component_model": component_model,
    }


class StreamedExecutionEvents:
    """The event pump's source on one core event subscription.

    The core's follower (``ExecutionEvents``) keeps the stream (resume from
    its cursor, dead link, reconnect); this reads the pump's subscription on
    the pump's own thread: every call first takes the messages already
    delivered. Rows wait in order until a later ``events`` call names a
    cursor at or after them, so a failed ingestion re-reads them; the latest
    ``state`` and ``progress`` are kept as received. ``identity`` and
    ``events`` answer in the shapes the pump reads; ``wait`` returns as soon
    as a new message arrives. The buffer is bounded: a full buffer takes no
    more messages (the core's subscription then fills and resumes on its own
    connection after the last message taken, losing nothing).
    """

    MAX_BUFFERED_EVENTS = 4_096

    def __init__(self, subscription: Any, application_start_id: str) -> None:
        self._subscription = subscription
        # One Kernel and one journal per application start: the stream names
        # no owner, so this start is the event source's identity.
        self._application_start_id = application_start_id
        self._state: dict[str, Any] | None = None
        self._progress: list[Any] = []
        self._events: deque[dict[str, Any]] = deque()
        self._connected = False

    def _apply(self, message: dict[str, Any]) -> None:
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
            self._events.append(message["event"])
        elif kind == "disconnected":
            self._connected = False

    def _take_delivered(self, timeout: float = 0.0) -> None:
        """Take the delivered messages (waiting up to ``timeout`` for the
        first) while the buffer has room."""

        while len(self._events) < self.MAX_BUFFERED_EVENTS:
            message = self._subscription.next(timeout)
            if message is None:
                return
            self._apply(message)
            timeout = 0.0

    def _require_connected(self) -> dict[str, Any]:
        self._take_delivered()
        if not self._connected or self._state is None:
            raise HeadlessApiError(
                "execution_owner_unavailable", "execution event stream is not connected"
            )
        return self._state

    def identity(self) -> dict[str, Any]:
        """The start in the ``identity`` reply shapes the pump reads
        (ready: the identity; no product: ``execution_owner_not_ready``)."""

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
        """The buffered rows strictly after ``after_sequence`` as one page
        with the latest stream facts."""

        # Consumed rows first: their room takes the next delivered messages.
        while self._events and self._events[0]["sequence"] <= after_sequence:
            self._events.popleft()
        state = self._require_connected()
        rows = [self._events[index] for index in range(min(limit, len(self._events)))]
        return _events_page(
            state,
            rows,
            has_more=len(self._events) > limit,
            after_sequence=after_sequence,
            progress=list(self._progress),
        )

    def close(self) -> None:
        self._subscription.close()

    def wait(self, timeout: float) -> None:
        """Until a new stream message, the subscription's close or ``timeout``."""

        self._take_delivered(timeout)


class ExecutionEventPump:
    """Move durable native facts to the C++ historian without owning truth."""

    def __init__(
        self,
        core: HeadlessCorePort,
        events: Any,
        *,
        application_start_id: str,
        poll_seconds: float = 0.1,
        page_limit: int = 16,
        motion_trace_source: ExecutionPort | None = None,
    ) -> None:
        if not 0.01 <= poll_seconds <= 5.0:
            raise ValueError("operation event polling period is outside [0.01, 5]")
        if not 1 <= page_limit <= 16:
            raise ValueError("operation event page limit is outside [1, 16]")
        self._core = core
        # The core's event follower (``ExecutionEvents``): identity, pages
        # and wake-ups come from one subscription to it; the request/reply
        # `events` command is never polled. The pump starts and closes it.
        self._events = events
        self._application_start_id = application_start_id
        self._poll_seconds = poll_seconds
        self._stop = threading.Event()
        self._stream: StreamedExecutionEvents | None = None
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
        # With a trace store configured (MANYFORGE_MOTION_TRACE_DIR), a newly
        # ingested failure or escalated Stop of a motion participant is handed
        # to the capture worker (memory only), which has the host export the
        # trace through ``motion_trace_source``; the export never runs here.
        self._motion_traces = (
            MotionTraceCapture(core, motion_trace_source)
            if motion_trace_source is not None
            and os.environ.get("MANYFORGE_MOTION_TRACE_DIR")
            else None
        )
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
        # Subscribed before the follower starts, so its first snapshot is
        # this subscription's first message.
        self._stream = StreamedExecutionEvents(
            self._events.subscribe(), self._application_start_id
        )
        self._events.start()
        if self._motion_traces is not None:
            self._motion_traces.start()
        self._thread = threading.Thread(
            target=self._run,
            name="manyforge-operation-event-pump",
        )
        self._thread.start()

    def shutdown(self) -> None:
        self._stop.set()
        if self._motion_traces is not None:
            self._motion_traces.shutdown()
        stream = self._stream
        if stream is not None:
            # Ends the pump thread's wait at once.
            stream.close()
        thread = self._thread
        if thread is not None:
            thread.join(timeout=1.5)
            if thread.is_alive():
                raise RuntimeError("operation event pump did not stop in time")
        self._thread = None
        self._events.close()

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

    def _observe_runtime_fault_snapshot(self, runtime_faults: dict[str, Any]) -> None:
        fault = runtime_faults.get("operation_journal_write_fault")
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
            current_runtime_faults_json=_canonical_json(runtime_faults).decode("utf-8"),
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
                    if key not in ("since_ns", "since_utc_ns", "details")
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
        assert source is not None

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
                self._observe_runtime_fault_snapshot(page["runtime_faults"])
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
                    ingested = self._core.ingest_operation_event(
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
                    if (
                        self._motion_traces is not None
                        and ingested.get("status") == "ingested"
                    ):
                        self._motion_traces.observe(event)
                    after_sequence = event["sequence"]
                    source_high_water = after_sequence
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
                    source.wait(self._poll_seconds)
                elif page_had_events:
                    # Do not delay a sparse lifecycle/Stop/fault transition
                    # behind a larger History catch-up.  The downstream cold
                    # combiner still retains only the newest requested value.
                    publish_if_changed(application_start_id, source_mode)
            except Exception as error:
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
    "gateway_api_error",
]
