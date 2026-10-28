"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import math
from pathlib import Path
from typing import Any, Callable, Literal, NoReturn

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    raw_sha256_digest,
    StrictArtifactError,
)
from manyforge_behavior.model_source_capture import capture_deployment_model_sources
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
)
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError
from manyforge_headless_api.program_binding_projection import (
    validate_applied_program_selection,
)
from pydantic import BaseModel, ConfigDict, Field

from .draft_session import (
    DraftSessionError,
    OPERATOR_DRAFT_MUTATIONS,
)
from .authoring_forest import AuthoringForestError, derive_candidate, from_program
from .expression_check import ExpressionCheckRefusal, check_draft_expression
from .authoring_identity import thaw_json
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .reset_apply_log import log_reset_apply_link
from .program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    normalize_program_save_path,
    save_program_artifact,
)
from .scenario_restore import (
    saved_deployment_differs_from,
    saved_deployment_restore,
    saved_program_restore,
)
from .state import ComposerState
from .authoring_admission import (
    AuthoringMutationEnvelope,
    OperationEffects,
    draft_session_http_exception,
    headless_client_http_exception,
    raise_save_failure,
    scenario_artifact_http_exception,
)
from .authoring_workspace import (
    AuthoringOperationContext,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
    scenario_project_facts,
)
from .scenario_artifacts import (
    SCENARIO_PROJECT_MEMBERS,
    ScenarioArtifactError,
    save_scenario_member,
)
from .authoring_workspace_fence import (
    commit_draft_edit,
    commit_local_authoring,
    require_captured_workspace_identity,
)


_AUTHORING_MUTATIONS = frozenset(
    {
        "blackboard_draft_remove_keys",
        "blackboard_draft_remove_namespaces",
        "blackboard_draft_upsert_keys",
        "blackboard_draft_upsert_namespaces",
        "program_draft_remove_parameters",
        "program_draft_upsert_parameters",
        "program_draft_update_motion_defaults",
        "scene_draft_add_object",
        "scene_draft_remove_objects",
        "scene_draft_update_object",
        "scene_draft_upsert_objects",
        "tree_draft_change_node_kind",
        "tree_draft_delete_node",
        "tree_draft_insert_node",
        "tree_draft_move_node",
        "tree_draft_rename_node",
        "tree_draft_replace_subtree",
        "tree_draft_update_positions",
        "tree_draft_update_node_params",
        "tree_draft_wrap_node",
        *OPERATOR_DRAFT_MUTATIONS,
    }
)
_SOURCE_RESOLVED_MUTATIONS = frozenset(
    {
        "program_draft_store_observed_move",
        "program_draft_store_observed_scene_resource",
    }
)
_DTO_CONFIG = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
class _DraftFence(BaseModel):
    model_config = _DTO_CONFIG

    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: int = Field(
        alias="expectedDraftRevision",
        ge=0,
        le=9_007_199_254_740_991,
    )
    expected_admission_generation: int = Field(
        alias="expectedAdmissionGeneration", ge=1, le=9_007_199_254_740_991
    )
    request_owner_epoch: str = Field(
        alias="requestOwnerEpoch", min_length=1, max_length=256
    )
    request_id: str = Field(alias="requestId", min_length=1, max_length=256)
    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )

    def require_workspace_fence(self, identity) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
        )


class _LocalDraftFence(_DraftFence):
    """Migrated ordinary-edit fence: the Program Draft plus the workspace.

    Program Apply, Revert and Scenario Load stay on the external-operation path
    and keep the plain :class:`_DraftFence` shape.
    """

    pass


# A fixed-input correction links its Draft edit and Apply to the context it
# recorded before Reset. The correlation is a reference, never a payload or an
# authority; ordinary edits and Applies omit it.
_CorrelationField = Field(
    default=None, alias="correlationId", min_length=1, max_length=256
)


class DraftMutationRequest(_LocalDraftFence):
    mutation: str = Field(min_length=1, max_length=128)
    arguments: dict[str, Any]


