"""Bounded same-user client for the Manager daemon's private protocol."""

from __future__ import annotations

import math
import os
from pathlib import Path
import socket
import stat
from typing import Any, Protocol
import unicodedata

import rfc8785

from .models import (
    ManagerApplyRequest,
    ProjectionRevisionRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
)
from .native import (
    DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    HeadlessApiError,
    MAX_MANAGER_COMMAND_TIMEOUT_SECONDS,
    strict_json_object,
)


MANAGER_SOCKET_FILE = "manager-command.sock"
MANAGER_COMMAND_VERSION = "manyforge.manager_command.v1"
MANAGER_RESPONSE_VERSION = "manyforge.manager_command_response.v1"
MAX_MANAGER_PACKET_BYTES = 128 * 1024 + 4 * 1024
MAX_MANAGER_RESPONSE_BYTES = 64 * 1024
_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
_SCENE_REPLAY_STATUSES = {
    "accepted_new",
    "replayed_exact",
    "request_result_expired",
    "request_id_conflict",
    "request_ledger_full",
    "stale_request_owner_epoch",
}
_SCENE_PROJECTION_STATUSES = {
    "not_attempted",
    "committed",
    "rejected_rollback_proven",
    "failed_projection_uncertain",
}
_SCENE_UPDATE_STATUSES = {"geometry_changed", "freshness_only", "no_op"}


class ManagerPort(Protocol):
    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]: ...

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]: ...


def _canonical_json(value: dict[str, Any]) -> bytes:
    try:
        return rfc8785.dumps(value)
    except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
        raise HeadlessApiError(
            "manager_command_invalid",
            f"Manager command is not canonically encodable: {exc}",
        ) from exc


