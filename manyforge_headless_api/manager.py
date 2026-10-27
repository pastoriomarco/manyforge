"""Bounded same-user client for the Manager daemon's private protocol."""

from __future__ import annotations

import math
from pathlib import Path
import socket
from typing import Any, Protocol
import unicodedata

import rfc8785
from pydantic import ValidationError

from .models import (
    ManagerApplyRequest,
    ComponentProcessStopRequest,
    ComponentProcessStopResult,
    ManagerApplyStatus,
    ManagerDeploymentCandidatePreviewRequest,
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
from .private_transport import (
    PinnedRunRoot,
    PrivateTransportCapabilityError,
)
from .close_wire import CanonicalEmptySelection


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
    def component_process_stop_offer(self, component_id: str) -> dict[str, Any]: ...

    def canonical_empty_selection(self) -> dict[str, Any]: ...

    def stop_component_process(self, request: ComponentProcessStopRequest) -> dict[str, Any]: ...

    def component_process_stop_result(self, request: ComponentProcessStopRequest) -> dict[str, Any]: ...

    def preview_deployment_candidate(
        self, request: ManagerDeploymentCandidatePreviewRequest
    ) -> dict[str, Any]: ...

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]: ...

    def observe_apply(
        self, apply_id: str, apply_generation: int
    ) -> dict[str, Any]: ...

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]: ...

    def scene_transaction_status(self, request: SceneTransactionRequest) -> dict[str, Any]: ...

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]: ...


