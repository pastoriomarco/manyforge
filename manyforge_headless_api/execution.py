"""Bounded same-user client for the selected native execution owner."""

from __future__ import annotations

import hashlib
import math
import os
import socket
import stat
import threading
from pathlib import Path
from typing import Any, Protocol
import unicodedata

import rfc8785

from .models import (
    FullStopIntentRequest,
    OperationControlRequest,
    OperationIntentRequest,
    Pose3Request,
    PickSequencePlanningRequest,
    PlaceSequencePlanningRequest,
    RuntimeScaleRequest,
    StopFaultRecoveryIntentRequest,
    StopIntentRequest,
)
from .native import HeadlessApiError, HeadlessCorePort, strict_json_object


EXECUTION_SOCKET_FILE = "execution-command.sock"
EXECUTION_CONTROL_SOCKET_FILE = "execution-control.sock"
EXECUTION_COMMAND_VERSION = "manyforge.execution_command.v1"
EXECUTION_RESPONSE_VERSION = "manyforge.execution_command_response.v1"
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
OPERATION_EVENT_KINDS = frozenset(
    {
        "operation_accepted",
        "control_accepted",
        "stop_acceptance_projected",
        "runtime_scale_accepted",
        "state_observed",
        "host_replaced",
    }
)


def _valid_operation_event_identity(event: dict[str, Any]) -> bool:
    operation_id = event.get("operation_id")
    operation_revision = event.get("operation_revision")
    if event.get("record_kind") == "stop_acceptance_projected":
        request_id = event.get("request_id")
        return (
            operation_id is None
            and operation_revision is None
            and isinstance(request_id, str)
            and bool(request_id)
        )
    return (
        isinstance(operation_id, str)
        and bool(operation_id)
        and type(operation_revision) is int
        and 1 <= operation_revision <= MAXIMUM_SAFE_INTEGER
    )


MAX_EXECUTION_PACKET_BYTES = 1024 * 1024
MAX_EXECUTION_RESPONSE_BYTES = 4 * 1024 * 1024
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
MOTION_PREVIEW_COLLISION_REVISION_KEYS = frozenset(
    {"content_hash", "revision_id"}
)
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
    def status(self) -> dict[str, Any]: ...

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
        self._control_socket_path = state_root / EXECUTION_CONTROL_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    @staticmethod
    def _validate_socket(socket_path: Path) -> None:
        try:
            metadata = socket_path.lstat()
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

    def _exchange_on(
        self, command: dict[str, Any], socket_path: Path
    ) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_EXECUTION_PACKET_BYTES:
            raise HeadlessApiError(
                "execution_command_oversized",
                "execution command exceeds its byte bound",
            )
        self._validate_socket(socket_path)
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.setsockopt(
                    socket.SOL_SOCKET,
                    socket.SO_RCVBUF,
                    MAX_EXECUTION_RESPONSE_BYTES,
                )
                client.settimeout(self._timeout_seconds)
                client.connect(str(socket_path))
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

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._socket_path)

    def _exchange_control(self, command: dict[str, Any]) -> dict[str, Any]:
        return self._exchange_on(command, self._control_socket_path)

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
                or not _valid_operation_event_identity(event)
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
                "attribution": self._http_attribution(trusted_principal_id),
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
            or receipt.get("request_id")
            != operation["request_context"]["request_id"]
            or receipt.get("request_owner_epoch")
            != operation["request_context"]["request_owner_epoch"]
            or receipt.get("program_id") != operation["program_id"]
            or receipt.get("program_revision")
            != operation["program_revision"]
            or receipt.get("program_binding_id")
            != operation["program_binding_id"]
            or receipt.get("program_run_id") != operation["program_run_id"]
            or receipt.get("graph_revision")
            != operation["expected_graph_revision"]
            or receipt.get("retained_bundle_id")
            != operation["expected_retained_bundle_id"]
            or receipt.get("scene_revision") != expected_scene
            or receipt.get("component_role_id")
            != participant["component_role_id"]
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
            or set(collision_revision)
            != MOTION_PREVIEW_COLLISION_REVISION_KEYS
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
            "sha256:"
            + hashlib.sha256(_canonical_json(digest_input)).hexdigest()
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
                "attribution": self._http_attribution(trusted_principal_id),
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
            command_value.get("position")
            if isinstance(command_value, dict)
            else None
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
            or receipt.get("request_id")
            != operation["request_context"]["request_id"]
            or receipt.get("request_owner_epoch")
            != operation["request_context"]["request_owner_epoch"]
            or receipt.get("program_id") != operation["program_id"]
            or receipt.get("program_revision")
            != operation["program_revision"]
            or receipt.get("program_binding_id")
            != operation["program_binding_id"]
            or receipt.get("program_run_id") != operation["program_run_id"]
            or receipt.get("graph_revision")
            != operation["expected_graph_revision"]
            or receipt.get("retained_bundle_id")
            != operation["expected_retained_bundle_id"]
            or receipt.get("scene_revision")
            != operation["expected_scene_revision"]
            or receipt.get("component_role_id")
            != participant["component_role_id"]
            or receipt.get("target_role_id") != participant["target_role_id"]
            or receipt.get("required_capability_id")
            != participant["required_capability_id"]
            or receipt.get("command_schema_id")
            != payload.get("command_schema_id")
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
            or not (
                bounded_model_resource
                or unbounded_component_local_resource
            )
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
        response = self._exchange_control(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "attribution": self._http_attribution(trusted_principal_id),
                "command": "stop",
                "request": request.model_dump(mode="json", exclude_none=True),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id")
            != request.request_context.request_id
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
                "attribution": self._http_attribution(trusted_principal_id),
                "command": "full_stop",
                "request": request.model_dump(mode="json"),
            }
        )
        accepted = response.get("accepted")
        if response.get("ok") is True and (
            response.get("command") != "full_stop"
            or not isinstance(accepted, dict)
            or accepted.get("request_id")
            != request.request_context.request_id
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
                "attribution": self._http_attribution(trusted_principal_id),
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
        if any(not isinstance(latch.get(key), str) or not latch[key] for key in required_text):
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
                not isinstance(certificate.get(key), str)
                or not certificate[key]
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
        response = self._exchange(
            {
                "api_version": EXECUTION_COMMAND_VERSION,
                "command": "recover_stop_fault",
                "request": request.model_dump(mode="json", exclude_none=True),
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
