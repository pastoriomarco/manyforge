"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from contextvars import ContextVar
from functools import wraps
import hashlib
import math
from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    StrictArtifactError,
)
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError
from manyforge_headless_api.program_binding_projection import (
    validate_applied_program_selection,
    validate_program_apply_result,
)
from pydantic import BaseModel, ConfigDict, Field

from .draft_session import (
    DraftSessionError,
    OPERATOR_DRAFT_MUTATIONS,
    draft_applicability,
)
from .authoring_forest import AuthoringForestError, derive_candidate, from_program
from .authoring_identity import thaw_json
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .source_model_client import ActiveSourceModelProjectionError
from .program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    normalize_program_save_path,
    save_program_artifact,
)
from .state import ComposerState
from .authoring_admission import (
    AuthoringMutationEnvelope,
    headless_client_http_exception,
)
from .authoring_workspace import (
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
)
from .authoring_workspace_fence import (
    commit_local_authoring,
    require_captured_workspace_identity,
)
from .assistant_mode_client import (
    ActiveAssistantModeProjectionError,
    read_active_assistant_context,
)
from .assistant_provider_client import AssistantProviderClientError
from .assistant_schema_validation import (
    CallbackSchemaValidationError,
    validate_callback_arguments,
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
        "assistant_draft_materialize_proposal",
        "program_draft_store_observed_move",
        "program_draft_store_observed_scene_resource",
    }
)
_DTO_CONFIG = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_ACTIVE_AUTHORING_CLAIM: ContextVar[dict[str, Any] | None] = ContextVar(
    "active_authoring_claim", default=None
)
_ACTIVE_AUTHORING_EFFECT: ContextVar[dict[str, Any] | None] = ContextVar(
    "active_authoring_effect", default=None
)


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

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            request_owner_epoch=self.request_owner_epoch,
            request_id=self.request_id,
        )


class _LocalDraftFence(_DraftFence):
    """Migrated ordinary-edit fence: the Program Draft plus the workspace.

    Program Apply, Revert and Scenario Load stay on the external-operation path
    and keep the plain :class:`_DraftFence` shape.
    """

    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )

    def require_workspace_fence(
        self, identity: "AuthoringWorkspaceIdentity"
    ) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
        )


class DraftMutationRequest(_LocalDraftFence):
    mutation: str = Field(min_length=1, max_length=128)
    arguments: dict[str, Any]


class RuntimeOverrideMaterializationRequest(_LocalDraftFence):
    program_run_id: str = Field(
        alias="programRunId", min_length=1, max_length=256
    )
    proposal_id: str = Field(
        alias="proposalId", pattern=r"^sha256:[0-9a-f]{64}$"
    )


class OpenDraftRequest(_LocalDraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class NewDraftRequest(_LocalDraftFence):
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class ValidateProgramSourceRequest(BaseModel):
    model_config = _DTO_CONFIG
    authoring: dict[str, Any]


class SaveDraftRequest(_LocalDraftFence):
    pass


class SaveDraftAsRequest(_LocalDraftFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_overwrite: bool = Field(default=False, alias="confirmOverwrite")


class ApplyDraftRequest(_DraftFence):
    pass


class DiscardDraftRequest(_LocalDraftFence):
    pass


class RevertDraftRequest(_DraftFence):
    pass


class LoadScenarioRequest(_DraftFence):
    scenario_id: str = Field(
        alias="scenarioId",
        min_length=1,
        max_length=128,
        pattern=r"^[a-z][a-z0-9_.-]*$",
    )
    scenario_catalog_revision: str = Field(
        alias="scenarioCatalogRevision",
        pattern=r"^sha256:[0-9a-f]{64}$",
    )
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


@dataclass(frozen=True, slots=True)
class _AppliedContext:
    scenario_id: str
    scenario_catalog_revision: str
    program_revision: str
    record_digest: str
    program_document: dict[str, Any]
    binding_document: dict[str, Any]
    projection_document: dict[str, Any]


def _raise_workspace_error(error: Exception) -> NoReturn:
    if isinstance(error, (DraftSessionError, ProgramArtifactError)):
        detail: dict[str, Any] = {
            "code": error.code,
            "message": str(error),
        }
        if isinstance(error, DraftSessionError) and error.detail != str(error):
            detail["context"] = error.detail
        raise HTTPException(status_code=error.status_code, detail=detail) from error
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


def _active_catalog(state: ComposerState):
    if state.headless_client is None:
        return state.node_catalog
    return read_active_node_environment(state).catalog


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
            scenario_catalog_revision=applied["scenario_catalog_revision"],
            program_revision=program_revision,
            record_digest=record_digest,
            program_document=program,
            binding_document=binding,
            projection_document=applied,
        ),
        None,
    )


