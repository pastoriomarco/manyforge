"""Headless's Manager commands: a thin adapter over the gateway core.

The core (``manyforge_headless_py.Gateway``) owns the Manager command socket,
the envelope and the scene commit's cross-field rules; each method here is its
request members, one core call and the reply interpretation that decides an
outcome or reshapes the reply. The Manager's encoder is the authority on the
reply's shape.
"""

from __future__ import annotations

from typing import Any, Protocol

from .models import (
    ManagerApplyRequest,
    ManagerApplyAcknowledgementContext,
    ManagerApplyStatus,
    ManagerDeploymentCandidatePreviewRequest,
    ProjectionRevisionRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
)
from .native import HeadlessApiError
from manyforge_behavior.reference_validation import validate_reference_refusal


_MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991


class ManagerPort(Protocol):
    def scene_runtime_owner(self) -> dict[str, Any]: ...

    def scene_projection_revision(self) -> dict[str, Any]: ...

    def preview_deployment_candidate(
        self, request: ManagerDeploymentCandidatePreviewRequest
    ) -> dict[str, Any]: ...

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]: ...

    def observe_apply(
        self, apply_id: int, apply_generation: int
    ) -> dict[str, Any]: ...

    def acknowledge_apply(
        self,
        apply_id: int,
        apply_generation: int,
        request_context: ManagerApplyAcknowledgementContext,
    ) -> dict[str, Any]: ...

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
        mesh_content_digest: str | None,
    ) -> dict[str, Any]: ...

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]: ...


def _transaction_body(
    request: SceneTransactionRequest, mesh_content_digest: str | None
) -> dict[str, Any]:
    """Wire body; absent optional members stay absent, not null.

    ``mesh_content_digest`` is the captured content of an API scene call's
    mesh (OD-335); without it the Manager fills the digest from the active
    Apply's captured meshes.
    """

    transaction = request.model_dump(mode="json")
    if mesh_content_digest is not None:
        transaction["operation"]["shape"]["mesh_content_digest"] = mesh_content_digest
    if transaction.get("program_node_evidence") is None:
        transaction.pop("program_node_evidence", None)
    for optional in ("grasp_lost_bound", "on_grasp_lost", "mass_kg", "carrier_component_id"):
        if transaction["operation"].get(optional, ...) is None:
            transaction["operation"].pop(optional)
    return transaction


_MANAGER_PRODUCER = "component_manager"
_MANAGER_ERROR_KEYS = frozenset({"code", "details", "message", "producer", "reference_validation"})


def _manager_error(error: object) -> HeadlessApiError | None:
    """The typed Manager refusal, or None when the error object is malformed.

    `{code, message}` with optional string `details` and the `producer` that
    produced it (the Manager unless it forwards another owner's refusal).
    """

    if (
        not isinstance(error, dict)
        or not {"code", "message"} <= set(error) <= _MANAGER_ERROR_KEYS
        or not isinstance(error["code"], str)
        or not error["code"]
        or not isinstance(error["message"], str)
        or not error["message"]
    ):
        return None
    details = error.get("details", {})
    producer = error.get("producer", _MANAGER_PRODUCER)
    if (
        not isinstance(details, dict)
        or len(details) > 4096
        or any(
            not isinstance(key, str) or not isinstance(value, str)
            for key, value in details.items()
        )
        or not isinstance(producer, str)
        or not producer
    ):
        return None
    if "reference_validation" in error:
        if error["code"] != "program_reference_validation_failed":
            return None
        reference = validate_reference_refusal(error["reference_validation"])
        if reference is None or set(details) & set(reference):
            return None
        details = {**details, **reference}
    return HeadlessApiError(
        error["code"], error["message"], details=details, producer=producer
    )


def _invalid(message: str) -> HeadlessApiError:
    return HeadlessApiError("manager_command_response_invalid", message)


def _raise_refusal(response: dict[str, Any], label: str) -> None:
    """Raise a refused reply as its typed Manager error."""

    typed = _manager_error(response.get("error"))
    if typed is None:
        raise _invalid(f"{label} refusal is invalid")
    raise typed


def _check_scene_result(response: object) -> None:
    """The outcome a scene commit result's consumers decide on.

    A success is a new commit or an exact replay and names its committed
    projection revision (the publication wait and the History record read
    it); a failure carries a typed error and no committed projection (its
    projection status tells refused from unknown).
    """

    if not isinstance(response, dict) or type(response.get("ok")) is not bool:
        raise _invalid("scene commit result has no boolean outcome")
    if response["ok"]:
        if (
            response.get("error") is not None
            or response.get("projection_status") != "committed"
            or response.get("replay_status") not in {"accepted_new", "replayed_exact"}
        ):
            raise _invalid("successful scene commit response is internally inconsistent")
        try:
            ProjectionRevisionRequest.model_validate(
                response.get("committed_projection_revision")
            )
        except ValueError as exc:
            raise _invalid(
                "scene commit response has an invalid projection revision"
            ) from exc
    elif (
        _manager_error(response.get("error")) is None
        or response.get("projection_status") == "committed"
        or response.get("committed_projection_revision") is not None
    ):
        raise _invalid("failed scene commit response is internally inconsistent")