def _transaction_body(request: SceneTransactionRequest) -> dict[str, Any]:
    """Wire body; absent optional members stay absent, not null."""

    transaction = request.model_dump(mode="json")
    if transaction.get("program_node_evidence") is None:
        transaction.pop("program_node_evidence", None)
    if transaction["operation"].get("grasp_lost_bound", ...) is None:
        transaction["operation"].pop("grasp_lost_bound")
    return transaction


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
        run_root: Path,
        *,
        timeout_seconds: float = DEFAULT_MANAGER_COMMAND_TIMEOUT_SECONDS,
    ) -> None:
        if (
            timeout_seconds <= 0
            or timeout_seconds > MAX_MANAGER_COMMAND_TIMEOUT_SECONDS
        ):
            raise ValueError("Manager timeout must be in (0, 60] seconds")
        try:
            self._run_root = PinnedRunRoot(run_root)
        except PrivateTransportCapabilityError as error:
            raise HeadlessApiError(
                "manager_command_unavailable",
                f"Manager run capability is invalid: {error}",
            ) from error
        self._timeout_seconds = timeout_seconds

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        encoded = _canonical_json(command)
        if len(encoded) > MAX_MANAGER_PACKET_BYTES:
            raise HeadlessApiError(
                "manager_command_oversized",
                "Manager command exceeds its byte bound",
            )
        try:
            with self._run_root.socket_address(
                MANAGER_SOCKET_FILE
            ) as socket_address:
                with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                    client.settimeout(self._timeout_seconds)
                    client.connect(socket_address)
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
        except PrivateTransportCapabilityError as exc:
            raise HeadlessApiError(
                "manager_command_unavailable",
                f"Manager command socket is unavailable: {exc}",
            ) from exc
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

    def _component_process_stop(
        self, command: str, request: ComponentProcessStopRequest,
    ) -> dict[str, Any]:
        response = self._exchange({
            "api_version": MANAGER_COMMAND_VERSION,
            "command": command,
            "request": request.model_dump(mode="json"),
        })
        if response["ok"] is False:
            error = response.get("error")
            if (set(response) not in ({"api_version", "ok", "error"}, {"api_version", "ok", "error", "result"})
                    or response.get("result") is not None or not isinstance(error, dict)
                    or set(error) not in ({"code", "message"}, {"code", "message", "details"})
                    or ("result" in response and "details" not in error)
                    or not isinstance(error["code"], str) or not error["code"]
                    or not isinstance(error["message"], str) or not error["message"]
                    or ("details" in error and (not isinstance(error["details"], dict)
                        or len(error["details"]) > 4096
                        or any(not isinstance(key, str) or not isinstance(value, str)
                               for key, value in error["details"].items())))):
                raise HeadlessApiError("manager_command_response_invalid", "Process Stop error envelope is invalid")
            raise HeadlessApiError(error["code"], error["message"], details=error.get("details", {}))
        if set(response) != {"api_version", "ok", "error", "result"}:
            raise HeadlessApiError("manager_command_response_invalid", "Process Stop response envelope is invalid")
        if response["error"] is not None:
            raise HeadlessApiError("manager_command_response_invalid", "Successful process Stop response carries an error")
        try:
            result = ComponentProcessStopResult.model_validate(response["result"])
        except ValidationError as error:
            raise HeadlessApiError("manager_command_response_invalid", "Process Stop result is invalid") from error
        if result.request != request:
            raise HeadlessApiError("manager_command_response_identity_mismatch", "Process Stop result does not match the exact launch request")
        return response["result"]

    def component_process_stop_offer(self, component_id: str) -> dict[str, Any]:
        response = self._exchange({"api_version": MANAGER_COMMAND_VERSION,
            "command": "component_process_stop_offer", "controllable_component_id": component_id})
        if response["ok"] is False:
            error = response.get("error")
            if not isinstance(error, dict) or not isinstance(error.get("code"), str) or not isinstance(error.get("message"), str):
                raise HeadlessApiError("manager_command_response_invalid", "Process Stop offer error is invalid")
            raise HeadlessApiError(error["code"], error["message"], details=error.get("details", {}))
        result = response.get("result")
        if (set(response) != {"api_version", "ok", "error", "result"} or response["error"] is not None
                or not isinstance(result, dict) or set(result) != {"controllable_component_id", "process_inventories"}
                or result["controllable_component_id"] != component_id or not isinstance(result["process_inventories"], list)):
            raise HeadlessApiError("manager_command_response_invalid", "Process Stop offer is invalid")
        return result

    def stop_component_process(self, request: ComponentProcessStopRequest) -> dict[str, Any]:
        return self._component_process_stop("stop_component_process", request)

    def canonical_empty_selection(self) -> dict[str, Any]:
        response = self._exchange({
            "api_version": MANAGER_COMMAND_VERSION,
            "command": "canonical_empty_selection",
        })
        if response["ok"] is False:
            error = response.get("error")
            if (set(response) != {"api_version", "ok", "error"}
                    or not isinstance(error, dict)
                    or set(error) not in ({"code", "message"}, {"code", "message", "details"})
                    or not isinstance(error.get("code"), str) or not error["code"]
                    or not isinstance(error.get("message"), str) or not error["message"]
                    or ("details" in error and (not isinstance(error["details"], dict)
                        or any(not isinstance(k, str) or not isinstance(v, str) for k, v in error["details"].items())))):
                raise HeadlessApiError("manager_command_response_invalid", "Canonical-empty selection error is invalid")
            raise HeadlessApiError(error["code"], error["message"], details=error.get("details", {}))
        if set(response) != {"api_version", "ok", "canonical_empty_selection"}:
            raise HeadlessApiError("manager_command_response_invalid", "Canonical-empty selection envelope is invalid")
        try:
            CanonicalEmptySelection.model_validate(response["canonical_empty_selection"])
        except ValidationError as error:
            raise HeadlessApiError("manager_command_response_invalid", "Canonical-empty selection is invalid") from error
        return response["canonical_empty_selection"]

    def component_process_stop_result(self, request: ComponentProcessStopRequest) -> dict[str, Any]:
        return self._component_process_stop("component_process_stop_result", request)

    def preview_deployment_candidate(
        self, request: ManagerDeploymentCandidatePreviewRequest
    ) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "deployment_candidate_preview",
                "deployment_artifact_id": request.deployment_artifact_id,
                "deployment_revision": request.deployment_revision,
                "candidate_retained_bundle_id": request.candidate_retained_bundle_id,
                "candidate_graph_revision": request.candidate_graph_revision,
                "prepared_activation_id": request.prepared_activation_id,
                "expected_admission_generation": (
                    request.expected_admission_generation
                ),
                "expected_current": request.expected_current.model_dump(),
                "request_owner_epoch": request.request_owner_epoch,
            }
        )
        return response

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "apply",
                "accepted_impact_digest": request.accepted_impact_digest,
                "candidate_receipt": request.candidate_receipt.model_dump(),
                "correlation_id": request.correlation_id,
                "deployment_artifact_id": request.deployment_artifact_id,
                "deployment_revision": request.deployment_revision,
                "expected_current": request.expected_current.model_dump(),
                "reject_if_affected_busy": request.reject_if_affected_busy,
                "prepared_activation": request.prepared_activation.model_dump(),
                "scene_initialization": request.scene_initialization,
                "request_id": request.request_id,
                "request_owner_epoch": request.request_owner_epoch,
                "source_label": request.source_label,
                "admission_context": (
                    {
                        "kind": "direct",
                        "expected_admission_generation": (
                            request.expected_admission_generation
                        ),
                    }
                    if request.parent_authoring_claim is None
                    else {
                        "kind": "parent_authoring_claim",
                        "claim": request.parent_authoring_claim,
                    }
                ),
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

    def observe_apply(
        self, apply_id: str, apply_generation: int
    ) -> dict[str, Any]:
        if (
            not isinstance(apply_id, str)
            or not apply_id
            or len(apply_id.encode("utf-8")) > 256
            or type(apply_generation) is not int
            or not 1 <= apply_generation <= _MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "manager_apply_status_invalid",
                "Manager Apply status requires one bounded durable identity",
            )
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "apply_generation": apply_generation,
                "apply_id": apply_id,
                "command": "apply_status",
            }
        )
        if response.get("ok") is False:
            error = response.get("error")
            if (
                set(response) != {"api_version", "error", "ok"}
                or not isinstance(error, dict)
                or set(error) != {"code", "message"}
                or not all(
                    isinstance(error.get(name), str) and error[name]
                    for name in ("code", "message")
                )
            ):
                raise HeadlessApiError(
                    "manager_command_response_invalid",
                    "Manager Apply status rejection is malformed",
                )
            return response
        members = set(ManagerApplyStatus.model_fields)
        if set(response) != (members | {"api_version", "error", "ok"}) or (
            response.get("error") is not None
        ):
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager Apply status response is malformed",
            )
        try:
            status = ManagerApplyStatus.model_validate(
                {name: response[name] for name in members}
            )
        except (TypeError, ValueError) as error:
            raise HeadlessApiError(
                "manager_command_response_invalid",
                "Manager Apply status response is malformed",
            ) from error
        if (
            status.apply_id != apply_id
            or status.apply_generation != apply_generation
        ):
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "Manager Apply status names another durable operation",
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
        if (not isinstance(response, dict) or set(response) != expected_fields
            or response.get("api_version") != MANAGER_RESPONSE_VERSION
            or type(response.get("ok")) is not bool):
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
        transaction = _transaction_body(request)
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

    def scene_transaction_status(self, request: SceneTransactionRequest) -> dict[str, Any]:
        transaction = _transaction_body(request)
        # Attribution is not mutation intent. Lookup performs no Scene write.
        transaction["attribution"] = {
            "assurance": "unverified", "principal_id": None, "transport": "http"}
        response = self._exchange({"api_version": MANAGER_COMMAND_VERSION,
                                   "command": "scene_transaction_status", "transaction": transaction})
        if (set(response) != {"api_version", "ok", "result"}
            or response.get("api_version") != MANAGER_RESPONSE_VERSION
            or response.get("ok") is not True):
            raise HeadlessApiError("manager_command_response_invalid", "Scene result lookup is malformed")
        if response["result"] is not None:
            self._validate_scene_response(response["result"], request)
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
        # The owner resolves against its current projection: a newer
        # revision of the same Scene is the latest truth, a lineage change
        # or an older revision is not.
        expected = request.expected_projection_revision
        if (
            projection.retained_bundle_id != expected.retained_bundle_id
            or projection.graph_revision != expected.graph_revision
            or projection.scene_revision.scene_id != expected.scene_revision.scene_id
            or projection.scene_revision.revision_id
            < expected.scene_revision.revision_id
            or (
                projection.scene_revision.revision_id
                == expected.scene_revision.revision_id
                and projection != expected
            )
        ):
            raise HeadlessApiError(
                "manager_command_response_identity_mismatch",
                "scene pose response changed the expected projection lineage",
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