def _workspace(state: ComposerState) -> dict[str, Any]:
    context, reason = _applied_context(state)
    for _attempt in range(3):
        identity = state.authoring_workspace.identity()
        workspace = state.draft_session.workspace_snapshot(
            applied_program_revision=(
                context.program_revision if context is not None else None
            ),
            applied_unavailable_reason=reason,
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
        raise headless_client_http_exception(error) from error
    targets = targets_projection.get("targets")
    control = control_projection.get("control_state")
    direct = targets.get("direct_control") if isinstance(targets, dict) else None
    direct_rows = (
        [row for row in targets.get("direct_control_targets", []) if isinstance(row, dict)]
        if isinstance(targets, dict) and isinstance(targets.get("direct_control_targets"), list)
        else []
    )
    if not direct_rows and isinstance(direct, dict):
        direct_rows = [direct]
    observed_rows = (
        [row for row in control.get("observed_target_states", []) if isinstance(row, dict)]
        if isinstance(control, dict) and isinstance(control.get("observed_target_states"), list)
        else []
    )
    observed = control.get("observed_motion") if isinstance(control, dict) else None
    authoring = state.draft_session.snapshot().get("authoring")
    if not isinstance(authoring, dict):
        raise DraftSessionError(
            "composer_draft_unavailable", "Composer has no Program Draft", status_code=404
        )
    node = _draft_node(authoring, instance_id)
    expected_node_id = (
        "motion.move_to_joint_target" if observed_kind == "joint" else "motion.move_to_pose"
    )
    node_target_role = node.get("parameters", {}).get("target_role")
    if node.get("node_id") != expected_node_id or type(node_target_role) is not str:
        raise DraftSessionError(
            "observed_move_target_mismatch",
            "selected move does not address an observed motion target",
            status_code=409,
        )
    matching_direct = [
        row for row in direct_rows
        if row.get("target_role_id") == node_target_role
    ]
    if len(matching_direct) != 1:
        raise DraftSessionError(
            "observed_move_target_mismatch",
            "selected move target role is not unique in the current execution owner",
            status_code=409,
        )
    direct = matching_direct[0]
    matching_observed = [
        row.get("motion") for row in observed_rows
        if row.get("motion_target_role_id") == node_target_role
        and row.get("command_target_id") == direct.get("command_target_id")
        and row.get("controllable_component_id") == direct.get("controllable_component_id")
        and isinstance(row.get("motion"), dict)
    ]
    if observed_rows:
        observed = matching_observed[0] if len(matching_observed) == 1 else None
    elif len(direct_rows) == 1:
        observed = control.get("observed_motion") if isinstance(control, dict) else None
    else:
        observed = None
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
        else {"current_tcp_pose": deepcopy(observed.get("current_tcp_pose"))}
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
        raise headless_client_http_exception(error) from error
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
        for field in ("resource_roles", "metadata", "collision_policy_ids"):
            if field in existing:
                resource[field] = deepcopy(existing[field])
    source_identity = {
        "active_projection": deepcopy(active),
        "transaction_sequence": snapshot.get("transaction_sequence"),
        "scene_resource_id": arguments["scene_resource_id"],
    }
    return {"scene_resource": resource}, source_identity


def _assistant_proposal_operations(
    state: ComposerState,
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    if set(arguments) != {"request_id", "proposal_index"}:
        raise DraftSessionError(
            "draft_arguments_invalid",
            "proposal materialization requires request_id and proposal_index",
        )
    request_id = arguments["request_id"]
    proposal_index = arguments["proposal_index"]
    if type(request_id) is not str or type(proposal_index) is not int or proposal_index < 0:
        raise DraftSessionError("draft_arguments_invalid", "proposal selection is invalid")
    if state.assistant_client is None:
        raise DraftSessionError(
            "assistant_proposal_unavailable",
            "assistant provider is unavailable",
            status_code=503,
        )
    try:
        status = state.assistant_client.status(request_id)
    except AssistantProviderClientError as error:
        raise DraftSessionError(
            error.code, str(error), status_code=error.status_code
        ) from error
    response = status.get("response") if status.get("state") == "completed" else None
    proposals = response.get("proposals") if isinstance(response, dict) else None
    if not isinstance(proposals, list) or proposal_index >= len(proposals):
        raise DraftSessionError(
            "assistant_proposal_not_found",
            "assistant proposal no longer exists",
            status_code=404,
        )
    proposal = proposals[proposal_index]
    if (
        response.get("assistantMode") != "composer-assistant"
        or not isinstance(proposal, dict)
        or proposal.get("effect") != "proposal"
        or proposal.get("status") != "draft"
        or proposal.get("requiresReview") is not True
    ):
        raise DraftSessionError(
            "assistant_proposal_rejected",
            "assistant response is not a reviewable Composer Draft proposal",
            status_code=409,
        )
    try:
        context = read_active_assistant_context(state, mode="composer-assistant")
    except (
        ActiveAssistantModeProjectionError,
        ActiveNodeEnvironmentProjectionError,
        ActiveSourceModelProjectionError,
    ) as error:
        raise DraftSessionError(error.code, str(error), status_code=error.status_code) from error
    tool = next(
        (
            row
            for row in context.assistant_mode.payload.get("tools", [])
            if row.get("tool_id") == proposal.get("toolId")
        ),
        None,
    )
    if (
        not isinstance(tool, dict)
        or tool.get("effect") != "proposal"
        or response.get("catalogHash")
        != context.assistant_mode.payload.get("catalog_hash")
        or tool.get("input_schema_hash") != proposal.get("inputSchemaHash")
    ):
        raise DraftSessionError(
            "assistant_proposal_stale",
            "assistant proposal schema is no longer current",
            status_code=409,
        )
    try:
        validate_callback_arguments(proposal.get("arguments"), tool["input_schema"])
    except CallbackSchemaValidationError as error:
        raise DraftSessionError(
            "assistant_proposal_invalid", str(error), status_code=400
        ) from error
    proposal_arguments = proposal["arguments"]
    tool_id = proposal["toolId"]
    proposal_suffix = hashlib.sha256(
        canonical_json_bytes(
            {
                "request_id": request_id,
                "proposal_index": proposal_index,
                "tool_id": tool_id,
                "arguments": proposal_arguments,
            }
        )
    ).hexdigest()[:12]
    resources: list[dict[str, Any]] = []
    if tool_id == "propose_scene_objects":
        report = proposal_arguments["inspection_report"]
        source_roots = set(proposal_arguments.get("source_roots", []))
        prefix = proposal_arguments.get("scene_resource_id_prefix", "")
        mesh_overrides = {
            row["source_root"]: row["mesh_resource_uri"]
            for row in proposal_arguments.get("mesh_resource_overrides", [])
        }
        for row in report["objects"]:
            if (
                not isinstance(row, dict)
                or not isinstance(row.get("scene_resource"), dict)
                or (
                    source_roots
                    and row.get("source_root") not in source_roots
                )
            ):
                continue
            resource = deepcopy(row["scene_resource"])
            resource["scene_resource_id"] = (
                f"{prefix}{resource['scene_resource_id']}"
            )
            replacement = mesh_overrides.get(row.get("source_root"))
            if (
                replacement is not None
                and resource.get("shape", {}).get("type") == "mesh"
            ):
                resource["shape"]["mesh_resource_uri"] = replacement
            resources.append(resource)
        maximum = proposal_arguments.get("max_proposals", len(resources))
        resources = resources[:maximum]
        operations = [
            {
                "mutation": "scene_draft_upsert_objects",
                "arguments": {"scene_resources": resources},
            }
        ]
    elif tool_id == "propose_scene_object_nodes":
        resources = deepcopy(proposal_arguments["scene_resources"])
        operations = [
            {
                "mutation": "scene_draft_upsert_objects",
                "arguments": {"scene_resources": resources},
            }
        ]
        authoring = state.draft_session.snapshot().get("authoring")
        used = _draft_instance_ids(authoring) if isinstance(authoring, dict) else set()
        operation_to_node = {
            "scene.add_object": "scene.add_object",
            "scene.upsert_object": "scene.upsert_object",
            "scene.remove_object": "scene.remove_object",
            "scene.attach_carried_object": "scene.attach_carried_object",
            "scene.detach_carried_object": "scene.detach_carried_object",
        }
        for resource in resources:
            resource_id = resource["scene_resource_id"]
            resource_token = (
                f"{resource_id[:48]}."
                f"{hashlib.sha256(resource_id.encode('utf-8')).hexdigest()[:8]}"
            )
            for operation_id in proposal_arguments["operations"]:
                node_id = operation_to_node[operation_id]
                parameters: dict[str, Any] = {"scene_resource_id": resource_id}
                if operation_id in {"scene.add_object", "scene.upsert_object"}:
                    parameters.update(
                        {"shape": deepcopy(resource["shape"]), "pose": deepcopy(resource["pose"])}
                    )
                elif operation_id == "scene.attach_carried_object":
                    parameters.update(
                        {
                            "target_role": proposal_arguments["target_role"],
                            "carrier_frame_role": proposal_arguments["carrier_frame_role"],
                            "touch_body_roles": deepcopy(proposal_arguments["touch_body_roles"]),
                        }
                    )
                elif operation_id == "scene.detach_carried_object":
                    parameters["target_role"] = proposal_arguments["target_role"]
                instance_id = (
                    f"assistant.{operation_id.removeprefix('scene.')}."
                    f"{resource_token}.{proposal_suffix}"
                )
                if instance_id in used:
                    raise DraftSessionError(
                        "assistant_proposal_already_materialized",
                        "assistant proposal is already present in this Draft",
                        status_code=409,
                    )
                used.add(instance_id)
                operations.append(
                    {
                        "mutation": "tree_draft_insert_node",
                        "arguments": {
                            "node": {
                                "node_id": node_id,
                                "instance_id": instance_id,
                                "parameters": parameters,
                                "children": [],
                            }
                        },
                    }
                )
    else:
        raise DraftSessionError(
            "assistant_proposal_unsupported",
            f"assistant Draft proposal is unsupported: {tool_id}",
            status_code=409,
        )
    if not operations or not resources:
        raise DraftSessionError(
            "assistant_proposal_empty",
            "assistant proposal contains no materializable Draft resources",
            status_code=409,
        )
    source_identity = {
        "request_id": request_id,
        "proposal_index": proposal_index,
        "assistant_mode": response["assistantMode"],
        "catalog_hash": response["catalogHash"],
        "tool_id": tool_id,
        "input_schema_hash": proposal["inputSchemaHash"],
        "arguments_digest": "sha256:"
        + hashlib.sha256(canonical_json_bytes(proposal_arguments)).hexdigest(),
    }
    return {
        "proposal_tool_id": tool_id,
        "operations": operations,
    }, source_identity


def _source_resolved_mutation(
    state: ComposerState,
    mutation: str,
    arguments: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]] | None:
    if mutation == "program_draft_store_observed_move":
        return _observed_move_arguments(state, arguments)
    if mutation == "program_draft_store_observed_scene_resource":
        return _observed_scene_arguments(state, arguments)
    if mutation == "assistant_draft_materialize_proposal":
        return _assistant_proposal_operations(state, arguments)
    return None


def _require_apply_ready(program: Program) -> None:
    applicability = draft_applicability(program.to_dict())
    if not applicability["applyReady"]:
        reason = applicability["unavailableReason"]
        raise DraftSessionError(
            reason["code"],
            reason["message"],
            status_code=409,
            detail={
                "affectedInstanceIds": applicability["affectedInstanceIds"],
            },
        )


def _apply_program(
    state: ComposerState,
    *,
    program: Program,
    scenario_id: str,
    scenario_catalog_revision: str,
    expected_applied_record_digest: str | None,
    authoring_claim: dict[str, Any],
) -> dict[str, Any]:
    _require_apply_ready(program)
    if state.headless_client is None:
        raise DraftSessionError(
            "program_apply_unavailable",
            "authoritative Program Apply requires a configured headless API",
            status_code=503,
        )
    try:
        candidate = Program.from_dict(
            program.to_dict(),
            _active_catalog(state),
            path="<composer-program-apply>",
        )
    except ProgramValidationError as error:
        raise DraftSessionError(
            "composer_draft_validation_failed",
            str(error),
            detail=str(error),
        ) from error
    result = state.headless_client.program_apply(
        {
            "scenario_id": scenario_id,
            "scenario_catalog_revision": scenario_catalog_revision,
            "program": candidate.to_dict(),
            "expected_applied_record_digest": expected_applied_record_digest,
            "authoring_claim": authoring_claim,
        }
    )
    try:
        validate_program_apply_result(result)
    except (ProgramBindingTransportError, TypeError, ValueError) as error:
        raise DraftSessionError(
            "program_apply_response_invalid",
            "headless Program Apply returned a divergent result",
            status_code=502,
        ) from error
    status = result.get("status")
    applied = result.get("applied")
    if (
        result.get("schema_version") != "manyforge.program_apply_result.v2"
        or status != "applied"
        or not isinstance(applied, dict)
        or applied.get("program_revision") != candidate.program_revision
        or not isinstance(applied.get("source"), dict)
        or not isinstance(applied["source"].get("record_digest"), str)
    ):
        raise DraftSessionError(
            "program_apply_response_invalid",
            "headless Program Apply returned a divergent result",
            status_code=502,
        )
    return result


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose revision-fenced authoring and thin authoritative Apply/Load."""

    router = APIRouter()

    def retained_applied_context() -> dict[str, Any]:
        context, reason = _applied_context(state)
        if context is None:
            return {"available": False, "reason": reason}
        return {
            "available": True,
            "scenario_id": context.scenario_id,
            "scenario_catalog_revision": context.scenario_catalog_revision,
            "program_revision": context.program_revision,
            "record_digest": context.record_digest,
            "program_document": deepcopy(context.program_document),
            "projection_document": deepcopy(context.projection_document),
        }

    def prepare_authoring_effect(
        body: _DraftFence, operation_kind: str
    ) -> dict[str, Any]:
        state.draft_session.require_current(
            draft_id=body.draft_id,
            expected_revision=body.expected_draft_revision,
        )
        pre_state = state.draft_session.retained_state()
        prepared: dict[str, Any] = {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "operation_kind": operation_kind,
            "effect_class": "local",
            "pre_state": pre_state,
        }
        if operation_kind == "apply_draft":
            document = pre_state["program"]
            if document is None:
                raise DraftSessionError(
                    "composer_authoring_draft_incomplete",
                    "Composer Draft has no strict Program candidate",
                    status_code=409,
                    detail={"diagnostics": pre_state["diagnostics"]},
                )
            _require_apply_ready(
                Program.from_dict(document, _active_catalog(state), path="<apply-intent>")
            )
            try:
                current_projection = state.current_scenario()
            except HeadlessClientError as error:
                raise headless_client_http_exception(error) from error
            current = current_projection.get("current_scenario")
            if current_projection.get("ready") is not True or not isinstance(
                current, dict
            ):
                raise DraftSessionError(
                    "program_current_scenario_unavailable",
                    "Program Apply requires one exact current active Scenario; "
                    "use Load Scenario first",
                    status_code=409,
                    detail={
                        "unavailableReasons": current_projection.get(
                            "unavailable_reasons", []
                        )
                    },
                )
            applied = retained_applied_context()
            if applied["available"] is not True:
                reason = applied.get("reason") or {
                    "code": "program_applied_selection_invalid",
                    "message": "Applied Program is absent",
                }
                raise DraftSessionError(
                    reason["code"],
                    reason["message"],
                    status_code=502,
                )
            if (
                current.get("scenario_id") != applied.get("scenario_id")
                or current.get("scenario_catalog_revision")
                != applied.get("scenario_catalog_revision")
                or current.get("program_revision")
                != applied.get("program_revision")
            ):
                raise DraftSessionError(
                    "program_current_scenario_inconsistent",
                    "current Scenario and Applied Program projections disagree",
                    status_code=409,
                )
            prepared.update(
                {
                    "effect_class": "external",
                    "external_kind": "program_apply",
                    "program_document": deepcopy(document),
                    "scenario_id": current["scenario_id"],
                    "scenario_catalog_revision": current[
                        "scenario_catalog_revision"
                    ],
                    "expected_applied_record_digest": applied["record_digest"],
                    "applied_context": applied,
                    "post_state": pre_state,
                }
            )
        elif operation_kind == "revert_draft":
            saved = pre_state["saved_document"]
            if saved is None:
                raise DraftSessionError(
                    "composer_saved_program_unavailable",
                    "Composer has no retained Saved Program",
                    status_code=404,
                )
            applied = retained_applied_context()
            if applied["available"] is not True:
                reason = applied.get("reason") or {
                    "code": "program_applied_selection_invalid",
                    "message": "Applied Program is absent",
                }
                raise DraftSessionError(
                    reason["code"],
                    reason["message"],
                    status_code=(
                        404
                        if reason["code"]
                        == "program_applied_selection_not_found"
                        else 502
                        if reason["code"]
                        == "program_applied_selection_invalid"
                        else 503
                    ),
                )
            post_state = deepcopy(pre_state)
            if pre_state["program"] != saved:
                post_state["draft_revision"] += 1
                post_state["authoring"] = from_program(
                    Program.from_dict(
                        deepcopy(saved),
                        _active_catalog(state),
                        path="<revert-intent>",
                    )
                )
                post_state["program"] = deepcopy(saved)
                post_state["diagnostics"] = []
            prepared.update(
                {
                    "effect_class": "external",
                    "external_kind": "revert_program",
                    "program_document": deepcopy(saved),
                    "scenario_id": applied["scenario_id"],
                    "scenario_catalog_revision": applied[
                        "scenario_catalog_revision"
                    ],
                    "expected_applied_record_digest": applied["record_digest"],
                    "applied_context": applied,
                    "post_state": post_state,
                }
            )
        return prepared

    def authoring_effect(
        body: _DraftFence,
        operation_kind: str,
        effect,
    ):
        """Run one external authoring effect through the durable owner.

        Only the retained external callers use this: Program Apply and Revert.
        Ordinary edits and saves commit through :func:`commit_local_authoring`
        instead, and every local branch of :func:`prepare_authoring_effect` was
        deleted when its last caller moved.
        """

        coordinator = state.authoring_admission
        if coordinator is None:
            raise HTTPException(
                status_code=503,
                detail={
                    "code": "authoring_not_ready",
                    "message": "authoritative authoring admission is unavailable",
                },
            )
        prepared: dict[str, Any] = {}

        def prepare() -> dict[str, Any]:
            prepared.update(prepare_authoring_effect(body, operation_kind))
            return prepared

        def execute_effect(claim: dict[str, Any]):
            claim_token = _ACTIVE_AUTHORING_CLAIM.set(claim)
            effect_token = _ACTIVE_AUTHORING_EFFECT.set(prepared)
            try:
                return effect(claim)
            finally:
                _ACTIVE_AUTHORING_EFFECT.reset(effect_token)
                _ACTIVE_AUTHORING_CLAIM.reset(claim_token)

        try:
            return coordinator.execute(
                body.admission_envelope(),
                operation_kind=operation_kind,
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                effect=execute_effect,
                post_image=state.authoring_retained_state,
                prepare_effect=prepare,
                after_release=(
                    lambda: state.prepare_current_applied_scene(
                        request_owner_epoch=body.request_owner_epoch,
                        request_id=body.request_id,
                    )
                    if operation_kind in {"apply_draft", "revert_draft"}
                    else None
                ),
            )
        except (DraftSessionError, ProgramArtifactError) as error:
            _raise_workspace_error(error)

    def authoring_gate(endpoint):
        @wraps(endpoint)
        def guarded(body):
            def invoke(_claim: dict[str, Any]):
                return endpoint(body)

            return authoring_effect(body, endpoint.__name__, invoke)

        return guarded

    def active_authoring_claim() -> dict[str, Any]:
        claim = _ACTIVE_AUTHORING_CLAIM.get()
        if claim is None:
            raise RuntimeError("authoring claim context is absent")
        return claim

    def active_authoring_effect() -> dict[str, Any]:
        effect = _ACTIVE_AUTHORING_EFFECT.get()
        if effect is None:
            raise RuntimeError("authoring effect context is absent")
        return effect

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
            commit = commit_local_authoring(
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

    @router.post("/api/program/draft/runtime-override-promotions")
    def materialize_runtime_override(
        body: RuntimeOverrideMaterializationRequest,
    ) -> dict[str, Any]:
        try:
            if state.headless_client is None:
                raise DraftSessionError(
                    "program_runtime_promotion_unavailable",
                    "runtime promotion requires the current Program Runner",
                    status_code=503,
                )
            try:
                projection = state.headless_client.program_runtime_controls(
                    body.program_run_id
                )
            except HeadlessClientError as error:
                raise headless_client_http_exception(error) from error
            controls = projection.get("runtime_controls")
            proposals = (
                controls.get("promotion_proposals")
                if isinstance(controls, dict)
                else None
            )
            proposal = next(
                (
                    row
                    for row in proposals
                    if isinstance(row, dict)
                    and row.get("proposal_id") == body.proposal_id
                ),
                None,
            ) if isinstance(proposals, list) else None
            if (
                projection.get("program_run_id") != body.program_run_id
                or not isinstance(proposal, dict)
                or proposal.get("status") != "ready"
                or proposal.get("program_run_id") != body.program_run_id
                or proposal.get("expected_draft_id") != body.draft_id
                or proposal.get("expected_draft_revision")
                != body.expected_draft_revision
                or not isinstance(proposal.get("node_instance_id"), str)
                or not isinstance(proposal.get("node_id"), str)
                or not isinstance(proposal.get("parameter_id"), str)
                or "value" not in proposal
            ):
                raise DraftSessionError(
                    "program_runtime_promotion_stale",
                    "runtime promotion no longer names this exact idle Draft",
                    status_code=409,
                )
            arguments = {
                "instance_id": proposal["node_instance_id"],
                "expected_node_id": proposal["node_id"],
                "parameters": {
                    proposal["parameter_id"]: deepcopy(proposal["value"])
                },
                "merge": True,
            }
            # Prepare the promoted parameter edit outside every owner lock.
            prepared = state.draft_session.prepare_mutation(
                tool_id="tree_draft_update_node_params",
                arguments=arguments,
                expected_revision=body.expected_draft_revision,
                catalog=state.node_catalog,
                draft_id=body.draft_id,
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="materialize_runtime_override",
                install=lambda: state.draft_session.install_prepared_mutation(
                    prepared
                ),
            )
        except Exception as error:
            _raise_workspace_error(error)
        return {
            "proposalId": proposal["proposal_id"],
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
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
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
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
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
        steps: the write happens outside every owner lock and the adoption is one
        composite commit fenced by the captured workspace identity. A stale
        adoption keeps the correctly written file and never labels newer
        in-memory content Saved.
        """

        prepared = state.draft_session.prepare_save(
            draft_id=body.draft_id,
            expected_revision=body.expected_draft_revision,
            target_path=target_path,
            confirm_overwrite=confirm_overwrite,
        )
        if _published_program_revision(state, prepared.target_path) == (
            prepared.program.program_revision
        ):
            # This exact content is already published at the target, so a lost
            # response is resolved without repeating the write.
            saved_path = prepared.resolved_target
        else:
            saved_path = save_program_artifact(
                prepared.program,
                prepared.target_path,
                allowed_roots=_program_write_roots(state),
                overwrite=prepared.overwrite,
            )
        try:
            commit = commit_local_authoring(
                state,
                body,
                operation_kind=prepared.tool_id,
                install=lambda: state.draft_session.install_prepared_save(
                    prepared, saved_path=saved_path
                ),
            )
        except AuthoringWorkspaceError as error:
            if error.code != "composer_workspace_stale":
                raise
            # The Program artifact is correctly written; only the Saved
            # baseline is refused, so newer in-memory content is never labelled
            # Saved and the client refreshes instead of retrying blindly.
            raise HTTPException(
                status_code=409,
                detail={
                    "code": "program_saved_baseline_not_adopted",
                    "message": (
                        "the Program artifact was written but a newer "
                        "workspace revision was not adopted as Saved"
                    ),
                    "path": saved_path,
                    "currentWorkspace": error.detail.get("workspace"),
                },
            ) from error
        return {
            **_workspace(state),
            "authoringWorkspace": commit.snapshot.as_dict(),
        }

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
    @authoring_gate
    def apply_draft(body: ApplyDraftRequest) -> JSONResponse:
        try:
            prepared = active_authoring_effect()

            def apply_current_program(program: Program) -> dict[str, Any]:
                _require_apply_ready(program)
                return _apply_program(
                    state,
                    program=program,
                    scenario_id=prepared["scenario_id"],
                    scenario_catalog_revision=prepared[
                        "scenario_catalog_revision"
                    ],
                    expected_applied_record_digest=prepared[
                        "expected_applied_record_digest"
                    ],
                    authoring_claim=active_authoring_claim(),
                )

            result = state.draft_session.apply_current_program(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                applier=apply_current_program,
            )
            return JSONResponse(
                status_code=200,
                content={
                    "status": "applied",
                    "headless": result,
                    "workspace": _workspace(state),
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/scenarios/load")
    def load_scenario(body: LoadScenarioRequest) -> JSONResponse:
        try:
            result = state.load_scenario(
                body.admission_envelope(),
                scenario_id=body.scenario_id,
                expected_catalog_revision=body.scenario_catalog_revision,
                confirm_discard=body.confirm_discard,
            )
            return JSONResponse(status_code=200, content=result)
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/load")
    def removed_program_load() -> None:
        raise HTTPException(
            status_code=410,
            detail={
                "code": "program_load_removed_use_scenario_load",
                "message": (
                    "Program-only Load was removed; use Open for editor-only "
                    "documents or Load Scenario for Deployment and Program activation"
                ),
            },
        )

    @router.post("/api/program/discard")
    def discard_draft(body: DiscardDraftRequest) -> dict[str, Any]:
        try:
            def read_applied() -> Program:
                context, reason = _applied_context(state)
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
                return candidate

            prepared = state.draft_session.prepare_discard_replacement(
                read_applied()
            )
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="discard_draft",
                install=lambda: state.draft_session.install_prepared_replacement(
                    prepared,
                    expected_draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return {
                **_workspace(state),
                "authoringWorkspace": commit.snapshot.as_dict(),
            }
        except Exception as error:
            _raise_workspace_error(error)

    @router.post("/api/program/revert")
    @authoring_gate
    def revert_draft(body: RevertDraftRequest) -> JSONResponse:
        try:
            def prepare_revert(
                saved_document: dict[str, Any],
            ) -> tuple[Program, Program, _AppliedContext]:
                context, reason = _applied_context(state)
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
                            else
                            502
                            if reason
                            and reason["code"]
                            == "program_applied_selection_invalid"
                            else 503
                        ),
                    )
                catalog = _active_catalog(state)
                try:
                    validated_saved = Program.from_dict(
                        saved_document,
                        catalog,
                        path="<composer-program-revert-saved>",
                    )
                    applied = Program.from_dict(
                        context.program_document,
                        catalog,
                        path="<composer-program-revert-applied>",
                    )
                except ProgramValidationError as error:
                    raise DraftSessionError(
                        "composer_revert_program_validation_failed",
                        str(error),
                        status_code=409,
                        detail=str(error),
                    ) from error
                return validated_saved, applied, context

            def apply_saved(
                program: Program,
                context: _AppliedContext,
            ) -> dict[str, Any]:
                result = _apply_program(
                    state,
                    program=program,
                    scenario_id=context.scenario_id,
                    scenario_catalog_revision=context.scenario_catalog_revision,
                    expected_applied_record_digest=context.record_digest,
                    authoring_claim=active_authoring_claim(),
                )
                selected, selected_reason = _applied_context(state)
                receipt = result["applied"]
                if (
                    selected_reason is not None
                    or selected is None
                    or selected.program_document != program.to_dict()
                    or selected.program_revision != program.program_revision
                    or selected.projection_document != receipt
                    or selected.record_digest
                    != receipt["source"]["record_digest"]
                ):
                    raise DraftSessionError(
                        "program_apply_selection_stale",
                        "Applied Program changed before Revert could commit its "
                        "local Draft identity",
                        status_code=409,
                    )
                return result

            result, _snapshot = state.draft_session.revert_saved_after_apply(
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
                prepare=prepare_revert,
                applier=apply_saved,
            )
            return JSONResponse(
                status_code=200,
                content={
                    "status": "reverted",
                    "headless": result,
                    "workspace": _snapshot,
                },
            )
        except Exception as error:
            _raise_workspace_error(error)

    return router


__all__ = ["create_draft_router"]