class OpenDraftRequest(_LocalDraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class NewDraftRequest(_LocalDraftFence):
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class ValidateProgramSourceRequest(BaseModel):
    model_config = _DTO_CONFIG
    authoring: dict[str, Any]


class ExpressionCheckRequest(BaseModel):
    model_config = _DTO_CONFIG
    # Longer text is still checked, so the engine reports text_too_long.
    expression: str = Field(max_length=65_536)
    destination_type: Literal["bool", "int64", "float64", "string"]
    exclude_computed_key: str | None = Field(default=None, min_length=1, max_length=256)


class SaveDraftRequest(_LocalDraftFence):
    pass


class SaveDraftAsRequest(_LocalDraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class ApplyDraftRequest(_DraftFence):
    correlation_id: str | None = _CorrelationField


class DiscardDraftRequest(_LocalDraftFence):
    pass


class RevertDraftRequest(_DraftFence):
    pass


@dataclass(frozen=True, slots=True)
class _AppliedContext:
    scenario_id: str
    source_document_digest: str
    program_revision: str
    record_digest: str
    program_document: dict[str, Any]
    binding_document: dict[str, Any]
    projection_document: dict[str, Any]


def _raise_workspace_error(error: Exception) -> NoReturn:
    if isinstance(error, DraftSessionError):
        raise draft_session_http_exception(error) from error
    if isinstance(error, ScenarioArtifactError):
        raise scenario_artifact_http_exception(error) from error
    if isinstance(error, ProgramArtifactError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, AuthoringWorkspaceError):
        detail = {"code": error.code, "message": str(error)}
        if error.detail != str(error):
            detail["context"] = error.detail
        raise HTTPException(status_code=error.status_code, detail=detail) from error
    if isinstance(error, ActiveNodeEnvironmentProjectionError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    raise error


def _error_code(error: Exception) -> str:
    code = getattr(error, "code", None)
    return code if isinstance(code, str) and code else type(error).__name__


def _active_catalog(state: ComposerState):
    if state.headless_client is None:
        return state.node_catalog
    return read_active_node_environment(state).catalog


def _non_executable_applied_context(
    state: ComposerState,
) -> tuple[_AppliedContext | None, dict[str, Any] | None]:
    assert state.headless_client is not None
    try:
        status = state.headless_client.program_applied_status()
    except HeadlessClientError as error:
        return None, {
            "code": error.code,
            "message": str(error),
            "restorationGates": [],
        }
    program = status.get("program") if isinstance(status, dict) else None
    publication = None
    try:
        publication = state._published_program_projection(status)
        if publication is None:
            raise ValueError("non-executable Scenario publication is absent")
        program_identity = Program.from_dict(
            program,
            _active_catalog(state),
            path="<headless-applied-program>",
        ).program_revision
    except (
        ProgramValidationError,
        ActiveNodeEnvironmentProjectionError,
        HeadlessClientError,
        TypeError,
        ValueError,
        StrictArtifactError,
    ):
        program_identity = None
    if (
        publication is None
        or not isinstance(status, dict)
        or status.get("executable") is not False
        or not isinstance(program, dict)
        or program.get("program_id") != status.get("program_id")
        or program_identity != status.get("program_revision")
    ):
        return None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program status",
            "restorationGates": [],
        }
    return (
        _AppliedContext(
            scenario_id=publication["scenario_id"],
            source_document_digest=publication["source_document_digest"],
            program_revision=status["program_revision"],
            record_digest=status["record_digest"],
            program_document=program,
            binding_document={},
            projection_document=status,
        ),
        None,
    )


def _applied_context(
    state: ComposerState,
) -> tuple[_AppliedContext | None, dict[str, Any] | None]:
    if state.headless_client is None:
        return None, {
            "code": "program_apply_unavailable",
            "message": "authoritative Program Apply requires a configured headless API",
            "restorationGates": [],
        }
    try:
        applied = state.headless_client.program_applied()
    except HeadlessClientError as error:
        if error.code == "program_applied_non_executable":
            # H1a: the preserved Program stays the authoring base so the
            # operator can edit and re-Apply it; it has no current binding.
            return _non_executable_applied_context(state)
        return None, {
            "code": error.code,
            "message": str(error),
            "restorationGates": [],
        }
    if not isinstance(applied, dict):
        return None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program projection",
            "restorationGates": [],
        }
    source = applied.get("source")
    program = applied.get("program")
    binding = applied.get("program_binding")
    program_id = applied.get("program_id")
    program_revision = applied.get("program_revision")
    record_digest = source.get("record_digest") if isinstance(source, dict) else None
    try:
        parsed_program = Program.from_dict(
            program,
            _active_catalog(state),
            path="<headless-applied-program>",
        )
        program_identity = parsed_program.program_revision
        validate_applied_program_selection(applied)
    except (
        ProgramBindingTransportError,
        ProgramValidationError,
        ActiveNodeEnvironmentProjectionError,
        HeadlessClientError,
        TypeError,
        ValueError,
        StrictArtifactError,
    ):
        program_identity = None
    if (
        not isinstance(program, dict)
        or program.get("program_id") != program_id
        or program_identity != program_revision
    ):
        return None, {
            "code": "program_applied_selection_invalid",
            "message": "headless API returned an invalid Applied Program projection",
            "restorationGates": [],
        }
    return (
        _AppliedContext(
            scenario_id=applied["scenario_id"],
            source_document_digest=applied["source_document_digest"],
            program_revision=program_revision,
            record_digest=record_digest,
            program_document=program,
            binding_document=binding,
            projection_document=applied,
        ),
        None,
    )


def _applied_empty_authoring(state: ComposerState) -> tuple[dict[str, Any], tuple[str, str, str]] | None:
    publication = state.published_scenario().get("published_scenario")
    if not isinstance(publication, dict) or publication.get("program_outcome") != {"kind": "no_program"}:
        return None
    captured = state.applied_activation_for_publication(publication)
    authoring = captured.get("program_authoring") if captured is not None else None
    if not isinstance(authoring, dict) or authoring.get("forest") != [] or authoring.get("scene"):
        return None
    derive_candidate(authoring, _active_catalog(state))
    return deepcopy(authoring), (publication["scenario_id"], publication["source_document_digest"], publication["retained_bundle_id"])


def _workspace(state: ComposerState) -> dict[str, Any]:
    context, reason = _applied_context(state)
    empty = _applied_empty_authoring(state) if reason is not None and reason["code"] == "program_not_selected" else None
    for _attempt in range(3):
        identity = state.authoring_workspace.identity()
        workspace = state.draft_session.workspace_snapshot(
            applied_program_revision=(
                context.program_revision if context is not None else None
            ),
            applied_unavailable_reason=reason,
            applied_authoring=empty[0] if empty is not None else None,
        )
        if state.authoring_workspace.identity() == identity:
            workspace["authoringWorkspace"] = identity.as_dict()
            return workspace
    raise HTTPException(
        status_code=503,
        detail={
            "code": "composer_workspace_snapshot_busy",
            "message": "the authoring workspace changed while it was being read",
        },
    )


def _program_write_roots(state: ComposerState) -> list[str]:
    """Use the explicit Program-write boundary, or preserve legacy callers."""

    if state.allowed_program_write_roots is not None:
        return state.allowed_program_write_roots
    return state.allowed_fs_roots


def _published_program_revision(state: ComposerState, target: str) -> str | None:
    """Return the content identity already published at one exact target.

    Save resolves a lost response by rereading the exact target and comparing
    this identity with the content captured for that request. A missing,
    unreadable or divergent target returns ``None``, so the ordinary overwrite
    rules still apply.
    """

    try:
        program, _path = load_program_artifact(
            target,
            allowed_roots=_program_write_roots(state),
            catalog=_active_catalog(state),
        )
    except (
        ProgramArtifactError,
        ActiveNodeEnvironmentProjectionError,
        HeadlessClientError,
        OSError,
    ):
        return None
    return program.program_revision


def _draft_node(document: dict[str, Any], instance_id: str) -> dict[str, Any]:
    pending = list(document.get("forest", []))
    while pending:
        node = pending.pop()
        if node.get("instance_id") == instance_id:
            return node
        children = node.get("children")
        if isinstance(children, list):
            pending.extend(children)
    raise DraftSessionError(
        "draft_node_not_found",
        f"Draft node does not exist: {instance_id}",
        status_code=404,
    )


def _draft_instance_ids(document: dict[str, Any]) -> set[str]:
    identifiers: set[str] = set()
    pending = list(document.get("forest", []))
    while pending:
        node = pending.pop()
        identity = node.get("instance_id")
        if isinstance(identity, str):
            identifiers.add(identity)
        children = node.get("children")
        if isinstance(children, list):
            pending.extend(children)
    return identifiers


def _observed_move_arguments(
    state: ComposerState,
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    if set(arguments) != {"instance_id", "observed_kind"}:
        raise DraftSessionError(
            "draft_arguments_invalid",
            "Save observed move requires exactly instance_id and observed_kind",
        )
    instance_id = arguments["instance_id"]
    observed_kind = arguments["observed_kind"]
    if type(instance_id) is not str or observed_kind not in {"joint", "tcp_pose"}:
        raise DraftSessionError(
            "draft_arguments_invalid", "Save observed move arguments are invalid"
        )
    # Every observed_move_* refusal in this resolver is raised before
    # mutate_draft prepares (prepare_mutation) or installs any Draft change and
    # before any authoring claim, so each one is a proven no-effect refusal by
    # code, whatever its HTTP status; the client releases its edit lock on it.
    if state.headless_client is None:
        raise DraftSessionError(
            "observed_move_unavailable",
            "Save observed move requires the current execution owner",
            status_code=503,
        )
    try:
        targets_projection = state.headless_client.execution_targets()
        control_projection = state.headless_client.execution_control_state()
    except HeadlessClientError as error:
        # A failed read of the robot pose is named, not passed through as a bare
        # owner error: nothing has been prepared yet, so it is a no-effect refusal.
        raise DraftSessionError(
            "observed_move_unavailable",
            f"current pose/joints could not be read: {error}",
            status_code=503,
            detail={"headless_code": error.code},
        ) from error
    targets = targets_projection.get("targets")
    control = control_projection.get("control_state")
    # execution_targets.v3 / execution_control_state.v6 (H1a-IF.2): motion
    # targets are component-owned rows; observations join by the full key.
    direct_rows = [
        {
            "controllable_component_id": component.get("controllable_component_id"),
            "command_target_id": target.get("command_target_id"),
            "operation_capability_id": target.get("operation_capability_id"),
            "target_role_id": target["command"].get("target_role_id"),
            "command": target["command"],
        }
        for component in (targets.get("components", []) if isinstance(targets, dict) else [])
        if isinstance(component, dict)
        for target in component.get("targets", [])
        if isinstance(target, dict)
        and target.get("presentation") == "motion"
        and isinstance(target.get("command"), dict)
    ]
    observed_rows = (
        [row for row in control.get("observed_targets", []) if isinstance(row, dict)]
        if isinstance(control, dict) and isinstance(control.get("observed_targets"), list)
        else []
    )
    authoring = state.draft_session.snapshot().get("authoring")
    if not isinstance(authoring, dict):
        raise DraftSessionError(
            "composer_draft_unavailable", "Composer has no Program Draft", status_code=404
        )
    node = _draft_node(authoring, instance_id)
    expected_node_id = (
        "motion.move_to_joint_target" if observed_kind == "joint" else "motion.move_to_pose"
    )
    if node.get("node_id") != expected_node_id:
        raise DraftSessionError(
            "observed_move_node_unsupported",
            ("current joints can only be captured into a Move to joint target node"
             if observed_kind == "joint" else
             "current robot pose can only be captured into a Move to pose node"),
            status_code=409,
        )
    # Every refusal below is raised before any Draft change is prepared. The
    # checks run from the Draft itself outward to the runtime owner, so each
    # refusal names the first thing the operator has to correct.
    parameters = node.get("parameters", {})
    if not isinstance(parameters, dict):
        parameters = {}
    node_target_role = parameters.get("target_role")
    if type(node_target_role) is not str:
        raise DraftSessionError(
            "observed_move_role_missing",
            "the selected move has no target role; choose the robot target role first",
            status_code=409,
        )
    frame_fields = (("base_frame_role", "base"),) if observed_kind == "tcp_pose" else ()
    for field, label in frame_fields:
        if type(parameters.get(field)) is not str:
            raise DraftSessionError(
                "observed_move_role_missing",
                f"the selected move has no {label} frame role; choose it before "
                "capturing the current robot pose",
                status_code=409,
            )
    tcp_endpoint = None
    if observed_kind == "tcp_pose":
        candidate_endpoint = (
            parameters.get("tcp_endpoint") if isinstance(parameters, dict) else None
        )
        if (
            not isinstance(candidate_endpoint, dict)
            or set(candidate_endpoint) != {"target_role", "frame_role"}
            or any(
                type(candidate_endpoint.get(field)) is not str
                or not candidate_endpoint[field]
                for field in ("target_role", "frame_role")
            )
        ):
            raise DraftSessionError(
                "observed_move_role_missing",
                "the selected move has no valid TCP endpoint; choose its target and frame "
                "roles before capturing the current robot pose",
                status_code=409,
            )
        tcp_endpoint = candidate_endpoint
    applied, unavailable = _applied_context(state)
    if applied is None and unavailable is not None:
        raise DraftSessionError(
            "observed_move_unavailable",
            f"current pose/joints could not be read: {unavailable['message']}",
            status_code=409,
        )
    if applied is None or not applied.binding_document:
        raise DraftSessionError(
            "observed_move_target_unbound",
            "no Program with robot target bindings is Applied; Apply the Program first",
            status_code=409,
        )
    identity = targets_projection.get("identity")
    if not isinstance(identity, dict):
        raise DraftSessionError(
            "observed_move_unavailable",
            "the execution owner publishes no configuration identity",
            status_code=409,
        )
    # The Applied binding is only meaningful for the configuration it was
    # compiled against, so check that before trusting any of its roles.
    if any(applied.binding_document.get(key) != identity.get(key)
           for key in ("graph_revision", "retained_bundle_id")):
        raise DraftSessionError(
            "observed_move_configuration_changed",
            "the Applied configuration changed: the Applied Program binding belongs to a "
            "different configuration than the current execution owner; Apply the Program again",
            status_code=409,
        )
    bound_targets = [row for row in applied.binding_document["target_role_bindings"]
                     if row["target_role_id"] == node_target_role]
    if len(bound_targets) != 1:
        raise DraftSessionError(
            "observed_move_target_unbound",
            f"target role {node_target_role!r} is not bound in the Applied Program",
            status_code=409,
        )
    bound = bound_targets[0]
    matching_direct = [
        row for row in direct_rows
        if row["controllable_component_id"] == bound["controllable_component_id"]
        and row["command_target_id"] == bound["command_target_id"]
        and row["operation_capability_id"] == "execution.trajectory.execute"
    ]
    if len(matching_direct) != 1:
        raise DraftSessionError(
            "observed_move_target_unavailable",
            ("the bound robot target is unavailable in the current execution owner"
             if not matching_direct else
             "the execution owner publishes more than one matching robot target"),
            status_code=409,
        )
    direct = matching_direct[0]
    frame_ref = None
    if observed_kind == "tcp_pose":
        command = direct["command"]
        base_row = next(
            (row for row in bound["frame_role_bindings"]
             if row["frame_role_id"] == parameters["base_frame_role"]),
            None,
        )
        if base_row is None:
            raise DraftSessionError(
                "observed_move_target_unbound",
                f"base frame role {parameters['base_frame_role']!r} is not bound for "
                f"target role {node_target_role!r} in the Applied Program",
                status_code=409,
            )
        assert tcp_endpoint is not None
        endpoint_targets = [
            row for row in applied.binding_document["target_role_bindings"]
            if row["target_role_id"] == tcp_endpoint["target_role"]
        ]
        if len(endpoint_targets) != 1:
            raise DraftSessionError(
                "observed_move_target_unbound",
                f"TCP endpoint target role {tcp_endpoint['target_role']!r} is not bound in "
                "the Applied Program",
                status_code=409,
            )
        endpoint_target = endpoint_targets[0]
        if endpoint_target["controllable_component_id"] != bound["controllable_component_id"]:
            raise DraftSessionError(
                "observed_move_target_unbound",
                f"TCP endpoint target role {tcp_endpoint['target_role']!r} does not share "
                f"motion target role {node_target_role!r}'s component",
                status_code=409,
            )
        tcp_frame_row = next(
            (row for row in endpoint_target["frame_role_bindings"]
             if row["frame_role_id"] == tcp_endpoint["frame_role"]),
            None,
        )
        if tcp_frame_row is None:
            raise DraftSessionError(
                "observed_move_target_unbound",
                f"TCP endpoint target role {tcp_endpoint['target_role']!r} frame role "
                f"{tcp_endpoint['frame_role']!r} is not bound in the Applied Program",
                status_code=409,
            )
        descriptor_frames = command["target_descriptor"]["frames"]
        direct_base_role = command.get("base_frame_role_id")
        direct_base_row = next(
            (row for row in descriptor_frames
             if row["frame_role_id"] == direct_base_role),
            None,
        )
        requested_base = base_row.get("compiled_frame_name")
        observed_base = (
            direct_base_row.get("compiled_frame_name")
            if isinstance(direct_base_row, dict) else None
        )
        if requested_base is None or requested_base != observed_base:
            raise DraftSessionError(
                "observed_move_frame_mismatch",
                (f"base frame role {parameters['base_frame_role']!r} resolves to "
                 f"{requested_base!r}, but the robot reports its current pose with base "
                 f"frame {observed_base!r}; choose the robot's base frame role to capture "
                 "its current pose"
                 if observed_base is not None else
                 "the robot target does not report a base frame for its current pose"),
                status_code=409,
            )
        direct_tcp_role = command.get("tcp_frame_role_id")
        direct_tcp_row = next(
            (row for row in descriptor_frames
             if row["frame_role_id"] == direct_tcp_role),
            None,
        )
        observed_frame = (
            direct_tcp_row.get("compiled_frame_name")
            if isinstance(direct_tcp_row, dict) else None
        )
        requested = tcp_frame_row.get("compiled_frame_name")
        if requested is None or requested != observed_frame:
            message = (
                f"TCP endpoint target role {tcp_endpoint['target_role']!r} frame role "
                f"{tcp_endpoint['frame_role']!r} resolves to {requested!r}, but the robot "
                f"reports its current pose with TCP frame {observed_frame!r}; current-pose "
                "capture requires the selected endpoint to match the robot's current TCP"
                if observed_frame is not None else
                f"the robot target does not report its current TCP frame for endpoint target "
                f"role {tcp_endpoint['target_role']!r} frame role "
                f"{tcp_endpoint['frame_role']!r}"
            )
            raise DraftSessionError(
                "observed_move_frame_mismatch",
                message,
                status_code=409,
            )
        model_identity = (
            tcp_frame_row.get("model_instance_id"),
            tcp_frame_row.get("model_revision"),
        )
        observed_model_identity = (
            direct_tcp_row.get("model_instance_id")
            if isinstance(direct_tcp_row, dict) else None,
            direct_tcp_row.get("model_revision")
            if isinstance(direct_tcp_row, dict) else None,
        )
        if (
            any(type(value) is not str or not value for value in model_identity)
            or model_identity != observed_model_identity
        ):
            raise DraftSessionError(
                "observed_move_frame_mismatch",
                f"TCP endpoint target role {tcp_endpoint['target_role']!r} frame role "
                f"{tcp_endpoint['frame_role']!r} and the robot's current TCP do not share "
                "the same retained model instance and revision",
                status_code=409,
            )
        frame_ref = {"kind": "target_frame", "target_role": node_target_role,
                     "frame_role": parameters["base_frame_role"]}
    matching_observed = [
        row.get("observed") for row in observed_rows
        if row.get("controllable_component_id") == direct.get("controllable_component_id")
        and row.get("command_target_id") == direct.get("command_target_id")
        and row.get("operation_capability_id") == direct.get("operation_capability_id")
        and isinstance(row.get("observed"), dict)
        and row["observed"].get("kind") == "joint_positions"
    ]
    observed = matching_observed[0] if len(matching_observed) == 1 else None
    if (
        targets_projection.get("ok") is not True
        or control_projection.get("ok") is not True
        or targets_projection.get("identity") != control_projection.get("identity")
        or not isinstance(direct, dict)
        or not isinstance(observed, dict)
        or observed.get("complete") is not True
        or type(observed.get("source_epoch")) is not str
        or type(observed.get("sequence")) is not int
        or type(direct.get("target_role_id")) is not str
        or type(direct.get("controllable_component_id")) is not str
        or type(direct.get("command_target_id")) is not str
    ):
        raise DraftSessionError(
            "observed_move_unavailable",
            "execution owner has no complete current observed target",
            status_code=409,
        )
    resolved_observation = (
        {
            "source_joint_names": deepcopy(observed.get("source_joint_names")),
            "positions_rad": deepcopy(observed.get("positions_rad")),
        }
        if observed_kind == "joint"
        else {"current_tcp_pose": deepcopy(observed.get("current_tcp_pose")),
              "frame_ref": frame_ref}
    )
    if observed_kind == "joint":
        names = resolved_observation["source_joint_names"]
        positions = resolved_observation["positions_rad"]
        if (
            type(names) is not list
            or not names
            or any(type(name) is not str or not name for name in names)
            or len(names) != len(set(names))
            or type(positions) is not list
            or len(positions) != len(names)
            or any(
                type(position) not in {int, float}
                or not math.isfinite(float(position))
                for position in positions
            )
        ):
            raise DraftSessionError(
                "observed_move_unavailable",
                "execution owner returned an invalid observed joint target",
                status_code=502,
            )
    else:
        pose = resolved_observation["current_tcp_pose"]
        position = pose.get("position_m") if isinstance(pose, dict) else None
        orientation = pose.get("orientation") if isinstance(pose, dict) else None
        values = (
            [position.get(field) for field in ("x_m", "y_m", "z_m")]
            + [orientation.get(field) for field in ("x", "y", "z", "w")]
            if isinstance(position, dict) and isinstance(orientation, dict)
            else []
        )
        if (
            len(values) != 7
            or any(
                type(value) not in {int, float}
                or not math.isfinite(float(value))
                for value in values
            )
            or math.hypot(*[float(value) for value in values[3:]]) <= 1e-12
        ):
            raise DraftSessionError(
                "observed_move_unavailable",
                "execution owner returned an invalid observed TCP target",
                status_code=502,
            )
    source_identity = {
        "source_epoch": observed["source_epoch"],
        "sequence": observed["sequence"],
        "target_role_id": direct["target_role_id"],
        "controllable_component_id": direct["controllable_component_id"],
        "command_target_id": direct["command_target_id"],
    }
    return (
        {
            "instance_id": instance_id,
            "expected_node_id": expected_node_id,
            "observed_kind": observed_kind,
            "observed": resolved_observation,
        },
        source_identity,
    )


def _program_shape(shape: dict[str, Any]) -> dict[str, Any]:
    def positive(value: Any) -> float:
        if (
            type(value) not in {int, float}
            or not math.isfinite(float(value))
            or float(value) <= 0.0
        ):
            raise DraftSessionError(
                "observed_scene_shape_unsupported",
                "observed object shape is not finite and positive",
                status_code=409,
            )
        return float(value)

    kind = shape.get("type")
    if kind == "box" and isinstance(shape.get("dimensions_m"), dict):
        dimensions = shape["dimensions_m"]
        return {
            "type": "box",
            "box_dimensions_m": [
                positive(dimensions.get(axis)) for axis in ("x", "y", "z")
            ],
        }
    if kind == "sphere":
        return {"type": "sphere", "sphere_radius_m": positive(shape.get("radius_m"))}
    if kind == "cylinder":
        return {
            "type": "cylinder",
            "cylinder_radius_m": positive(shape.get("radius_m")),
            "cylinder_height_m": positive(shape.get("height_m")),
        }
    if kind == "mesh" and isinstance(shape.get("scale"), dict):
        scale = shape["scale"]
        resource_uri = shape.get("resource_uri")
        if type(resource_uri) is not str or not resource_uri:
            raise DraftSessionError(
                "observed_scene_shape_unsupported",
                "observed mesh has no portable resource URI",
                status_code=409,
            )
        return {
            "type": "mesh",
            "mesh_resource_uri": resource_uri,
            "mesh_scale": [positive(scale.get(axis)) for axis in ("x", "y", "z")],
        }
    raise DraftSessionError(
        "observed_scene_shape_unsupported",
        "observed object has no single supported portable collision shape",
        status_code=409,
    )


def _observed_scene_arguments(
    state: ComposerState,
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    if set(arguments) != {"scene_resource_id"} or type(arguments["scene_resource_id"]) is not str:
        raise DraftSessionError(
            "draft_arguments_invalid",
            "Save observed object requires exactly one scene_resource_id",
        )
    if state.headless_client is None:
        raise DraftSessionError(
            "observed_scene_unavailable",
            "Save observed object requires the current logical-scene owner",
            status_code=503,
        )
    try:
        projection = state.headless_client.scene()
    except HeadlessClientError as error:
        # Nothing has been prepared yet, so a failed read is a named no-effect
        # refusal, like every other refusal of this resolver.
        raise DraftSessionError(
            "observed_scene_unavailable",
            f"the current logical scene could not be read: {error}",
            status_code=503,
            detail={"headless_code": error.code},
        ) from error
    logical = projection.get("logical_scene")
    access = logical.get("access") if isinstance(logical, dict) else None
    snapshot = logical.get("snapshot") if isinstance(logical, dict) else None
    active = access.get("active_projection") if isinstance(access, dict) else None
    objects = snapshot.get("objects") if isinstance(snapshot, dict) else None
    object_row = next(
        (
            row
            for row in objects
            if isinstance(row, dict)
            and row.get("scene_resource_id") == arguments["scene_resource_id"]
        ),
        None,
    ) if isinstance(objects, list) else None
    placement = object_row.get("placement") if isinstance(object_row, dict) else None
    shapes = object_row.get("collision_shapes") if isinstance(object_row, dict) else None
    pose = placement.get("pose_in_cell") if isinstance(placement, dict) else None
    if (
        not isinstance(active, dict)
        or not isinstance(snapshot, dict)
        or not isinstance(object_row, dict)
        or not isinstance(placement, dict)
        or placement.get("kind") != "cell_fixed"
        or not isinstance(pose, dict)
        or not isinstance(shapes, list)
        or len(shapes) != 1
        or not isinstance(shapes[0], dict)
    ):
        raise DraftSessionError(
            "observed_scene_object_ineligible",
            "only one current cell-fixed object with one portable shape can be saved",
            status_code=409,
        )
    position = pose.get("position_m")
    orientation = pose.get("orientation_xyzw")
    pose_values = (
        [position.get(axis) for axis in ("x", "y", "z")] + orientation
        if isinstance(position, dict)
        and isinstance(orientation, list)
        and len(orientation) == 4
        else []
    )
    if (
        len(pose_values) != 7
        or any(
            type(value) not in {int, float}
            or not math.isfinite(float(value))
            for value in pose_values
        )
        or math.hypot(*[float(value) for value in pose_values[3:]]) <= 1e-12
    ):
        raise DraftSessionError(
            "observed_scene_object_ineligible",
            "observed object pose is incomplete",
            status_code=409,
        )
    resource = {
        "scene_resource_id": arguments["scene_resource_id"],
        "shape": _program_shape(shapes[0]),
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [float(position[axis]) for axis in ("x", "y", "z")],
            "orientation": {"xyzw": [float(value) for value in orientation]},
        },
    }
    authoring = state.draft_session.snapshot().get("authoring")
    scene = authoring.get("scene") if isinstance(authoring, dict) else None
    existing = next(
        (
            row
            for row in scene
            if isinstance(row, dict)
            and row.get("scene_resource_id") == arguments["scene_resource_id"]
        ),
        None,
    ) if isinstance(scene, list) else None
    if isinstance(existing, dict):
        for field in ("resource_roles", "metadata"):
            if field in existing:
                resource[field] = deepcopy(existing[field])
    source_identity = {
        "active_projection": deepcopy(active),
        "transaction_sequence": snapshot.get("transaction_sequence"),
        "scene_resource_id": arguments["scene_resource_id"],
    }
    return {"scene_resource": resource}, source_identity


def _source_resolved_mutation(
    state: ComposerState,
    mutation: str,
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]] | None:
    if mutation == "program_draft_store_observed_move":
        return _observed_move_arguments(state, arguments)
    if mutation == "program_draft_store_observed_scene_resource":
        return _observed_scene_arguments(state, arguments)
    return None


def prepare_program_discard(
    state: ComposerState, draft_id: str, expected_revision: int
) -> tuple[Callable[[], None], Callable[[], bool]]:
    """Prepare returning the Program Draft to the exact Applied Program.

    Returns the under-commit validation and the no-I/O install, so the
    Program Discard and the Scenario Discard commit it the same way.
    """

    def read_applied() -> tuple[Program | dict[str, Any], tuple[str, str, str]]:
        context, reason = _applied_context(state)
        if reason is not None and reason["code"] == "program_not_selected":
            empty = _applied_empty_authoring(state)
            if empty is not None:
                return empty
        if reason is not None or context is None:
            raise DraftSessionError(
                (
                    reason["code"]
                    if reason
                    else "program_applied_selection_invalid"
                ),
                reason["message"] if reason else "Applied Program is absent",
                status_code=(
                    404
                    if reason
                    and reason["code"]
                    == "program_applied_selection_not_found"
                    else 502
                    if reason
                    and reason["code"]
                    == "program_applied_selection_invalid"
                    else 503
                ),
            )
        try:
            candidate = Program.from_dict(
                context.program_document,
                _active_catalog(state),
                path="<composer-program-discard>",
            )
        except ProgramValidationError as error:
            raise DraftSessionError(
                "composer_applied_program_validation_failed",
                str(error),
                status_code=409,
                detail=str(error),
            ) from error
        if candidate.program_revision != context.program_revision:
            raise DraftSessionError(
                "program_applied_selection_invalid",
                "headless Applied Program content is divergent",
                status_code=502,
            )
        return candidate, (
            context.scenario_id,
            context.program_revision,
            context.record_digest,
        )

    applied_program, applied_identity = read_applied()
    prepared = state.draft_session.prepare_discard_replacement(
        applied_program, _active_catalog(state)
    )

    def validate_applied_unchanged() -> None:
        _program, current_identity = read_applied()
        if current_identity != applied_identity:
            raise DraftSessionError(
                "program_applied_changed",
                "Applied Program changed before Discard committed",
                status_code=409,
            )

    return validate_applied_unchanged, lambda: state.draft_session.install_prepared_replacement(
        prepared,
        expected_draft_id=draft_id,
        expected_revision=expected_revision,
    )


def discard_program_draft(state: ComposerState, body: Any) -> None:
    """Return the Program Draft to Applied (Draft-only, no Manager effect)."""

    validate, install = prepare_program_discard(
        state, body.draft_id, body.expected_draft_revision
    )
    commit_draft_edit(
        state,
        body,
        operation_kind="discard_draft",
        validate=validate,
        install=install,
    )


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose revision-fenced authoring and thin authoritative Apply/Load."""

    router = APIRouter()

    def prepare_program_activation(
        body: _DraftFence,
        program_document: dict[str, Any] | None,
        *,
        correlation_id: str | None = None,
        require_deployment: Callable[[dict[str, Any]], None] | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Prepare one complete immutable activation for a Program change.

        ``require_deployment`` checks the Applied Deployment this activation
        carries before any Manager read.
        """

        if state.headless_client is None:
            raise DraftSessionError(
                "program_apply_unavailable",
                "Program activation requires the configured Manager",
                status_code=503,
            )
        publication = state.published_scenario().get("published_scenario")
        context = (
            state.applied_activation_for_publication(publication)
            if isinstance(publication, dict)
            else None
        )
        deployment = context.get("deployment") if context is not None else None
        assignment = context.get("program_role_assignment") if context is not None else None
        source_digest = context.get("source_document_digest") if context is not None else None
        scenario_id = context.get("scenario_id") if context is not None else None
        project_path = context.get("project_path") if context is not None else None
        # The Applied Scenario project folder; Program Apply keeps it.
        scenario_project_path = (
            context.get("scenario_project_path") if context is not None else None
        )
        if (
            not isinstance(deployment, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_digest, str)
            or not isinstance(scenario_id, str)
            or not isinstance(project_path, str)
            or not isinstance(scenario_project_path, str)
        ):
            raise DraftSessionError(
                "activation_input_incomplete",
                "Program Apply requires captured authoring input for the active Deployment",
                status_code=409,
            )
        assert context is not None
        if require_deployment is not None:
            require_deployment(deployment)
        source_closure = capture_deployment_model_sources(
            deployment, project_root=Path(project_path)
        )
        if source_closure != context["model_source_closure"]:
            raise DraftSessionError(
                "activation_model_source_changed",
                "An active Deployment model source changed since Apply",
                status_code=409,
            )
        if program_document is None:
            assignment = {"component_roles": [], "target_roles": []}
        captured_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "scenario_id": scenario_id,
                    "project_path": scenario_project_path,
                    "source_document_digest": source_digest,
                    "deployment": deployment,
                    "model_source_closure": source_closure,
                    "program": program_document,
                    "program_role_assignment": assignment,
                }
            )
        )
        bundle, graph, _manager_epoch = state._manager_current(
            state.headless_client.manager_status()
        )
        intent = {
            "scene_initialization": "preserve",
            "scenario_id": scenario_id,
            "project_path": scenario_project_path,
            "source_document_digest": source_digest,
            "captured_input_digest": captured_digest,
            "deployment": deepcopy(deployment),
            "model_source_closure": source_closure,
            "program": deepcopy(program_document),
            "program_role_assignment": assignment,
            "expected_manager_current": {
                "retained_bundle_id": bundle,
                "graph_revision": graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "manager_request_id": "program-activation-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32],
            # Preserve a correction's shared reference; otherwise the request
            # is its own correlation.
            "correlation_id": correlation_id or body.request_id,
            "confirmation_token": None,
            "draft_id": body.draft_id,
            "draft_revision": body.expected_draft_revision,
        }
        prepared = state.execute_authored_deployment_candidate(
            intent, None, apply=True, prepare_only=True
        )
        if prepared.get("confirmation_required") is True:
            raise DraftSessionError(
                "program_apply_confirmation_required",
                "Program Apply unexpectedly changed Deployment structure",
                status_code=409,
            )
        return intent, prepared

    @router.get("/api/program/draft")
    def read_draft() -> dict[str, Any]:
        return state.draft_session.snapshot()

    @router.post("/api/program/validate")
    def validate_program_source(body: ValidateProgramSourceRequest) -> dict[str, Any]:
        # Pure derivation with the same build-qualified authoring catalog.
        # No Draft replacement, Apply, file write, lease, or runtime read.
        try:
            candidate, diagnostics = derive_candidate(body.authoring, state.node_catalog)
        except AuthoringForestError as error:
            raise HTTPException(status_code=422, detail={
                "code": error.code, "message": str(error), "context": error.detail,
            }) from error
        return {
            "valid": candidate is not None and not diagnostics,
            "diagnostics": diagnostics,
            "candidate": candidate.to_dict() if candidate is not None else None,
        }

    @router.get("/api/program/workspace")
    def read_workspace() -> dict[str, Any]:
        return _workspace(state)

    @router.post("/api/program/draft/expression-check")
    def check_expression(body: ExpressionCheckRequest) -> dict[str, Any]:
        # Pure check against the current Draft's declarations; no Draft change.
        draft = state.draft_session.snapshot()
        if not draft["program_loaded"]:
            raise HTTPException(status_code=404, detail={
                "code": "composer_draft_unavailable",
                "message": "Composer has no loaded Program Draft",
            })
        try:
            return check_draft_expression(
                draft["authoring"],
                state.node_catalog,
                expression=body.expression,
                destination_type=body.destination_type,
                exclude_computed_key=body.exclude_computed_key,
            )
        except ProgramValidationError as error:
            raise HTTPException(status_code=409, detail={
                "code": "expression_environment_invalid",
                "message": (
                    "the Draft's parameter or key declarations are invalid: "
                    f"{error}"
                ),
            }) from error
        except ExpressionCheckRefusal as error:
            raise HTTPException(status_code=422, detail={
                "code": error.code, "message": str(error),
            }) from error

    @router.post("/api/program/draft/mutations")
    def mutate_draft(body: DraftMutationRequest) -> dict[str, Any]:
        if body.mutation not in _AUTHORING_MUTATIONS:
            raise HTTPException(
                status_code=404,
                detail={
                    "code": "composer_authoring_mutation_unsupported",
                    "message": (
                        f"unsupported Program authoring mutation: {body.mutation}"
                    ),
                },
            )
        try:
            arguments = body.arguments
            if body.mutation in _SOURCE_RESOLVED_MUTATIONS:
                resolved = _source_resolved_mutation(
                    state, body.mutation, body.arguments
                )
                if resolved is None:
                    raise RuntimeError("source-resolved mutation has no resolver")
                arguments, _source_identity = resolved
            # Prepare the edit and its derivation outside every owner lock.
            prepared = state.draft_session.prepare_mutation(
                tool_id=body.mutation,
                arguments=deepcopy(arguments),
                expected_revision=body.expected_draft_revision,
                # Draft validation uses the immutable build-qualified catalog.
                # Owner truth is rechecked only at consequential Save/Apply
                # boundaries, not through one headless round-trip per edit.
                catalog=state.node_catalog,
                draft_id=body.draft_id,
            )
            commit = commit_draft_edit(
                state,
                body,
                operation_kind="mutate_draft",
                install=lambda: state.draft_session.install_prepared_mutation(
                    prepared
                ),
            )
        except Exception as error:
            _raise_workspace_error(error)
        return {
            "mutation": prepared.tool_id,
            "details": thaw_json(prepared.details),
            "workspace": _workspace(state),
            "authoringWorkspace": commit.snapshot.as_dict(),
        }

    @router.post("/api/program/open-draft")
    def open_draft(body: OpenDraftRequest) -> dict[str, Any]:
        try:
            state.draft_session.require_replace_allowed(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
            )
            program, source_path = load_program_artifact(
                body.path,
                allowed_roots=state.allowed_fs_roots,
                catalog=_active_catalog(state),
            )
            prepared = state.draft_session.prepare_open_replacement(
                program, source_path=source_path
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="open_draft",
                session="whole-workspace",
                install=lambda: state.draft_session.install_prepared_replacement(
                    prepared,
                    expected_draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Program workspace carries its own Draft-matched identity; the
            # composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/new-draft")
    def new_draft(body: NewDraftRequest) -> dict[str, Any]:
        try:
            def applied_revision() -> str | None:
                context, _reason = _applied_context(state)
                return context.program_revision if context is not None else None

            state.draft_session.require_replace_allowed(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                discard_changes=body.confirm_discard,
            )
            current = state.draft_session.snapshot()
            if (
                current["program_loaded"]
                and not body.confirm_discard
                and (applied := applied_revision()) is not None
                and (
                    current["program_revision"] is None
                    or current["program_revision"] != applied
                )
            ):
                raise DraftSessionError(
                    "composer_draft_discard_confirmation_required",
                    "creating a new Program would discard unapplied Draft changes",
                    status_code=409,
                )
            prepared = state.draft_session.prepare_new_replacement(
                _active_catalog(state)
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="new_draft",
                session="whole-workspace",
                install=lambda: state.draft_session.install_prepared_replacement(
                    prepared,
                    expected_draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Program workspace carries its own Draft-matched identity; the
            # composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    def program_save(
        body: SaveDraftRequest | SaveDraftAsRequest,
        *,
        target_path: str | None,
        confirm_overwrite: bool,
    ) -> dict[str, Any]:
        """Persist one exact Program Draft and adopt it as the Saved baseline.

        The capture, the write and the baseline adoption are three separate
        steps: the write happens only after admission, under the local
        authoring claim, so an admission refusal writes nothing; the adoption
        is one composite commit fenced by the captured workspace identity. A
        stale adoption keeps the correctly written file and never labels newer
        in-memory content Saved. One effect record decides both the HTTP
        outcome and the claim settlement: Not saved, Saved but not adopted
        (naming the written path), or unverified.
        """

        effects = OperationEffects()
        try:
            _program_save(
                body,
                target_path=target_path,
                confirm_overwrite=confirm_overwrite,
                effects=effects,
            )
        except Exception as error:
            raise_save_failure(
                error,
                effects,
                _raise_workspace_error,
                code="program_saved_baseline_not_adopted",
                document="Program Draft",
            )
        # The Program workspace carries its own Draft-matched identity; the
        # composite snapshot must not replace it (it breaks that contract).
        return _workspace(state)

    def _program_save(
        body: SaveDraftRequest | SaveDraftAsRequest,
        *,
        target_path: str | None,
        confirm_overwrite: bool,
        effects: OperationEffects,
    ) -> None:
        prepared = state.draft_session.prepare_save(
            draft_id=body.draft_id,
            expected_revision=body.expected_draft_revision,
            target_path=target_path,
            confirm_overwrite=confirm_overwrite,
        )
        captured_workspace = state.authoring_workspace.snapshot().as_dict()
        project_path = captured_workspace["project"]["document_path"]
        scenario_member = (
            target_path is None and project_path is not None
            and prepared.resolved_target == str(
                Path(project_path) / SCENARIO_PROJECT_MEMBERS["program_draft"]
            )
        )
        saved_path = prepared.resolved_target

        def write_artifact_under_claim() -> None:
            nonlocal saved_path
            # The adoption's fences, checked before the write: a stale Save is
            # refused before any effect.
            body.require_workspace_fence(state.authoring_workspace.identity())
            state.draft_session.require_current(
                draft_id=prepared.draft_id, expected_revision=prepared.base_revision
            )
            # A prior completed write of this exact revision is resolved
            # without repeating it.
            revision = prepared.program.program_revision
            if _published_program_revision(state, prepared.target_path) == revision:
                return
            try:
                saved_path = save_program_artifact(
                    prepared.program,
                    prepared.target_path,
                    allowed_roots=_program_write_roots(state),
                    overwrite=prepared.overwrite,
                )
            except Exception:
                # The write is one atomic replacement: the target holds either
                # its previous content (Not saved) or exactly this revision.
                if _published_program_revision(
                    state, prepared.target_path
                ) == revision:
                    effects.destination_published(saved_path)
                raise
            effects.destination_published(saved_path)

        def prepare_saved_project() -> dict[str, Any]:
            document, path, digest = save_scenario_member(
                project_path, "program_draft", captured_workspace["program"]["authoring"],
                allowed_roots=state.allowed_fs_roots, effects=effects,
            )
            return scenario_project_facts(
                document, document_path=path, source_document_digest=digest
            )

        # Both forms are written under the claim, after admission: a Scenario
        # member with its manifest, a standalone artifact as the under-claim
        # step before the commit.
        commit_local_authoring(
            state,
            body,
            operation_kind=prepared.tool_id,
            session="whole-workspace" if scenario_member else "program",
            validate_under_claim=(
                None if scenario_member else write_artifact_under_claim
            ),
            prepare_project_under_claim=(
                prepare_saved_project if scenario_member else None
            ),
            install=lambda: state.draft_session.install_prepared_save(
                prepared, saved_path=saved_path
            ),
            effects=effects,
        )

    @router.post("/api/program/save")
    def save_draft(body: SaveDraftRequest) -> dict[str, Any]:
        try:
            return program_save(
                body,
                target_path=None,
                confirm_overwrite=True,
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/save-as")
    def save_draft_as(body: SaveDraftAsRequest) -> dict[str, Any]:
        try:
            return program_save(
                body,
                target_path=normalize_program_save_path(body.path),
                confirm_overwrite=body.confirm_overwrite,
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/apply")
    def apply_draft(body: ApplyDraftRequest) -> JSONResponse:
        try:
            retained = state.draft_session.retained_state()
            document = retained.get("program")
            if not isinstance(document, dict):
                raise DraftSessionError(
                    "composer_authoring_draft_incomplete",
                    "Composer Draft has no strict Program candidate",
                    status_code=409,
                )
            program = Program.from_dict(
                document, _active_catalog(state), path="<composer-program-apply>"
            )
            # Nothing to apply: refused before preparation or admission, so no
            # effect is possible. An Applied Program that cannot be read now
            # never proves a no-op.
            applied_context, _unavailable = _applied_context(state)
            if (
                applied_context is not None
                and applied_context.program_revision == program.program_revision
            ):
                raise DraftSessionError(
                    "program_apply_nothing_to_apply",
                    "Nothing to apply: the Program Draft already matches Applied",
                    status_code=409,
                )
            intent, prepared = prepare_program_activation(
                body, program.to_dict(), correlation_id=body.correlation_id
            )
            coordinator = state.authoring_admission
            if coordinator is None:
                raise DraftSessionError(
                    "authoring_not_ready",
                    "authoritative authoring admission is unavailable",
                    status_code=503,
                )
            identity = state.authoring_workspace.identity()
            body.require_workspace_fence(identity)

            def validate_submission(_claim: dict[str, Any]) -> None:
                body.require_workspace_fence(state.authoring_workspace.identity())
                state.draft_session.require_current(
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
                if state.draft_session.retained_state() != retained:
                    raise DraftSessionError(
                        "composer_draft_stale",
                        "Program Draft changed before activation submission",
                        status_code=409,
                    )

            result = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind="apply_program",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                validate_under_claim=validate_submission,
                submit=lambda claim: state.execute_authored_deployment_candidate(
                    intent, claim, apply=True, prepared=prepared
                ),
            )
            if body.correlation_id is not None:
                log_reset_apply_link(
                    "program_apply",
                    correlation_id=body.correlation_id,
                    request_id=body.request_id,
                    outcome=str(result.get("status")),
                    apply_id=result.get("apply_id"),
                    apply_generation=result.get("apply_generation"),
                    manager_request_id=intent["manager_request_id"],
                    draft_id=body.draft_id,
                    draft_revision=body.expected_draft_revision,
                )
            return JSONResponse(
                status_code=202 if result["status"] == "accepted" else 200,
                content={
                    "status": result["status"],
                    "headless": result,
                    "workspace": _workspace(state),
                },
            )
        except Exception as error:
            if body.correlation_id is not None:
                log_reset_apply_link(
                    "program_apply",
                    correlation_id=body.correlation_id,
                    request_id=body.request_id,
                    outcome="error",
                    error_code=_error_code(error),
                    draft_id=body.draft_id,
                    draft_revision=body.expected_draft_revision,
                )
            _raise_workspace_error(error)

    @router.post("/api/program/discard")
    def discard_draft(body: DiscardDraftRequest) -> dict[str, Any]:
        try:
            discard_program_draft(state, body)
            # The Program workspace carries its own Draft-matched identity; the
            # composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/revert", status_code=202)
    def revert_draft(body: RevertDraftRequest) -> JSONResponse:
        try:
            retained = state.draft_session.retained_state()
            restore = saved_program_restore(state, retained, _active_catalog(state))
            saved_document = restore.saved_document
            saved_program = restore.saved_program
            # Revert exists only while Saved differs from Applied: refused
            # before preparation or admission, so no effect is possible. An
            # Applied Program that cannot be read now never proves a no-op.
            applied_context, _unavailable = _applied_context(state)
            if (
                saved_program is not None
                and applied_context is not None
                and applied_context.program_revision == saved_program.program_revision
            ):
                raise DraftSessionError(
                    "program_revert_nothing_to_revert",
                    "Nothing to revert: the Saved Program already matches Applied",
                    status_code=409,
                )
            def require_saved_deployment(deployment: dict[str, Any]) -> None:
                # A Program-only Revert keeps the Applied Deployment, so it can
                # restore the Scenario only while the Saved Deployment is that
                # one: checked on the activation's own Deployment, before the
                # Manager preview.
                if saved_deployment_differs_from(state, deployment):
                    raise DraftSessionError(
                        "scenario_revert_requires_deployment",
                        "The Saved Deployment differs from Applied; Revert the whole "
                        "Scenario through the Deployment Revert",
                        status_code=409,
                    )

            intent, prepared = prepare_program_activation(
                body,
                saved_program.to_dict() if saved_program is not None else None,
                require_deployment=require_saved_deployment,
            )
            if saved_program is None:
                intent["program_authoring"] = deepcopy(saved_document)
            replacement = restore.replacement
            # Revert restores the whole Scenario: a Deployment Draft that left
            # Saved (which matches Applied here) returns to it in the same commit.
            deployment_restore = saved_deployment_restore(state)
            coordinator = state.authoring_admission
            if coordinator is None:
                raise DraftSessionError(
                    "authoring_not_ready",
                    "authoritative authoring admission is unavailable",
                    status_code=503,
                )
            identity = state.authoring_workspace.identity()
            body.require_workspace_fence(identity)

            def validate_submission(_claim: dict[str, Any]) -> None:
                body.require_workspace_fence(state.authoring_workspace.identity())
                if state.draft_session.retained_state() != retained:
                    raise DraftSessionError(
                        "composer_draft_stale",
                        "Program Draft changed before Revert submission",
                        status_code=409,
                    )

            def commit_saved() -> None:
                # One whole-workspace commit: both Drafts return to Saved
                # together, or the workspace is contained.
                def install() -> bool:
                    changed = state.draft_session.install_prepared_replacement(
                        replacement,
                        expected_draft_id=body.draft_id,
                        expected_revision=body.expected_draft_revision,
                    )
                    if deployment_restore is not None:
                        changed = deployment_restore(identity) or changed
                    return changed

                state.authoring_workspace.replace(
                    expected_identity=identity,
                    install=install,
                    notify=False,
                    context=AuthoringOperationContext(
                        operation="revert_program",
                        request_owner_epoch=body.request_owner_epoch,
                        request_id=body.request_id,
                    ),
                )

            result = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind="revert_program",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                validate_under_claim=validate_submission,
                commit_before_submit=commit_saved,
                workspace=state.authoring_workspace,
                submit=lambda claim: state.execute_authored_deployment_candidate(
                    intent, claim, apply=True, prepared=prepared
                ),
                after_release=state.authoring_workspace.publish_commit_wake,
            )
            return JSONResponse(
                status_code=202 if result["status"] == "accepted" else 200,
                content={
                    "status": result["status"],
                    "headless": result,
                    "workspace": _workspace(state),
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    return router


__all__ = ["create_draft_router"]
