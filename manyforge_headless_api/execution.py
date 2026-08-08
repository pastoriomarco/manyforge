"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

import os
import socket
import stat
import threading
from pathlib import Path
from typing import Any, Protocol
import unicodedata

import rfc8785

from .models import (
    OperationControlRequest,
    OperationIntentRequest,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    StopIntentRequest,
)
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v1"
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
OPERATION_EVENT_KINDS = frozenset(
    {
        "operation_accepted",
        "control_accepted",
        "stop_accepted",
        "runtime_scale_accepted",
        "state_observed",
        "host_replaced",
    }
)
MAX_EXECUTION_PACKET_BYTES = 1024 * 1024
MAX_EXECUTION_RESPONSE_BYTES = 4 * 1024 * 1024


class ExecutionPort(Protocol):
    def status(self) -> dict[str, Any]: ...

    def submit(
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

    def set_runtime_scale(
        self,
        request: RuntimeScaleRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def get_runtime_scale(self, operation_id: str) -> dict[str, Any]: ...

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


class ExecutionCommandClient:
    """Forward intents and observations without caching execution truth."""

    def __init__(self, state_root: Path, *, timeout_seconds: float = 10.0) -> None:
        if timeout_seconds <= 0 or timeout_seconds > 30:
            raise ValueError("execution timeout must be in (0, 30] seconds")
        self._socket_path = state_root / EXECUTION_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    def _validate_socket(self) -> None:
        try:
            metadata = self._socket_path.lstat()
        except FileNotFoundError as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution command socket is absent",
            ) from exc
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HeadlessApiError(
                "execution_owner_unavailable",
                "native execution path is not the expected same-user socket",
            )

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_EXECUTION_PACKET_BYTES:
            raise HeadlessApiError(
                "execution_command_oversized",
                "execution command exceeds its byte bound",
            )
        self._validate_socket()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_RCVBUF,
                    MAX_EXECUTION_RESPONSE_BYTES,
                )
                client.settimeout(self._timeout_seconds)
                client.connect(str(self._socket_path))
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
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "execution_owner_unavailable",
                f"native execution transport failed: {exc}",
            ) from exc
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
        if _canonical_json(response) != payload:
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

    @staticmethod
    def _http_attribution(trusted_principal_id: str | None) -> dict[str, Any]:
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
        return {
            "transport": "http",
            "assurance": (
                "externally_authenticated"
                if trusted_principal_id is not None
                else "unverified"
            ),
            "principal_id": trusted_principal_id,
        }

    def status(self) -> dict[str, Any]:
        response = self._exchange(
            {"api_version": EXECUTION_COMMAND_VERSION, "command": "status"}
        )
        if response.get("ok") is True and response.get("command") != "status":
            raise HeadlessApiError(
                "execution_command_response_invalid",
                "execution status response has the wrong command kind",
            )
        return response

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
        ):
            raise HeadlessApiError(error["code"], error["message"])
        raise HeadlessApiError(
            "execution_command_response_invalid",
            "native execution owner returned an invalid error response",
        )

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
        if (
            response.get("command") != "events"
            or not isinstance(page, dict)
            or page.get("schema_version")
            != "manyforge.execution_event_page.v1"
            or page.get("owner_epoch") != owner_epoch
            or type(page.get("next_after_sequence")) is not int
            or not after_sequence
            <= page["next_after_sequence"]
            <= MAXIMUM_SAFE_INTEGER
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
                or not isinstance(event.get("operation_id"), str)
                or not event["operation_id"]
                or type(event.get("operation_revision")) is not int
                or not 1
                <= event["operation_revision"]
                <= MAXIMUM_SAFE_INTEGER
                or type(event.get("occurred_at_ns")) is not int
                or not 0 <= event["occurred_at_ns"] <= MAXIMUM_SAFE_INTEGER
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
                "attribution": self._http_attribution(trusted_principal_id),
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
                "attribution": self._http_attribution(trusted_principal_id),
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
                or result.get("status")
                not in {"planned", "no_feasible_candidate"}
                or type(result.get("attempted_candidate_count")) is not int
                or not 0
                <= result["attempted_candidate_count"]
                <= 4096
                or not isinstance(result.get("reports"), list)
                or len(result["reports"])
                > result["attempted_candidate_count"]
                or (
                    expected_kind == "pick"
                    and result["attempted_candidate_count"] > 0
                    and not isinstance(
                        result.get("last_attempted_candidate"), dict
                    )
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
            or value.get("schema_version")
            != "manyforge.pick_place_sequence_status.v1"
            or value.get("sequence_id") != sequence_id
            or value.get("kind") not in {"pick", "place"}
            or (
                expected_kind is not None
                and value.get("kind") != expected_kind
            )
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
                and not isinstance(
                    value.get("committed_projection_revision"), dict
                )
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
                "attribution": self._http_attribution(trusted_principal_id),
                "command": command,
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != command
            or not isinstance(accepted, dict)
            or accepted.get("request_id")
            != request.request_context.request_id
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
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._http_attribution(trusted_principal_id),
                "command": "stop",
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        requested_operations = [item.operation_id for item in request.operations]
        if response.get("ok") is True and (
            response.get("command") != "stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id")
            != request.request_context.request_id
            or accepted.get("operations") != requested_operations
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "execution Stop acceptance does not match its request",
            )
        return response

    def set_runtime_scale(
        self,
        request: RuntimeScaleRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._http_attribution(trusted_principal_id),
                "command": "set_runtime_scale",
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "set_runtime_scale"
            or not isinstance(accepted, dict)
            or accepted.get("request_id")
            != request.request_context.request_id
            or accepted.get("operation_id") != request.operation_id
            or accepted.get("operation_revision")
            != request.expected_operation_revision
            or accepted.get("requested_scale") != request.scale
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "runtime-scale acceptance does not match its request",
            )
        return response

    def get_runtime_scale(self, operation_id: str) -> dict[str, Any]:
        if not operation_id or len(operation_id.encode("utf-8")) > 256:
            raise ValueError("operation_id must be bounded non-empty text")
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "get_runtime_scale",
                "operation_id": operation_id,
            }
        )
        status = response.get("runtime_scale")
        if response.get("ok") is True and (
            response.get("command") != "get_runtime_scale"
            or not isinstance(status, dict)
            or status.get("operation_id") != operation_id
        ):
            raise HeadlessApiError(
                "execution_command_response_identity_mismatch",
                "runtime-scale observation does not match the requested operation",
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
        page_limit: int = 16,
    ) -> None:
        if not 0.01 <= poll_seconds <= 5.0:
            raise ValueError("operation event polling period is outside [0.01, 5]")
        if not 1 <= page_limit <= 16:
            raise ValueError("operation event page limit is outside [1, 16]")
        self._core = core
        self._execution = execution
        self._poll_seconds = poll_seconds
        self._page_limit = page_limit
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._last_error: str | None = None
        self._health_lock = threading.Lock()

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

    def _record_error(self, error: Exception | None) -> None:
        with self._health_lock:
            if error is None:
                self._last_error = None
            else:
                code = getattr(error, "code", None)
                prefix = f"{type(error).__name__}: "
                if isinstance(code, str) and code:
                    prefix += f"{code}: "
                self._last_error = prefix + str(error)

    def _run(self) -> None:
        owner_epoch: str | None = None
        after_sequence = 0
        while not self._stop.is_set():
            try:
                if owner_epoch is None:
                    status_response = self._execution.status()
                    if status_response.get("ok") is not True:
                        ExecutionCommandClient._raise_error_response(status_response)
                    status = status_response.get("status")
                    next_epoch = (
                        status.get("owner_epoch")
                        if isinstance(status, dict)
                        else None
                    )
                    if not isinstance(next_epoch, str) or not next_epoch:
                        raise HeadlessApiError(
                            "execution_command_response_invalid",
                            "execution status has no owner epoch",
                        )
                    owner_epoch = next_epoch
                    after_sequence = 0
                page = self._execution.events(
                    owner_epoch,
                    after_sequence,
                    limit=self._page_limit,
                )
                for event in page["events"]:
                    self._core.ingest_operation_event(
                        source_owner_epoch=owner_epoch,
                        source_sequence=event["sequence"],
                        record_kind=event["record_kind"],
                        request_id=event["request_id"],
                        operation_id=event["operation_id"],
                        operation_revision=event["operation_revision"],
                        occurred_at_ns=event["occurred_at_ns"],
                        occurred_at_wall=event["occurred_at_wall"],
                        effect_pending=event["effect_pending"],
                        payload_json=_canonical_json(event["payload"]).decode("utf-8"),
                        payload_digest=event["payload_digest"],
                    )
                    after_sequence = event["sequence"]
                if page["next_after_sequence"] != after_sequence:
                    raise HeadlessApiError(
                        "execution_command_response_invalid",
                        "operation event pump cursor diverged from its page",
                    )
                if not page["has_more"]:
                    self._core.complete_operation_event_snapshot(
                        source_owner_epoch=owner_epoch,
                        through_source_sequence=after_sequence,
                    )
                    self._record_error(None)
                    self._stop.wait(self._poll_seconds)
            except Exception as error:
                if owner_epoch is not None:
                    try:
                        self._core.mark_operation_event_source_unknown(
                            source_owner_epoch=owner_epoch,
                        )
                    except Exception as quality_error:
                        error = RuntimeError(
                            f"{type(error).__name__}: {error}; "
                            "operation source-quality update failed: "
                            f"{type(quality_error).__name__}: {quality_error}"
                        )
                self._record_error(error)
                owner_epoch = None
                after_sequence = 0
                self._stop.wait(self._poll_seconds)


__all__ = ["ExecutionCommandClient", "ExecutionEventPump", "ExecutionPort"]