class ManagerCommandClient:
    """Forward one command at a time; never cache or reduce Manager truth.

    ``gateway`` is Headless's ``manyforge_headless_py.Gateway`` (HTTP
    attribution); its typed failures become ``HeadlessApiError`` with the
    same code, message, details and producer.
    """

    def __init__(self, gateway: Any) -> None:
        import manyforge_headless_py as core  # type: ignore[import-not-found]

        self._gateway = gateway
        self._gateway_error: type[Exception] = core.GatewayError

    def _call(self, command: str, members: dict[str, Any] | None = None) -> dict[str, Any]:
        try:
            return getattr(self._gateway, command)(members or {})
        except self._gateway_error as error:
            raise self._error(error) from error

    @staticmethod
    def _error(error: Any) -> HeadlessApiError:
        return HeadlessApiError(
            error.code, error.message, details=error.details, producer=error.producer
        )

    def scene_runtime_owner(self) -> dict[str, Any]:
        """Read the existing Scene owner identity when a viewer source starts."""
        response = self._call("scene_runtime_owner_read")
        if response["ok"] is False:
            _raise_refusal(response, "Scene runtime owner")
        return response

    def scene_projection_revision(self) -> dict[str, Any]:
        """The Scene owner's current projection revision (``scene_owner_read``)."""
        response = self._call("scene_owner_read")
        if response["ok"] is False:
            _raise_refusal(response, "Scene owner read")
        return response["projection_revision"]

    def preview_deployment_candidate(
        self, request: ManagerDeploymentCandidatePreviewRequest
    ) -> dict[str, Any]:
        return self._call(
            "deployment_candidate_preview",
            {
                "deployment_revision": request.deployment_revision,
                "candidate_retained_bundle_id": request.candidate_retained_bundle_id,
                "candidate_graph_revision": request.candidate_graph_revision,
                "prepared_activation_id": request.prepared_activation_id,
                "expected_admission_generation": request.expected_admission_generation,
                "expected_current": request.expected_current.model_dump(),
            },
        )

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        response = self._call(
            "apply",
            {
                "accepted_impact_digest": request.accepted_impact_digest,
                "candidate_receipt": request.candidate_receipt.model_dump(),
                "deployment_revision": request.deployment_revision,
                "expected_current": request.expected_current.model_dump(),
                "reject_if_affected_busy": request.reject_if_affected_busy,
                "prepared_activation": request.prepared_activation.model_dump(),
                "scene_initialization": request.scene_initialization,
                "request_key": request.request_key,
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
            },
        )
        error = response.get("error")
        if response["ok"] is False and isinstance(error, dict) and "reference_validation" in error:
            # The reference diagnostics become the typed refusal's details.
            _raise_refusal(response, "Manager Apply")
        return response

    def _apply_status_command(
        self,
        command_name: str,
        label: str,
        apply_id: int,
        apply_generation: int,
        request_context: ManagerApplyAcknowledgementContext | None = None,
    ) -> dict[str, Any]:
        # ``apply_id`` is the Manager's Apply number (contract 1.16.1).
        if (
            type(apply_id) is not int
            or not 1 <= apply_id <= _MAXIMUM_SAFE_INTEGER
            or type(apply_generation) is not int
            or not 1 <= apply_generation <= _MAXIMUM_SAFE_INTEGER
        ):
            raise HeadlessApiError(
                "manager_apply_status_invalid",
                f"{label} requires one bounded durable identity",
            )
        members: dict[str, Any] = {
            "apply_generation": apply_generation,
            "apply_id": apply_id,
        }
        if request_context is not None:
            members["request_context"] = request_context.model_dump(
                mode="json", exclude_none=True
            )
        response = self._call(command_name, members)
        if response["ok"] is False:
            return response
        # The Apply lifecycle the routes return and the Apply follower reads.
        try:
            ManagerApplyStatus.model_validate(
                {name: response[name] for name in ManagerApplyStatus.model_fields}
            )
        except (KeyError, TypeError, ValueError) as error:
            raise _invalid(f"{label} response is malformed") from error
        return response

    def observe_apply(
        self, apply_id: int, apply_generation: int
    ) -> dict[str, Any]:
        return self._apply_status_command(
            "apply_status", "Manager Apply status", apply_id, apply_generation
        )

    def acknowledge_apply(
        self,
        apply_id: int,
        apply_generation: int,
        request_context: ManagerApplyAcknowledgementContext,
    ) -> dict[str, Any]:
        return self._apply_status_command(
            "apply_acknowledge",
            "Manager Apply acknowledgement",
            apply_id,
            apply_generation,
            request_context,
        )

    def release_failed_apply_for_new(
        self, request_context: ManagerApplyAcknowledgementContext
    ) -> dict[str, Any]:
        """New: release whatever failed Apply still holds the cell.

        ``released`` names that Apply, or is ``None`` when none holds it.
        """

        return self._call(
            "apply_release_for_new",
            {
                "request_context": request_context.model_dump(
                    mode="json", exclude_none=True
                ),
            },
        )

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
        mesh_content_digest: str | None,
    ) -> dict[str, Any]:
        """The core's scene commit: its cross-field rules, the caller's
        attribution and the exchange; the result's outcome is checked."""

        try:
            response = self._gateway.scene_commit(
                _transaction_body(request, mesh_content_digest), trusted_principal_id
            )
        except self._gateway_error as error:
            raise self._error(error) from error
        _check_scene_result(response)
        return response

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]:
        response = self._call(
            "scene_pose_query",
            {
                "expected_projection_revision": (
                    request.expected_projection_revision.model_dump()
                ),
                "scene_resource_ids": request.scene_resource_ids,
            },
        )
        if response["ok"] is not True:
            _raise_refusal(response, "Manager scene pose query")
        # The owner resolves against its current projection: a newer
        # revision of the same Scene is the latest truth; it refuses a
        # lineage change itself.
        return {
            "api_version": "manyforge.scene_resource_pose_response.v1",
            "poses": response.get("poses"),
            "projection_revision": response.get("projection_revision"),
        }