class ManagerCommandClient:
    """Forward one packet at a time; never cache or reduce Manager truth."""

    def __init__(
        self,
        state_root: Path,
        *,
        timeout_seconds: float = DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    ) -> None:
        if (
            timeout_seconds <= 0
            or timeout_seconds > MAX_MANAGER_COMMAND_TIMEOUT_SECONDS
        ):
            raise ValueError("Manager timeout must be in (0, 60] seconds")
        self._socket_path = state_root / MANAGER_SOCKET_FILE
        self._timeout_seconds = timeout_seconds

    def _validate_socket(self) -> None:
        try:
            metadata = self._socket_path.lstat()
        except FileNotFoundError as exc:
            raise HeadlessApiError(
                "manager_command_unavailable",
                "Manager command socket is absent",
            ) from exc
        if not stat.S_ISSOCK(metadata.st_mode) or metadata.st_uid != os.geteuid():
            raise HeadlessApiError(
                "manager_command_unavailable",
                "Manager command path is not the expected same-user socket",
            )

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_MANAGER_PACKET_BYTES:
            raise HeadlessApiError(
                "manager_command_oversized",
                "Manager command exceeds its byte bound",
            )
        self._validate_socket()
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(self._timeout_seconds)
                client.connect(str(self._socket_path))
                sent = client.send(encoded)
                if sent != len(encoded):
                    raise HeadlessApiError(
                        "manager_command_transport_failure",
                        "Manager command packet was truncated",
                    )
                payload, _ancillary, flags, _address = client.recvmsg(
                    MAX_MANAGER_RESPONSE_BYTES
                )
        except HeadlessApiError:
            raise
        except (OSError, TimeoutError) as exc:
            raise HeadlessApiError(
                "manager_command_unavailable",
                f"Manager command transport failed: {exc}",
            ) from exc
        if flags & socket.MSG_TRUNC or not payload:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response is empty or oversized",
            )
        try:
            text = payload.decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response is not UTF-8",
            ) from exc
        response = strict_json_object(text, label="Manager command response")
        if _canonical_json(response) != payload:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response is not canonical JSON",
            )
        if response.get("api_version") != MANAGER_RESPONSE_VERSION:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response version is unsupported",
            )
        if type(response.get("ok")) is not bool:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager command response has no boolean result",
            )
        return response

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "apply",
                "correlation_id": request.correlation_id,
                "deployment_artifact_id": request.deployment_artifact_id,
                "deployment_revision": request.deployment_revision,
                "expected_current": request.expected_current.model_dump(),
                "reject_if_affected_busy": request.reject_if_affected_busy,
                "request_id": request.request_id,
                "request_owner_epoch": request.request_owner_epoch,
                "source_label": request.source_label,
            }
        )
        if (
            response.get("request_id") != request.request_id
            or response.get("request_owner_epoch") != request.request_owner_epoch
        ):
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "Manager Apply response does not match the submitted request",
            )
        return response

    @staticmethod
    def _validate_scene_response(
        response: dict[str, Any], request: SceneTransactionRequest
    ) -> None:
        expected_fields = {
            "api_version",
            "committed_projection_revision",
            "error",
            "ok",
            "projection_status",
            "replay_status",
            "request_id",
            "request_owner_epoch",
            "transaction_sequence",
            "update_status",
        }
        context = request.request_context
        if set(response) != expected_fields:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene commit response has an unknown or missing field",
            )
        if (
            response.get("request_id") != context.request_id
            or response.get("request_owner_epoch") != context.request_owner_epoch
        ):
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "Manager scene response does not match the submitted request",
            )
        if response.get("replay_status") not in _SCENE_REPLAY_STATUSES:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene commit response has an invalid replay status",
            )
        projection_status = response.get("projection_status")
        if projection_status not in _SCENE_PROJECTION_STATUSES:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene commit response has an invalid projection status",
            )
        error = response.get("error")
        if error is not None:
            if (
                not isinstance(error, dict)
                or set(error) != {"code", "details", "message"}
                or not isinstance(error.get("code"), str)
                or not error["code"]
                or not isinstance(error.get("message"), str)
                or not error["message"]
                or not isinstance(error.get("details"), dict)
                or any(
                    not isinstance(key, str) or not isinstance(value, str)
                    for key, value in error["details"].items()
                )
            ):
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    "scene commit response has an invalid error",
                )
        sequence = response.get("transaction_sequence")
        if sequence is not None and (
            type(sequence) is not int
            or sequence < 1
            or sequence > _MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene commit response has an invalid transaction sequence",
            )
        projection = response.get("committed_projection_revision")
        parsed_projection: ProjectionRevisionRequest | None = None
        if projection is not None:
            try:
                parsed_projection = ProjectionRevisionRequest.model_validate(projection)
            except ValueError as exc:
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    "scene commit response has an invalid projection revision",
                ) from exc
        update_status = response.get("update_status")
        success = response["ok"]
        if success:
            if (
                error is not None
                or sequence is None
                or projection is None
                or projection_status != "committed"
                or update_status not in _SCENE_UPDATE_STATUSES
                or response["replay_status"]
                not in {"accepted_new", "replayed_exact"}
            ):
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    "successful scene commit response is internally inconsistent",
                )
            assert parsed_projection is not None
            expected = request.expected_projection_revision
            if (
                parsed_projection.retained_bundle_id
                != expected.retained_bundle_id
                or parsed_projection.graph_revision != expected.graph_revision
                or parsed_projection.scene_revision.scene_id
                != expected.scene_revision.scene_id
            ):
                raise HeadlessApiError(
                    "manager_command_response_identity_mismatch",
                    "Manager scene response changed structural or "
                    "scene-lineage identity",
                )
        elif (
            error is None
            or sequence is not None
            or projection is not None
            or projection_status == "committed"
            or update_status is not None
        ):
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "failed scene commit response is internally inconsistent",
            )

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        if trusted_principal_id is not None and (
            not trusted_principal_id
            or unicodedata.normalize("NFC", trusted_principal_id)
            != trusted_principal_id
            or len(trusted_principal_id.encode("utf-8")) > 256
        ):
            raise HeadlessApiError(
                "scene_request_attribution_invalid",
                "trusted scene principal must be bounded non-empty NFC text",
            )
        attribution = {
            "assurance": (
                "externally_authenticated"
                if trusted_principal_id is not None
                else "unverified"
            ),
            "principal_id": trusted_principal_id,
            "transport": "http",
        }
        transaction = request.model_dump(mode="json")
        transaction["attribution"] = attribution
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "scene_commit",
                "transaction": transaction,
            }
        )
        self._validate_scene_response(response, request)
        return response

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "scene_pose_query",
                "expected_projection_revision": (
                    request.expected_projection_revision.model_dump()
                ),
                "scene_resource_ids": request.scene_resource_ids,
            }
        )
        if response.get("ok") is not True:
            error = response.get("error")
            code = error.get("code") if isinstance(error, dict) else None
            message = error.get("message") if isinstance(error, dict) else None
            details = error.get("details") if isinstance(error, dict) else None
            if details is not None and (
                not isinstance(details, dict)
                or not all(
                    isinstance(key, str) and isinstance(value, str)
                    for key, value in details.items()
                )
            ):
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    "Manager scene pose error details are invalid",
                )
            raise HeadlessApiError(
                code if isinstance(code, str) and code else "scene_pose_unavailable",
                message
                if isinstance(message, str) and message
                else "Manager scene pose query was rejected",
                details=details if isinstance(details, dict) else None,
            )
        if set(response) != {
            "api_version",
            "ok",
            "poses",
            "projection_revision",
        }:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene pose response has an unknown or missing field",
            )
        try:
            projection = ProjectionRevisionRequest.model_validate(
                response["projection_revision"]
            )
        except (KeyError, ValueError) as exc:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene pose response has an invalid projection revision",
            ) from exc
        if projection != request.expected_projection_revision:
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "scene pose response changed the expected projection revision",
            )
        poses = response.get("poses")
        if not isinstance(poses, list) or len(poses) != len(
            request.scene_resource_ids
        ):
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "scene pose response is incomplete",
            )
        parsed: list[dict[str, Any]] = []
        for index, (expected_id, row) in enumerate(
            zip(request.scene_resource_ids, poses, strict=True)
        ):
            if (
                not isinstance(row, dict)
                or set(row) != {"pose_in_cell", "scene_resource_id"}
                or row.get("scene_resource_id") != expected_id
            ):
                raise HeadlessApiError(
                    "manager_command_response_identity_mismatch",
                    f"scene pose response row {index} changed object identity",
                )
            pose = row.get("pose_in_cell")
            if not isinstance(pose, dict) or set(pose) != {
                "orientation_xyzw",
                "position_m",
            }:
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    f"scene pose response row {index} has an invalid pose",
                )
            position = pose.get("position_m")
            orientation = pose.get("orientation_xyzw")
            if (
                not isinstance(position, dict)
                or set(position) != {"x", "y", "z"}
                or not isinstance(orientation, list)
                or len(orientation) != 4
                or any(
                    isinstance(value, bool)
                    or not isinstance(value, (int, float))
                    or not math.isfinite(float(value))
                    for value in (*position.values(), *orientation)
                )
            ):
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    f"scene pose response row {index} has invalid coordinates",
                )
            norm = math.sqrt(sum(float(value) ** 2 for value in orientation))
            if abs(norm - 1.0) > 1.0e-6:
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    f"scene pose response row {index} has a non-unit quaternion",
                )
            parsed.append(row)
        return {
            "api_version": "manyforge.scene_resource_pose_response.v1",
            "poses": parsed,
            "projection_revision": projection.model_dump(),
        }
