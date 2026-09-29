"""Bounded HTTP authoring client for the Composer-owned Program workspace."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import math
from pathlib import Path
from typing import Any, NoReturn

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    raw_sha256_digest,
    StrictArtifactError,
)
from manyforge_behavior.model_source_capture import capture_deployment_model_sources
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_binding_transport import ProgramBindingTransportError
from manyforge_headless_api.program_binding_projection import (
    validate_applied_program_selection,
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
    scenario_project_facts,
)
from .scenario_artifacts import (
    SCENARIO_PROJECT_MEMBERS,
    ScenarioArtifactError,
    save_scenario_member,
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
    if isinstance(error, (DraftSessionError, ProgramArtifactError, ScenarioArtifactError)):
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
    try:
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
        not isinstance(status, dict)
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
            scenario_id=status["scenario_id"],
            source_document_digest=status["source_document_digest"],
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
    # execution_targets.v3 / execution_control_state.v6 (H1a-IF.2): motion
    # targets are component-owned rows; observations join by the full key.
    direct_rows = [
        {
            "controllable_component_id": component.get("controllable_component_id"),
            "command_target_id": target.get("command_target_id"),
            "operation_capability_id": target.get("operation_capability_id"),
            "target_role_id": target["command"].get("target_role_id"),
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


def create_draft_router(state: ComposerState) -> APIRouter:
    """Expose revision-fenced authoring and thin authoritative Apply/Load."""

    router = APIRouter()

    def prepare_program_activation(
        body: _DraftFence, program_document: dict[str, Any] | None
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Prepare one complete immutable activation for a Program change."""

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
        if (
            not isinstance(deployment, dict)
            or not isinstance(assignment, dict)
            or not isinstance(source_digest, str)
            or not isinstance(scenario_id, str)
            or not isinstance(project_path, str)
        ):
            raise DraftSessionError(
                "activation_input_incomplete",
                "Program Apply requires captured authoring input for the active Deployment",
                status_code=409,
            )
        assert context is not None
        source_closure = capture_deployment_model_sources(
            deployment, project_root=Path(project_path)
        )
        if source_closure != context["model_source_closure"]:
            raise DraftSessionError(
                "activation_model_source_changed",
                "An active Deployment model source changed since Apply",
                status_code=409,
            )
        overrides = deepcopy(context["typed_parameter_overrides"])
        if program_document is None:
            assignment = {"component_roles": [], "target_roles": []}
            overrides = {}
        captured_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "scenario_id": scenario_id,
                    "source_document_digest": source_digest,
                    "deployment": deployment,
                    "model_source_closure": source_closure,
                    "program": program_document,
                    "program_role_assignment": assignment,
                    "typed_parameter_overrides": overrides,
                }
            )
        )
        bundle, graph, _manager_epoch = state._manager_current(
            state.headless_client.manager_status()
        )
        intent = {
            "scenario_id": scenario_id,
            "source_document_digest": source_digest,
            "captured_input_digest": captured_digest,
            "deployment": deepcopy(deployment),
            "model_source_closure": source_closure,
            "program": deepcopy(program_document),
            "program_role_assignment": assignment,
            "typed_parameter_overrides": overrides,
            "expected_manager_current": {
                "retained_bundle_id": bundle,
                "graph_revision": graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "manager_request_id": "program-activation-"
            + hashlib.sha256(
                f"{body.request_owner_epoch}:{body.request_id}".encode("utf-8")
            ).hexdigest()[:32],
            "correlation_id": body.request_id,
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
        captured_workspace = state.authoring_workspace.snapshot().as_dict()
        project_path = captured_workspace["project"]["document_path"]
        scenario_member = (
            target_path is None and project_path is not None
            and prepared.resolved_target == str(
                Path(project_path) / SCENARIO_PROJECT_MEMBERS["program_draft"]
            )
        )
        saved_path = prepared.resolved_target
        artifact_written = not scenario_member

        def prepare_saved_project() -> dict[str, Any]:
            nonlocal artifact_written
            document, path, digest = save_scenario_member(
                project_path, "program_draft", captured_workspace["program"]["authoring"],
                allowed_roots=state.allowed_fs_roots,
            )
            artifact_written = True
            return scenario_project_facts(
                document, document_path=path, source_document_digest=digest
            )

        # Scenario members are written with their manifest under the claim below.
        if not scenario_member and _published_program_revision(
            state, prepared.target_path
        ) != prepared.program.program_revision:
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
                session="whole-workspace" if scenario_member else "program",
                prepare_project_under_claim=(
                    prepare_saved_project if scenario_member else None
                ),
                install=lambda: state.draft_session.install_prepared_save(
                    prepared, saved_path=saved_path
                ),
            )
        except AuthoringWorkspaceError as error:
            if error.code != "composer_workspace_stale" or not artifact_written:
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
            _require_apply_ready(program)
            intent, prepared = prepare_program_activation(body, program.to_dict())
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

    @router.post("/api/program/discard")
    def discard_draft(body: DiscardDraftRequest) -> dict[str, Any]:
        try:
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

            def validate_applied_under_claim() -> None:
                _program, current_identity = read_applied()
                if current_identity != applied_identity:
                    raise DraftSessionError(
                        "program_applied_changed",
                        "Applied Program changed before Discard committed",
                        status_code=409,
                    )

            commit = commit_local_authoring(
                state,
                body,
                operation_kind="discard_draft",
                validate_under_claim=validate_applied_under_claim,
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

    @router.post("/api/program/revert", status_code=202)
    def revert_draft(body: RevertDraftRequest) -> JSONResponse:
        try:
            retained = state.draft_session.retained_state()
            saved_document = retained.get("saved_document")
            if not isinstance(saved_document, dict):
                raise DraftSessionError(
                    "composer_saved_program_unavailable",
                    "Composer has no retained Saved Program",
                    status_code=404,
                )
            saved_empty = saved_document.get("forest") == [] and not saved_document.get("scene")
            saved_program = None if saved_empty else Program.from_dict(
                saved_document, _active_catalog(state), path="<composer-program-revert-saved>")
            intent, prepared = prepare_program_activation(
                body, saved_program.to_dict() if saved_program is not None else None
            )
            if saved_program is None:
                intent["program_authoring"] = deepcopy(saved_document)
            replacement_state = deepcopy(retained)
            restored_program = saved_program.to_dict() if saved_program is not None else None
            restored_authoring = from_program(saved_program) if saved_program is not None else deepcopy(saved_document)
            if replacement_state.get("authoring") != restored_authoring:
                replacement_state["draft_revision"] += 1
            replacement_state["program"] = restored_program
            replacement_state["authoring"] = restored_authoring
            replacement_state["diagnostics"] = derive_candidate(restored_authoring, _active_catalog(state))[1]
            replacement = state.draft_session.prepare_load_replacement(
                replacement_state, _active_catalog(state)
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
                if state.draft_session.retained_state() != retained:
                    raise DraftSessionError(
                        "composer_draft_stale",
                        "Program Draft changed before Revert submission",
                        status_code=409,
                    )

            def commit_saved() -> None:
                state.authoring_workspace.commit_program(
                    draft_id=body.draft_id,
                    expected_identity=identity,
                    install=lambda: state.draft_session.install_prepared_replacement(
                        replacement,
                        expected_draft_id=body.draft_id,
                        expected_revision=body.expected_draft_revision,
                    ),
                    notify=False,
                )

            result = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind="revert_program",
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                validate_under_claim=validate_submission,
                commit_before_submit=commit_saved,
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
