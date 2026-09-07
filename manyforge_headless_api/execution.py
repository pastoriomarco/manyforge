"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

import hashlib
import math
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
    OperationControlRequest,
    OperationRecoveryActionRequest,
    OperationIntentRequest,
    Pose3Request,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    ProgramRunStopIntentRequest,
    ProgramOperationQuery,
    RuntimeScaleControlRequest,
    OperationRuntimeScaleRequest,
    RuntimeScaleControlSelectorRequest,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
    TargetObservationQuery,
)
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object
from manyforge_behavior.json_scalars import nonnegative_int64
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_CONTROL_SOCKET_FILE = "execution-control.sock"
EXECUTION_OBSERVATION_SOCKET_FILE = "execution-observation.sock"
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v1"
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
    if event.get("record_kind") == "stop_terminal_projected":
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


MAX_EXECUTION_PACKET_BYTES = 1024 * 1024
MAX_EXECUTION_RESPONSE_BYTES = 4 * 1024 * 1024
# Mirrors the native operation journal's closed maximum. Released operations
# are removed as their control event is ingested, so this is a concurrent
# owner-state bound rather than lifetime retention.
MAXIMUM_RESIDENT_OPERATION_CONTROL_KEYS = 4096
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

    def start_pick(
        self,
        request: PickSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def start_place(
        self,
        request: PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def read_pick_place_sequence(self, sequence_id: str) -> dict[str, Any]: ...

    def pause(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def resume(
        self,
        request: OperationControlRequest,
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
        request: StopFaultRecoveryIntentRequest,
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
        """Read one exact aggregate Stop result for the internal Program owner."""

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
        response = self._exchange(
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
            or set(status)
            != {
                "request_id",
                "schema_version",
                "state",
                "stop_generation",
                "stop_id",
            }
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

    def observation(self) -> dict[str, Any]:
        return self._read_projection_on("observation", self._observation_socket_path)

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
        page["admission_generation"] = admission_generation
        return page

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
            or receipt.get("scene_revision") != expected_scene
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
            if command.startswith("start_"):
                sequence_status = response.get("sequence_status")
                if result["status"] == "planned":
                    self._validate_pick_place_sequence_status(
                        sequence_status,
                        sequence_id=request.sequence_id,
                        expected_kind=expected_kind,
                    )
                elif sequence_status is not None:
                    raise HeadlessApiError(
                        "execution_command_response_identity_mismatch",
                        "no-feasible pick/place result unexpectedly began a sequence",
                    )
        return response

    @staticmethod
    def _validate_pick_place_sequence_status(
        value: object,
        *,
        sequence_id: str,
        expected_kind: str | None = None,
    ) -> dict[str, Any]:
        phases = {
            "approach",
            "descend",
            "tool_command",
            "scene_transition",
            "retreat",
            "completed",
            "failed",
            "intervention_required",
        }
        if (
            not isinstance(value, dict)
            or value.get("schema_version") != "manyforge.pick_place_sequence_status.v1"
            or value.get("sequence_id") != sequence_id
            or value.get("kind") not in {"pick", "place"}
            or (expected_kind is not None and value.get("kind") != expected_kind)
            or value.get("phase") not in phases
            or not isinstance(value.get("selected_object_id"), str)
            or not value["selected_object_id"]
            or not isinstance(value.get("completed_operation_ids"), list)
            or not all(
                isinstance(operation_id, str) and operation_id
                for operation_id in value["completed_operation_ids"]
            )
            or (
                value.get("active_operation_id") is not None
                and not isinstance(value.get("active_operation_id"), str)
            )
            or (
                value.get("committed_projection_revision") is not None
                and not isinstance(value.get("committed_projection_revision"), dict)
            )
            or (
                value.get("terminal_error") is not None
                and not isinstance(value.get("terminal_error"), dict)
            )
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "pick/place sequence status does not match its owner-issued contract",
            )
        return value

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

    def start_pick(
        self,
        request: PickSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._plan_pick_place(
            "start_pick",
            request,
            trusted_principal_id=trusted_principal_id,
        )

    def start_place(
        self,
        request: PlaceSequencePlanningRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._plan_pick_place(
            "start_place",
            request,
            trusted_principal_id=trusted_principal_id,
        )

    def read_pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        if not sequence_id or len(sequence_id.encode("utf-8")) > 96:
            raise ValueError("sequence_id must be bounded non-empty text")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "read_pick_place_sequence",
                "sequence_id": sequence_id,
            }
        )
        if response.get("ok") is True:
            if response.get("command") != "read_pick_place_sequence":
                raise HeadlessApiError(
                    "execution_command_response_identity_mismatch",
                    "pick/place sequence read returned another command",
                )
            self._validate_pick_place_sequence_status(
                response.get("sequence_status"), sequence_id=sequence_id
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

    def pause(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._control(
            "pause", request, trusted_principal_id=trusted_principal_id
        )

    def resume(
        self,
        request: OperationControlRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        return self._control(
            "resume", request, trusted_principal_id=trusted_principal_id
        )

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
        }:
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
        if (
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
        return bool(expectations)

    def stop_fault(self) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "get_stop_fault",
            }
        )
        if response.get("ok") is True and (
            response.get("command") != "get_stop_fault"
            or not self._valid_stop_fault_offer(response.get("stop_fault"))
        ):
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "Stop-fault response is invalid",
            )
        return response

    def recover_stop_fault(
        self,
        request: StopFaultRecoveryIntentRequest,
    ) -> dict[str, Any]:
        request_payload = request.model_dump(mode="json")
        if request.reason is None:
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
            or not isinstance(result.get("latch"), dict)
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "Stop-fault recovery result does not match its request",
            )
        return response

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
    ) -> None:
        if not 0.01 <= poll_seconds <= 5.0:
            raise ValueError("operation event polling period is outside [0.01, 5]")
        if not 0.05 <= condition_poll_seconds <= 5.0:
            raise ValueError("operation condition polling period is outside [0.05, 5]")
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

    def set_change_observer(self, observer: Callable[[], None] | None) -> None:
        if observer is not None and not callable(observer):
            raise TypeError("execution event change observer must be callable")
        self._change_observer = observer

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
            return previous != self._last_error

    @staticmethod
    def _control_projection_key(event: dict[str, Any]) -> tuple[Any, ...]:
        """Return only operation facts that can change the control shell.

        Native ``state_observed`` rows deliberately retain exact progress and
        timing in History.  Those high-rate samples belong to the credited
        Viewer/history paths, not to the cold control projection.  Lifecycle,
        Stop, runtime-scale, and fault meaning remains in this small key.
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
    def _operation_is_released(event: dict[str, Any]) -> bool:
        if event["record_kind"] != "control_accepted":
            return False
        result = event["payload"].get("result")
        return isinstance(result, dict) and result.get("control") in {
            "acknowledge_completed",
            "reset",
        }

    @staticmethod
    def _admission_generation(response: dict[str, Any]) -> int:
        generation = response.get("admission_generation")
        if type(generation) is not int or not 1 <= generation <= MAXIMUM_SAFE_INTEGER:
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "native execution identity has no valid admission generation",
            )
        return generation

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
            try:
                if owner_epoch is None:
                    identity_response = self._execution.identity()
                    next_admission_generation = self._admission_generation(
                        identity_response
                    )
                    if admission_generation != next_admission_generation:
                        admission_generation = next_admission_generation
                        control_projection_changed = True
                    if identity_response.get("ok") is not True:
                        idle_epoch = self._idle_owner_epoch(identity_response)
                        if idle_epoch is None:
                            ExecutionCommandClient._raise_error_response(
                                identity_response
                            )
                        if source_high_water_epoch != idle_epoch:
                            source_high_water_epoch = idle_epoch
                            source_high_water = 0
                        self._core.complete_operation_event_snapshot(
                            source_owner_epoch=idle_epoch,
                            through_source_sequence=source_high_water,
                            **condition_snapshot_kwargs(idle_epoch, source_high_water),
                        )
                        publish_if_changed(idle_epoch, "idle")
                        if self._record_error(None):
                            self._notify_change()
                        self._stop.wait(self._poll_seconds)
                        continue
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
                        operation_control_keys.clear()
                    else:
                        after_sequence = source_high_water
                    owner_epoch = next_epoch
                page = self._execution.events(
                    owner_epoch,
                    after_sequence,
                    limit=self._page_limit,
                )
                next_admission_generation = page["admission_generation"]
                if admission_generation != next_admission_generation:
                    admission_generation = next_admission_generation
                    control_projection_changed = True
                page_had_events = bool(page["events"])
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
                        if self._operation_is_released(event):
                            operation_control_keys.pop(operation_id, None)
                    else:
                        # The only operation-less events are sparse Stop facts.
                        control_projection_changed = True
                if page["next_after_sequence"] != after_sequence:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "operation event pump cursor diverged from its page",
                    )
                if not page["has_more"]:
                    self._core.complete_operation_event_snapshot(
                        source_owner_epoch=owner_epoch,
                        through_source_sequence=after_sequence,
                        **condition_snapshot_kwargs(owner_epoch, after_sequence),
                    )
                    publish_if_changed(owner_epoch, "active")
                    if self._record_error(None):
                        self._notify_change()
                    self._stop.wait(self._poll_seconds)
                elif page_had_events and control_projection_changed:
                    # Do not delay a sparse lifecycle/Stop/fault transition
                    # behind a larger History catch-up.  The downstream cold
                    # combiner still retains only the newest requested value.
                    publish_if_changed(owner_epoch, "active")
            except Exception as error:
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
