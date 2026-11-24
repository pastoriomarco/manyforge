"""Thin HTTP boundary for the separate Composer Deployment Draft."""

from __future__ import annotations

from copy import deepcopy
import json
import logging
from pathlib import Path
from contextlib import contextmanager
import time
from typing import Any, Callable, Iterator, Literal, NoReturn

from fastapi import APIRouter, HTTPException
from manyforge_behavior import parametric_tool
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    raw_sha256_digest,
)
from manyforge_behavior.model_source_capture import (
    ModelSourceCaptureError,
    capture_deployment_model_sources,
    load_tool_parameters,
)
from manyforge_behavior.model_source_capture import SceneMeshPathError
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.held_run_authoring import (
    held_run_draft_edit_allowed,
    manual_operation_settled,
)
from pydantic import BaseModel, ConfigDict, Field

from .application_identity import (
    ApplicationStartIdText,
    RequestKeyText,
    format_application_start_id,
    new_request_key,
    parse_application_start_id,
)
from .authoring_admission import (
    AuthoringOperationError,
    AuthoringMutationEnvelope,
    PARTIAL_EFFECT_OBSERVATION_CODES,
    draft_session_http_exception,
    headless_client_http_exception,
    refused_before_effect,
    scenario_artifact_http_exception,
)
from .component_catalog import component_catalog, profile_facts
from .deployment_artifacts import (
    load_deployment_artifact,
)
from .deployment_draft import DeploymentDraftError, PreparedDeploymentReplacement
from .deployment_intent import (
    DEPLOYMENT_INTENT_API_VERSION,
)
from .draft_session import DraftSessionError
from .apply_orchestration import (
    ApplyRefused,
    ApplyStepFailed,
    Preview,
    candidate_preview,
    apply_effect_unknown,
    apply_step,
    apply_step_source,
    one_apply_at_a_time,
    preview_view,
    run_apply,
)
from .deployment_defaults import effective_fields, with_defaults
from .routes_draft import _active_catalog
from .scenario_restore import saved_deployment_differs, saved_program_restore
from .state import ComposerState
from .authoring_workspace import (
    AuthoringOperationContext,
    AuthoringWorkspaceError,
    AuthoringWorkspaceIdentity,
)
from .scenario_artifacts import (
    ScenarioArtifactError,
)
from .authoring_workspace_fence import (
    commit_draft_edit,
    commit_local_authoring,
    require_captured_workspace_identity,
)


_DTO = ConfigDict(populate_by_name=True, extra="forbid", allow_inf_nan=False)
_LOGGER = logging.getLogger(__name__)


def _capture_model_sources(
    state: ComposerState, deployment: dict[str, Any]
) -> dict[str, Any]:
    project = state.deployment_source_project_path
    if not isinstance(project, str):
        raise DeploymentDraftError(
            "activation_source_project_unavailable",
            "Activation preparation requires a source project directory",
            status_code=409,
        )
    try:
        return capture_deployment_model_sources(
            deployment, project_root=Path(project), paths=state.project_paths
        )
    except ModelSourceCaptureError as error:
        raise DeploymentDraftError(
            error.code or "activation_model_source_invalid",
            str(error),
            status_code=409,
        ) from error


def _capture_scene_meshes(
    program: dict[str, Any] | None, state: ComposerState, project_path: str
) -> dict[str, Any]:
    try:
        return state.capture_scene_mesh_closure(program, project_path=project_path)
    except SceneMeshPathError as error:
        raise DeploymentDraftError(error.code, str(error), status_code=409) from error


class _Fence(BaseModel):
    model_config = _DTO

    draft_id: str = Field(alias="draftId", min_length=1, max_length=256)
    expected_draft_revision: int = Field(
        alias="expectedDraftRevision", ge=0, le=9_007_199_254_740_991
    )
    expected_admission_generation: int = Field(
        alias="expectedAdmissionGeneration", ge=1, le=9_007_199_254_740_991
    )
    application_start_id: ApplicationStartIdText = Field(alias="applicationStartId")
    request_key: RequestKeyText = Field(alias="requestKey")
    expected_workspace_epoch: str = Field(
        alias="expectedWorkspaceEpoch", min_length=1, max_length=256
    )
    expected_workspace_revision: int = Field(
        alias="expectedWorkspaceRevision", ge=1, le=9_007_199_254_740_991
    )

    def admission_envelope(self) -> AuthoringMutationEnvelope:
        return AuthoringMutationEnvelope(
            expected_admission_generation=self.expected_admission_generation,
            application_start_id=parse_application_start_id(self.application_start_id),
            request_key=bytes.fromhex(self.request_key),
        )

    def require_workspace_fence(self, identity) -> None:
        require_captured_workspace_identity(
            self.expected_workspace_epoch,
            self.expected_workspace_revision,
            identity,
        )


class _LocalFence(_Fence):
    """Migrated ordinary-edit fence: the Deployment Draft plus the workspace.

    Deployment candidate Preview/Apply/Revert stay on the external-operation
    path and keep the plain :class:`_Fence` shape.
    """

    pass


class OpenDeploymentRequest(_LocalFence):
    path: str = Field(min_length=1, max_length=4096)
    confirm_discard: bool = Field(default=False, alias="confirmDiscard")


class UpdateDeploymentRequest(_LocalFence):
    path: list[str | int] = Field(min_length=1, max_length=32)
    operation: Literal["set", "append", "remove"] = "set"
    value: Any = None


class AddComponentsRequest(_LocalFence):
    components: list[dict[str, Any]] = Field(min_length=1, max_length=64)
    attachments: list[dict[str, Any]] = Field(default_factory=list, max_length=64)


class RemoveComponentRequest(_LocalFence):
    component_id: str = Field(min_length=1, max_length=256)


class ReplaceToolRequest(_LocalFence):
    component_id: str = Field(min_length=1, max_length=256)
    replacement_component_id: str = Field(min_length=1, max_length=256)


class UpdateControlledStopRequest(_LocalFence):
    controllable_component_id: str = Field(min_length=1, max_length=256)
    command_target_id: str = Field(min_length=1, max_length=256)
    profile: dict[str, Any] | None


class UpdateMotionTargetLimitRequest(_LocalFence):
    controllable_component_id: str = Field(min_length=1, max_length=256)
    command_target_id: str = Field(min_length=1, max_length=256)
    maximum_tcp_linear_speed_mps: float = Field(gt=0.0, le=1000.0)


class DiscardDeploymentRequest(_LocalFence):
    pass


class DeploymentCandidateRequest(_Fence):
    """The fence of one server-run Preview/Apply/Revert.

    Built by the server from the current Drafts, workspace and admission
    generation (:func:`server_deployment_fence`); the request body is ``{}``.
    """


class ServerApplyRequest(BaseModel):
    """Preview takes an empty body."""

    model_config = _DTO


class ConfirmedApplyRequest(ServerApplyRequest):
    """Apply and Revert: the ``scope`` of the Preview the user confirmed.

    When present, the Apply is refused before any effect if the activation
    input changed since that Preview (``activation_input_changed_after_confirmation``).
    ``confirmed_collision_rule_ids`` names every ``rule_id`` of the Preview's
    ``collision_rules``; an Apply that changes a rule it leaves out ends
    ``collision_rule_confirmation_required`` before any component step.
    """

    scope: dict[str, Any] | None = None
    # The ids of the Preview's ``collision_rules`` the user confirmed. An Apply
    # whose Preview changes a rule this list leaves out ends
    # ``collision_rule_confirmation_required`` before any component step.
    confirmed_collision_rule_ids: list[str] = Field(default_factory=list, max_length=64)
    # The command's identity (OD-214): the same key again returns its row.
    request_key: RequestKeyText


def activation_scope(state: ComposerState) -> dict[str, Any]:
    """The complete activation input a Preview is computed from.

    Deployment activation compiles the Deployment Draft together with the
    current Program Draft and the Scenario source document; the scope names
    all of them.
    """

    deployment = state.deployment_draft_session.retained_state()
    document = deployment.get("document")
    program = state.draft_session.snapshot()
    return {
        "deployment_draft_id": deployment.get("draft_id"),
        "deployment_draft_revision": deployment.get("draft_revision"),
        "deployment_revision": (
            document.get("deployment_revision") if isinstance(document, dict) else None
        ),
        "program_draft_id": program.get("draft_id"),
        "program_draft_revision": program.get("draft_revision"),
        "program_revision": program.get("program_revision"),
        "source_document_digest": state.scenario_source_document_digest,
    }


def require_activation_scope(state: ComposerState, scope: dict[str, Any] | None) -> None:
    """Refuse, before any effect, when the confirmed input changed."""

    if scope is not None and scope != activation_scope(state):
        raise refused_before_effect(
            409,
            {
                "code": "activation_input_changed_after_confirmation",
                "message": "Draft changed after confirmation; review again",
            },
        )


@contextmanager
def apply_slot() -> Iterator[None]:
    """The one Apply slot; a second Apply is refused ``apply_in_progress``."""

    try:
        with one_apply_at_a_time():
            yield
    except ApplyRefused as refusal:
        if refusal.code != "apply_in_progress":
            raise
        raise refused_before_effect(
            409, {"code": refusal.code, "message": str(refusal)}
        ) from refusal


def server_admission_generation(state: ComposerState) -> int:
    if state.headless_client is None:
        raise DeploymentDraftError(
            "deployment_candidate_unavailable",
            "Deployment preview and Apply require the configured Manager",
            status_code=503,
        )
    return state.headless_client.authoring_admission().get("admission_generation")


def server_deployment_fence(state: ComposerState) -> DeploymentCandidateRequest:
    """The current Deployment Draft, workspace and admission, as one fence."""

    identity = state.authoring_workspace.identity()
    return DeploymentCandidateRequest(
        draft_id=identity.deployment_draft_id,
        expected_draft_revision=identity.deployment_draft_revision,
        expected_admission_generation=server_admission_generation(state),
        # A server-originated request: Composer's own `A` and a fresh key.
        application_start_id=format_application_start_id(state.application_start_id),
        request_key=new_request_key().hex(),
        expected_workspace_epoch=identity.workspace_epoch,
        expected_workspace_revision=identity.workspace_revision,
    )


def _applied_program_revision(state: ComposerState) -> str | None:
    """The Applied Program revision, or None when it cannot be read now."""

    try:
        applied = state.headless_client.program_applied()
    except HeadlessClientError:
        return None
    revision = applied.get("program_revision") if isinstance(applied, dict) else None
    return revision if isinstance(revision, str) and revision else None


# Deployment Apply refusals raised before admission or any Manager submission
# (or under the claim before it, which then settles as proven no-effect): a
# no-op Apply, and a confirmed activation input that changed meanwhile.
def _raise(error: Exception) -> NoReturn:
    if isinstance(error, AuthoringWorkspaceError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringOperationError)
        and error.code == "program_scene_preparation_unavailable"
    ):
        raise HTTPException(
            status_code=503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if (
        isinstance(error, AuthoringOperationError)
        and error.code in PARTIAL_EFFECT_OBSERVATION_CODES
    ):
        raise HTTPException(
            status_code=503,
            detail={"code": error.code, "message": str(error)},
        ) from error
    # Manager's definite refusal of the candidate (preview or Apply). The
    # message names Manager's own code and message; claim settlement already
    # happened under its code before this rendering.
    if isinstance(error, AuthoringOperationError) and error.code in {
        "manager_apply_rejected",
        "deployment_candidate_rejected",
    }:
        raise HTTPException(
            status_code=409,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, DeploymentDraftError):
        raise HTTPException(
            status_code=error.status_code,
            detail={"code": error.code, "message": str(error)},
        ) from error
    if isinstance(error, ScenarioArtifactError):
        raise scenario_artifact_http_exception(error) from error
    if isinstance(error, HeadlessClientError):
        raise headless_client_http_exception(error) from error
    if isinstance(error, DraftSessionError):
        raise draft_session_http_exception(error) from error
    raise error


def _workspace(state: ComposerState) -> dict[str, Any]:
    authoring = state.authoring_workspace.snapshot()
    deployment = authoring.as_dict()["deployment"]
    document = deployment["document"]
    saved_document = deployment["savedDocument"]
    workspace = {
        "schemaVersion": "manyforge.composer_deployment_workspace.v2",
        "draft": {
            "id": deployment["draftId"],
            "revision": deployment["draftRevision"],
            "document": document,
            "deploymentRevision": (
                document.get("deployment_revision")
                if isinstance(document, dict)
                else None
            ),
            "graphRevision": (
                document.get("graph_revision")
                if isinstance(document, dict)
                else None
            ),
            "differsFromSaved": deployment["differsFromSaved"],
        },
        "saved": {
            "path": deployment["savedPath"],
            "deploymentRevision": (
                saved_document.get("deployment_revision")
                if isinstance(saved_document, dict)
                else None
            ),
        },
    }
    workspace["authoringWorkspace"] = authoring.identity.as_dict()
    # Apply, Revert, Load and New in progress and the last ended (OD-214).
    workspace["apply_commands"] = state.apply_commands.snapshot()
    draft = workspace["draft"]
    current: dict[str, Any] | None = None
    binding: dict[str, Any] | None = None
    unavailable: list[str] = []
    if state.headless_client is None:
        unavailable.append("headless_unavailable")
    else:
        try:
            projection = state.published_scenario()
            value = projection.get("published_scenario")
            if isinstance(value, dict):
                current = value
        except HeadlessClientError as error:
            unavailable.append(error.code)
    draft_revision = draft["deploymentRevision"]
    applied_revision = (
        current.get("deployment_revision") if current is not None else None
    )
    applied_document = (
        state.applied_deployment_for_publication(current)
        if current is not None
        else None
    )
    # Applied holds the compiled input (required defaults filled at Apply);
    # the Draft keeps the document as authored, so compare it filled the same way.
    run_ready = (
        applied_document is not None
        and isinstance(document, dict)
        and with_defaults(document) == applied_document
    )
    # The same comparison the Revert guard makes, on the Saved this view
    # captured with its authoring snapshot; unknown without both sides.
    workspace["saved"]["differsFromApplied"] = saved_deployment_differs(
        saved_document, applied_document
    )
    # Catalog candidates were a legacy package-selection view. Authoring now
    # operates only on the open Scenario and its explicit Saved baseline.
    matching_scenarios: list[dict[str, Any]] = []
    saved_scenarios: list[dict[str, Any]] = []
    workspace.update(
        {
            "managerApplied": {
                "activationOrder": (
                    {
                        "retainedBundleId": current["retained_bundle_id"],
                        "graphRevision": current["graph_revision"],
                        "componentIds": [
                            row["component_id"] for row in applied_document["components"]
                        ],
                    }
                    if applied_document is not None and current is not None
                    else None
                ),
                "deploymentRevision": applied_revision,
                "scenarioId": (
                    current.get("scenario_id") if current is not None else None
                ),
            },
            "runAdmission": {
                "ready": run_ready,
                "reason": (
                    None
                    if run_ready
                    else "Deployment Draft differs from Manager Applied"
                ),
            },
            "binding": binding,
            "scenarioCandidates": matching_scenarios,
            "savedScenarioCandidates": saved_scenarios,
            "unavailableReasons": unavailable,
        }
    )
    return workspace


def deployment_unapplied(view: dict[str, Any]) -> bool:
    """Something is applied and the Deployment Draft is not it.

    The publication identity (revision) decides when both sides carry one;
    otherwise the backend's document comparison (run admission) decides.
    """

    draft = view.get("draft") or {}
    applied = (view.get("managerApplied") or {}).get("deploymentRevision")
    if draft.get("document") is None or applied is None:
        return False
    revision = draft.get("deploymentRevision")
    if revision is not None:
        return revision != applied
    return (view.get("runAdmission") or {}).get("ready") is not True


def read_applied_deployment(
    state: ComposerState,
) -> tuple[dict[str, Any], str | None, str]:
    """The Manager Applied Deployment as captured at its Apply, the project
    folder its relative sources resolve against, and its identity.

    Only the captured authoring input whose exact candidate was published by
    Manager and Program binding is returned.
    """

    current_projection = state.published_scenario()
    current = current_projection.get("published_scenario")
    if current_projection.get("ready") is not True or not isinstance(
        current, dict
    ):
        raise DeploymentDraftError(
            "deployment_applied_unavailable",
            "Deployment Discard requires one exact Manager Applied Scenario",
            status_code=409,
        )
    if state.headless_client is None:
        raise DeploymentDraftError(
            "deployment_applied_unavailable",
            "Deployment Discard requires the configured Manager",
            status_code=503,
        )
    context = state.applied_activation_for_publication(current)
    if context is None:
        raise DeploymentDraftError(
            "deployment_applied_projection_mismatch",
            "this Composer process has no exact Applied Deployment baseline",
            status_code=409,
        )
    applied = context["deployment"]
    identity = raw_sha256_digest(
        canonical_json_bytes(
            {
                "scenario_id": current["scenario_id"],
                "deployment": applied,
            }
        )
    )
    return applied, context["project_path"], identity


def prepare_deployment_saved_baseline(
    state: ComposerState,
    *,
    saved_document: dict[str, Any],
    saved_path: str,
    source_project_root: str | None,
    draft_document: dict[str, Any] | None = None,
) -> PreparedDeploymentReplacement:
    """The Deployment Draft with ``saved_document``, written at
    ``saved_path``, as its Saved baseline; the Draft keeps its own content
    (OD-365: Save writes the Applied Deployment, never unapplied edits).
    ``draft_document``, when given, is the Draft's new content (a Save into
    another project folder rebases its sources)."""

    retained = state.deployment_draft_session.retained_state()
    if draft_document is not None and draft_document != retained["document"]:
        retained.update(
            draft_revision=retained["draft_revision"] + 1,
            document=deepcopy(draft_document),
        )
    retained.update(saved_path=saved_path, saved_document=deepcopy(saved_document))
    return state.deployment_draft_session.prepare_load_replacement(
        retained, source_project_root=source_project_root
    )


def prepare_deployment_discard(
    state: ComposerState, draft_id: str, expected_revision: int
) -> tuple[Callable[[], None], Callable[[], bool]]:
    """Prepare returning the Deployment Draft to the exact Manager Applied one.

    Returns the under-commit validation and the no-I/O install, so the
    Deployment Discard and the Scenario Discard commit it the same way.
    """

    applied, _root, applied_identity = read_applied_deployment(state)
    # The publication read and compilation stay outside owner locks.
    # Once admission is claimed, re-read its exact identity before the
    # local install; Manager publication is external to the workspace
    # revision and therefore needs this explicit validation.
    compiled = state.deployment_draft_session.preview_source(
        json.dumps(applied, ensure_ascii=False, allow_nan=False)
    )

    def validate_applied_unchanged() -> None:
        _current, _root, current_identity = read_applied_deployment(state)
        if current_identity != applied_identity:
            raise DeploymentDraftError(
                "deployment_applied_changed",
                "Manager Applied Deployment changed before Discard committed",
                status_code=409,
            )

    return validate_applied_unchanged, lambda: state.deployment_draft_session.commit_compiled(
        compiled,
        draft_id=draft_id,
        expected_revision=expected_revision,
    )


def discard_deployment_draft(state: ComposerState, body: Any) -> None:
    """Return the Deployment Draft to Applied (Draft-only, no Manager effect)."""

    validate, install = prepare_deployment_discard(
        state, body.draft_id, body.expected_draft_revision
    )
    commit_draft_edit(
        state,
        body,
        operation_kind="discard_deployment_draft",
        session="deployment",
        validate=validate,
        install=install,
    )


_ASSISTANT_PROFILES = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment/resources/maintained/maintained_assistant_profiles.v2.json"
)
# Bound by the assistant component's row, never by the Deployment's aliases.
_CHAT_MODEL_ALIAS = "assistant_model"


def assistant_profile_aliases() -> dict[str, Any]:
    """Each maintained assistant profile: its chat mode and runtime aliases."""

    profiles = json.loads(_ASSISTANT_PROFILES.read_text(encoding="utf-8"))["profiles"]
    return {
        "schemaVersion": "manyforge.composer_assistant_profile_aliases.v2",
        "profiles": [
            {
                "profileId": row["profile_id"],
                "chat": row["mode_profile_id"] is not None,
                "modelEndpointAliases": sorted(
                    endpoint["model_endpoint_alias"]
                    for endpoint in row["model_endpoints"]
                    if endpoint["model_endpoint_alias"] != _CHAT_MODEL_ALIAS
                ),
            }
            for row in sorted(profiles, key=lambda row: row["profile_id"])
        ],
    }


def _tool_declarations(
    state: ComposerState, row: dict[str, Any]
) -> dict[str, set[str]] | None:
    """The command targets, frames and collision bodies a tool row declares:
    from its parameters (a generated tool: its body target ``tool``, a
    gripper target per jaw pair, its TCPs and collision bodies), else from
    its model's catalog facts plus its custom TCPs. None when they cannot be
    read (parameters that do not load, a model without facts)."""

    parameters = row["model_source"].get("parameters")
    if parameters is not None:
        project = state.deployment_source_project_path
        if not isinstance(project, str):
            return None
        try:
            loaded, _folder = load_tool_parameters(
                row["component_id"], parameters,
                project_root=Path(project), paths=state.project_paths,
            )
        except (parametric_tool.ParametricToolError, ModelSourceCaptureError):
            return None
        semantics = parametric_tool.component_semantics(loaded)
        return {
            "commandTargets": {
                "tool", *(target["command_target_id"] for target in semantics["gripper_targets"])
            },
            # Its TCPs and, on its body target, each spring slider (OD-371).
            "frames": {tcp["id"] for tcp in semantics["tcps"]} | {
                slider["collision_body_role_id"] for slider in semantics["spring_sliders"]
            },
            "collisionBodies": set(semantics["collision_bodies"]),
        }
    facts = profile_facts().get(row["component_profile_id"])
    if facts is None:
        return None
    targets = [
        *facts.get("frameTargets", []),
        *facts.get("tcpTargets", []),
        *facts.get("collisionBodyTargets", []),
    ]
    return {
        "commandTargets": {target["commandTargetId"] for target in targets},
        "frames": {
            frame
            for target in facts.get("frameTargets", [])
            for frame in target["frameRoleIds"]
        } | {tcp["tcp_id"] for tcp in row.get("custom_tcps", [])},
        "collisionBodies": {
            body
            for target in facts.get("collisionBodyTargets", [])
            for body in target["collisionBodyRoleIds"]
        },
    }


def create_deployment_draft_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/api/deployment/assistant-profiles")
    def read_assistant_profiles() -> dict[str, Any]:
        return assistant_profile_aliases()

    def execute(
        body: _Fence,
        operation_kind: str,
        *,
        prepare,
        effect,
        validate=None,
        commit_before_submit=None,
        after_release=None,
    ):
        try:
            prepare()
            coordinator = state.authoring_admission
            if coordinator is None:
                raise DeploymentDraftError(
                    "authoring_not_ready",
                    "authoritative authoring admission is unavailable",
                    status_code=503,
                )
            identity = state.authoring_workspace.identity()
            body.require_workspace_fence(identity)
            # Preparation needs idle admission. Capture its immutable result
            # before taking the claim; under the claim only recheck the input
            # fence. Manager validates the original preview and parent claim.
            validation = validate(None) if validate is not None else None
            candidate = (
                validation.get("prepared_deployment_candidate")
                if isinstance(validation, dict)
                else None
            )
            if isinstance(candidate, dict) and candidate.get("confirmation_required"):
                return {
                    "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                    "workspace": _workspace(state),
                    "result": effect(None),
                }

            def validate_submission(claim: dict[str, Any]) -> None:
                body.require_workspace_fence(state.authoring_workspace.identity())
                if validate is not None:
                    validate(claim)

            result = coordinator.execute_external_submission(
                body.admission_envelope(),
                operation_kind=operation_kind,
                draft_id=body.draft_id,
                draft_revision=body.expected_draft_revision,
                payload=body.model_dump(mode="json", by_alias=True),
                validate_under_claim=validate_submission,
                submit=effect,
                commit_before_submit=commit_before_submit,
                after_release=after_release,
                workspace=state.authoring_workspace,
            )
            return {
                "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                "workspace": _workspace(state),
                "result": result,
            }
        except Exception as error:
            _raise(error)

    @router.get("/api/deployment/workspace")
    def read_workspace() -> dict[str, Any]:
        return _workspace(state)

    @router.get("/api/deployment/component-catalog")
    def read_component_catalog() -> dict[str, Any]:
        """Components Add component can compose into the Deployment Draft."""

        return component_catalog(state.deployment_source_project_path)

    @router.post("/api/deployment/open-draft")
    def open_draft(body: OpenDeploymentRequest) -> dict[str, Any]:
        try:
            state.deployment_draft_session.require_open_allowed(
                body.draft_id,
                body.expected_draft_revision,
                confirm_discard=body.confirm_discard,
            )
            # The artifact read and its compilation happen outside every owner
            # lock; the commit installs the compiled value atomically and
            # rechecks the discard decision under the composite fence.
            source, source_path = load_deployment_artifact(
                body.path, allowed_roots=state.allowed_fs_roots
            )
            compiled = state.deployment_draft_session.preview_source(source)
            commit = commit_local_authoring(
                state,
                body,
                operation_kind="open_deployment_draft",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_open(
                    compiled,
                    source_path,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                    confirm_discard=body.confirm_discard,
                ),
            )
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/mutations")
    def mutate_draft(body: UpdateDeploymentRequest) -> dict[str, Any]:
        try:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            compiled = state.deployment_draft_session.preview_update(
                body.path,
                body.value,
                body.operation,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            commit_draft_edit(
                state,
                body,
                operation_kind="mutate_deployment_draft",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/components")
    def add_components(body: AddComponentsRequest) -> dict[str, Any]:
        """Add component: new component rows and their attachment rows in one edit."""

        try:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            compiled = state.deployment_draft_session.preview_component_addition(
                body.components,
                body.attachments,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            commit_draft_edit(
                state,
                body,
                operation_kind="add_deployment_components",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/components/remove")
    def remove_component(body: RemoveComponentRequest) -> dict[str, Any]:
        """Remove a component and the attachment rows that name it, in one edit
        (a mounted tool and its mount row leave together)."""

        try:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            compiled = state.deployment_draft_session.preview_component_removal(
                body.component_id,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            commit_draft_edit(
                state,
                body,
                operation_kind="remove_deployment_component",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/components/replace-tool")
    def replace_tool(body: ReplaceToolRequest) -> dict[str, Any]:
        """Replace tool (OD-367): the replacement takes over the old tool's
        mount and every reference to it in the Deployment and Program Drafts,
        one Draft-only edit; the old tool leaves. Lists what the replacement
        does not declare of what those references name."""

        try:
            deployment = state.deployment_draft_session
            deployment.require_current(body.draft_id, body.expected_draft_revision)
            compiled, used = deployment.preview_tool_replacement(
                body.component_id,
                body.replacement_component_id,
                draft_id=body.draft_id,
                expected_revision=body.expected_draft_revision,
            )
            program = state.draft_session
            prepared = (
                program.prepare_mutation(
                    tool_id="program_draft_replace_component",
                    arguments={
                        "component_id": body.component_id,
                        "replacement_component_id": body.replacement_component_id,
                    },
                    expected_revision=program.revision,
                    catalog=state.node_catalog,
                    draft_id=program.draft_id,
                )
                if program.program_loaded
                else None
            )
            if prepared is not None:
                for kind, ids in prepared.details["used"].items():
                    used[kind].update(ids)

            def validate() -> None:
                deployment.require_current(body.draft_id, body.expected_draft_revision)
                if prepared is not None:
                    program.require_identity(prepared.draft_id, prepared.base_revision)

            def install() -> bool:
                changed = prepared is not None and program.install_prepared_mutation(prepared)
                return deployment.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ) or changed

            commit_draft_edit(
                state,
                body,
                operation_kind="replace_deployment_tool",
                # Both Drafts in one commit when the Program names the tool.
                session="scenario" if prepared is not None and prepared.changed else "deployment",
                validate=validate,
                install=install,
            )
            row = next(
                row for row in compiled.document["components"]
                if row["component_id"] == body.replacement_component_id
            )
            declared = _tool_declarations(state, row)
            return {
                "schemaVersion": "manyforge.composer_tool_replacement.v1",
                "workspace": _workspace(state),
                "replacement": {
                    "componentId": body.component_id,
                    "replacementComponentId": body.replacement_component_id,
                    "programReferences": (
                        0 if prepared is None else len(prepared.details["references"])
                    ),
                    # What the rewritten references name and the new tool does
                    # not declare; null when its declarations cannot be read.
                    "lacks": None if declared is None else {
                        kind: sorted(ids - declared[kind]) for kind, ids in used.items()
                    },
                },
            }
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/controlled-stop")
    def update_controlled_stop(body: UpdateControlledStopRequest) -> dict[str, Any]:
        try:
            compiled = (
                state.deployment_draft_session.preview_controlled_stop_update(
                    body.profile,
                    controllable_component_id=body.controllable_component_id,
                    command_target_id=body.command_target_id,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
            )
            commit_draft_edit(
                state,
                body,
                operation_kind="mutate_deployment_controlled_stop",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/draft/motion-target-limit")
    def update_motion_target_limit(
        body: UpdateMotionTargetLimitRequest,
    ) -> dict[str, Any]:
        try:
            compiled = (
                state.deployment_draft_session.preview_motion_target_limit_update(
                    controllable_component_id=body.controllable_component_id,
                    command_target_id=body.command_target_id,
                    maximum_tcp_linear_speed_mps=(body.maximum_tcp_linear_speed_mps),
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
            )
            commit_draft_edit(
                state,
                body,
                operation_kind="mutate_deployment_motion_target_limit",
                session="deployment",
                install=lambda: state.deployment_draft_session.commit_compiled(
                    compiled,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                ),
            )
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/discard")
    def discard(body: DiscardDeploymentRequest) -> dict[str, Any]:
        try:
            discard_deployment_draft(state, body)
            # The Deployment workspace carries its own Draft-matched identity;
            # the composite snapshot must not replace it (it breaks that contract).
            return _workspace(state)
        except Exception as error:
            _raise(error)

    def revert_ports(body: DeploymentCandidateRequest) -> "HeadlessApplyPorts":
        """Revert on the one Apply path: the Saved Scenario is
        applied like any Deployment, and both Drafts return to Saved in the
        workspace commit right before the Manager submission."""

        if state.headless_client is None:
            _raise(
                DeploymentDraftError(
                    "manager_apply_unavailable",
                    "Deployment Revert requires the configured Manager",
                    status_code=503,
                )
            )
        retained = state.deployment_draft_session.retained_state()
        saved = retained["saved_document"]
        if not isinstance(saved, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_saved_unavailable",
                    "Deployment has no Saved baseline",
                    status_code=404,
                )
            )
        # Revert restores the whole Scenario to Saved: the Saved Program is
        # activated with the Saved Deployment and both Drafts return to Saved.
        program_retained = state.draft_session.retained_state()
        try:
            program_restore = saved_program_restore(
                state, program_retained, _active_catalog(state)
            )
        except Exception as error:
            _raise(error)
        saved_program = program_restore.saved_program
        program = saved_program.to_dict() if saved_program is not None else None
        source_document_digest = state.scenario_source_document_digest
        scenario_id = state.scenario_document_id
        project_path = state.scenario_project_path
        if (
            not isinstance(source_document_digest, str)
            or not isinstance(scenario_id, str)
            or not isinstance(project_path, str)
        ):
            _raise(
                DeploymentDraftError(
                    "activation_input_incomplete",
                    "Scenario Revert requires a complete saved Scenario input",
                    status_code=409,
                )
            )
        identity_at_request = state.authoring_workspace.identity()
        source_closure = _capture_model_sources(state, saved)
        scene_mesh_closure = _capture_scene_meshes(program, state, project_path)
        captured_input_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "scenario_id": scenario_id,
                    "project_path": project_path,
                    "source_document_digest": source_document_digest,
                    "deployment": saved,
                    "model_source_closure": source_closure,
                    "program": program,
                    "scene_mesh_closure": scene_mesh_closure,
                }
            )
        )
        current_bundle, current_graph = state._manager_current(
            state.headless_client.manager_status(), phase="deployment_revert.preview"
        )
        compiled_saved = state.deployment_draft_session.preview_source(
            json.dumps(saved, ensure_ascii=False, allow_nan=False)
        )
        base_intent = {
            "schema_version": "manyforge.composer_authoring_effect.v2",
            "scene_initialization": "preserve",
            "operation_kind": "revert_deployment",
            "effect_class": "external",
            "external_kind": "revert_deployment",
            "scenario_id": scenario_id,
            "project_path": project_path,
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "scene_mesh_closure": scene_mesh_closure,
            "program": deepcopy(program),
            "deployment": deepcopy(saved),
            "expected_manager_current": {
                "retained_bundle_id": current_bundle,
                "graph_revision": current_graph,
            },
            "draft_id": body.draft_id,
            "draft_revision": body.expected_draft_revision,
            "application_start_id": body.application_start_id,
            "request_key": new_request_key().hex(),
            "confirmation_token": None,
            "preparation_application_start_id": body.application_start_id,
            "preparation_request_key": body.request_key,
        }
        if program is None:
            base_intent["program_authoring"] = deepcopy(program_restore.saved_document)

        def revert_intent(generation: int) -> dict[str, Any]:
            return {**deepcopy(base_intent), "expected_admission_generation": generation}

        def prepare(intent: dict[str, Any]) -> dict[str, Any]:
            # The server confirms the impact itself: it previewed this exact
            # candidate (apply_orchestration) before disabling anything.
            return state.execute_authored_deployment_candidate(
                intent, None, apply=True, prepare_only=True, confirmed=True
            )

        current: dict[str, Any] = {}

        def validate(_claim: dict[str, Any] | None) -> dict[str, Any]:
            if state.deployment_draft_session.retained_state() != retained:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before Revert",
                    status_code=409,
                )
            if state.draft_session.retained_state() != program_retained:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Program Draft changed before Revert",
                    status_code=409,
                )
            if (
                _claim is not None
                and _capture_model_sources(state, saved) != source_closure
            ):
                raise DeploymentDraftError(
                    "activation_model_source_changed",
                    "A model source changed during activation capture",
                    status_code=409,
                )
            if _claim is None:
                current["prepared"] = prepare(current["intent"])
            return {"prepared_deployment_candidate": current["prepared"]}

        def effect(claim: dict[str, Any] | None) -> dict[str, Any]:
            return state.execute_authored_deployment_candidate(
                current["intent"],
                claim,
                apply=True,
                prepared=current["prepared"],
            )

        def commit_saved_before_submit() -> None:
            # Both Drafts return to Saved in one whole-workspace commit fenced
            # on the request identity: readers never see a mixed pair, and a
            # failed install contains the workspace (no rollback, no no-effect).
            def install() -> bool:
                deployment_changed = state.deployment_draft_session.commit_compiled(
                    compiled_saved,
                    draft_id=body.draft_id,
                    expected_revision=body.expected_draft_revision,
                )
                program_changed = state.draft_session.install_prepared_replacement(
                    program_restore.replacement,
                    expected_draft_id=identity_at_request.program_draft_id,
                    expected_revision=identity_at_request.program_draft_revision,
                )
                return deployment_changed or program_changed

            state.authoring_workspace.replace(
                expected_identity=identity_at_request,
                install=install,
                notify=False,
                context=AuthoringOperationContext(
                    operation="revert_deployment",
                    application_start_id=parse_application_start_id(
                        body.application_start_id
                    ),
                    request_key=bytes.fromhex(body.request_key),
                ),
            )

        class _RevertPorts(HeadlessApplyPorts):
            def preview(self) -> Preview:
                intent = revert_intent(body.expected_admission_generation)
                return candidate_preview(prepare(intent).get("candidate_preview"))

            def apply(self) -> None:
                # Disabling components may advance the admission generation:
                # prepare and claim under the current one.
                generation = self._client.authoring_admission().get("admission_generation")
                current["intent"] = revert_intent(generation)
                response = execute(
                    body.model_copy(update={"expected_admission_generation": generation}),
                    "revert_deployment",
                    prepare=lambda: current["intent"],
                    validate=validate,
                    commit_before_submit=commit_saved_before_submit,
                    effect=effect,
                    after_release=state.authoring_workspace.publish_commit_wake,
                )
                self.wait_for_manager_apply(response["result"])

        return _RevertPorts(state)

    @router.post("/api/deployment/revert/preview")
    def preview_revert(body: ServerApplyRequest) -> dict[str, Any]:
        """Per-component impact of reverting to the Saved Scenario."""
        try:
            scope = activation_scope(state)
            return preview_view(
                revert_ports(server_deployment_fence(state)).preview(), scope
            )
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/revert")
    def revert(body: ConfirmedApplyRequest) -> dict[str, Any]:
        """Apply of the Saved Scenario; both Drafts return to Saved. Answers
        at acceptance with its row (``manyforge.apply_command_reply.v1``);
        the row's end carries the ApplyResult's outcome. Bound to the Revert
        Preview's ``scope`` when given."""

        def run() -> dict[str, Any]:
            try:
                with apply_slot():
                    require_activation_scope(state, body.scope)
                    return orchestrated_apply(
                        revert_ports(server_deployment_fence(state)),
                        body.confirmed_collision_rule_ids,
                    )
            except Exception as error:
                _raise(error)

        return state.apply_commands.run(
            state.headless_client, "deployment_revert", body.request_key, run
        )

    def authored_candidate(
        body: DeploymentCandidateRequest,
        *,
        apply: bool,
        refuse_unchanged: bool = False,
    ) -> dict[str, Any]:
        """Preview, or confirmed Apply, of the current Draft through Manager.

        The Apply is confirmed by the server: it previewed this candidate
        itself (apply_orchestration) before disabling anything.
        ``refuse_unchanged`` only refuses an unchanged Draft: the Apply checks
        it before anything else, a finished run's Reset included.
        """
        operation_kind = (
            "apply_deployment_candidate"
            if apply
            else "preview_deployment_candidate"
        )
        if state.headless_client is None:
            _raise(
                DeploymentDraftError(
                    "deployment_candidate_unavailable",
                    "Deployment preview and Apply require the configured Manager",
                    status_code=503,
                )
            )

        def manager_current() -> tuple[str, str]:
            try:
                bundle, graph = state._manager_current(
                    state.headless_client.manager_status(), phase="deployment_candidate.preview"
                )
                return bundle, graph
            except DraftSessionError as error:
                raise DeploymentDraftError(
                    error.code, str(error), status_code=error.status_code
                ) from error

        state.deployment_draft_session.require_current(
            body.draft_id, body.expected_draft_revision
        )
        deployment_state = state.deployment_draft_session.retained_state()
        deployment = deployment_state.get("document")
        if not isinstance(deployment, dict):
            _raise(
                DeploymentDraftError(
                    "deployment_draft_unavailable",
                    "Open a Deployment Draft before previewing it",
                    status_code=404,
                )
            )
        program = state.draft_session.retained_state().get("program")
        source_document_digest = state.scenario_source_document_digest
        scenario_id = state.scenario_document_id
        project_path = state.scenario_project_path
        if (
            not isinstance(program, dict)
            or not isinstance(source_document_digest, str)
            or not isinstance(scenario_id, str)
            or not isinstance(project_path, str)
        ):
            _raise(
                DeploymentDraftError(
                    "activation_input_incomplete",
                    "Save or load one complete Scenario document with Program, "
                    "and Deployment before preparation",
                    status_code=409,
                )
            )
        deployment = with_defaults(deployment)
        source_closure = _capture_model_sources(state, deployment)
        scene_mesh_closure = _capture_scene_meshes(program, state, project_path)
        if apply or refuse_unchanged:
            # Nothing to apply: every input already is the Manager's Applied
            # activation. Refused before admission, so no effect is possible.
            # An Applied fact that cannot be read now never proves a no-op.
            try:
                publication = state.published_scenario().get("published_scenario")
            except HeadlessClientError:
                publication = None
            applied_context = (
                state.applied_activation_for_publication(publication)
                if isinstance(publication, dict)
                else None
            )
            program_revision = (
                program.get("program_revision") if isinstance(program, dict) else None
            )
            if (
                applied_context is not None
                and applied_context.get("deployment") == deployment
                and applied_context.get("model_source_closure") == source_closure
                and applied_context.get("scene_mesh_closure") == scene_mesh_closure
                and applied_context.get("source_document_digest") == source_document_digest
                and applied_context.get("scenario_id") == scenario_id
                and applied_context.get("scenario_project_path") == project_path
                and isinstance(program_revision, str)
                and _applied_program_revision(state) == program_revision
            ):
                raise refused_before_effect(
                    409,
                    {
                        "code": "deployment_apply_nothing_to_apply",
                        "message": (
                            "Nothing to apply: the Deployment and Program "
                            "Drafts already match Applied"
                        ),
                    },
                )
            if refuse_unchanged:
                return {}
        captured_input_digest = raw_sha256_digest(
            canonical_json_bytes(
                {
                    "scenario_id": scenario_id,
                    "project_path": project_path,
                    "source_document_digest": source_document_digest,
                    "deployment": deployment,
                    "model_source_closure": source_closure,
                    "program": program,
                    "scene_mesh_closure": scene_mesh_closure,
                }
            )
        )
        current_bundle, current_graph = manager_current()
        pre_state = state.authoring_retained_state()
        intent = {
            "schema_version": "manyforge.composer_authoring_effect.v2",
            "operation_kind": operation_kind,
            "effect_class": "external",
            "external_kind": (
                "deployment_candidate_apply"
                if apply
                else "deployment_candidate_preview"
            ),
            "scenario_id": scenario_id,
            "project_path": project_path,
            "source_document_digest": source_document_digest,
            "captured_input_digest": captured_input_digest,
            "model_source_closure": source_closure,
            "scene_mesh_closure": scene_mesh_closure,
            "deployment": deepcopy(deployment),
            "program": deepcopy(program),
            "expected_manager_current": {
                "retained_bundle_id": current_bundle,
                "graph_revision": current_graph,
            },
            "expected_admission_generation": body.expected_admission_generation,
            "scene_initialization": "preserve",
            "application_start_id": body.application_start_id,
            "request_key": new_request_key().hex(),
            "confirmation_token": None,
            "preparation_application_start_id": body.application_start_id,
            "preparation_request_key": body.request_key,
            "draft_id": body.draft_id,
            "draft_revision": body.expected_draft_revision,
            "post_state": pre_state,
        }

        prepared: dict[str, Any] = {}

        def validate(_claim: dict[str, Any] | None) -> dict[str, Any] | None:
            state.deployment_draft_session.require_current(
                body.draft_id, body.expected_draft_revision
            )
            if state.deployment_draft_session.retained_state() != deployment_state:
                raise DeploymentDraftError(
                    "deployment_draft_stale",
                    "Deployment Draft changed before candidate compilation",
                    status_code=409,
                )
            latest_bundle, latest_graph = manager_current()
            if (
                state.draft_session.retained_state().get("program") != program
                or state.scenario_source_document_digest != source_document_digest
                or (
                    _claim is not None
                    and _capture_model_sources(state, deployment) != source_closure
                )
                or (latest_bundle, latest_graph) != (current_bundle, current_graph)
            ):
                raise DeploymentDraftError(
                    "deployment_candidate_source_changed",
                    "Captured authoring input or Manager selection changed "
                    "before activation preparation",
                    status_code=409,
                )
            # Compile or resolve the confirmation exactly once before Manager
            # mutation. Manager durably owns the accepted operation; Composer
            # retains no external-effect replay journal.
            if _claim is not None:
                return {"prepared_deployment_candidate": prepared["candidate"]}
            try:
                prepared["candidate"] = (
                    state.execute_authored_deployment_candidate(
                        intent,
                        _claim,
                        apply=apply,
                        prepare_only=True,
                        confirmed=apply,
                    )
                )
            except AuthoringOperationError as error:
                if error.code not in {"confirmation_expired", "confirmation_stale"}:
                    raise
                raise HTTPException(
                    status_code=409,
                    detail={"code": error.code, "message": str(error)},
                ) from error
            except HeadlessClientError as error:
                raise headless_client_http_exception(error) from error
            return {"prepared_deployment_candidate": prepared["candidate"]}

        def effect(claim: dict[str, Any] | None) -> dict[str, Any]:
            try:
                result = state.execute_authored_deployment_candidate(
                    intent,
                    claim,
                    apply=apply,
                    prepared=prepared.get("candidate"),
                )
            except Exception as error:
                candidate = prepared.get("candidate")
                phases = (
                    candidate.get("phases_ms")
                    if isinstance(candidate, dict)
                    else None
                )
                manager_request = (
                    candidate.get("manager_apply")
                    if isinstance(candidate, dict)
                    else None
                )
                if not isinstance(phases, dict):
                    failure_phase = "preparation"
                elif (
                    manager_request is not None
                    and "manager_apply_ms" not in phases
                ):
                    failure_phase = "manager_submission"
                else:
                    failure_phase = "candidate_submission"
                _LOGGER.exception(
                    "deployment_candidate_activation_failed",
                    extra={
                        "application_start_id": body.application_start_id,
                        "request_key": body.request_key,
                        "draft_id": body.draft_id,
                        "draft_revision": body.expected_draft_revision,
                        "failure_phase": failure_phase,
                        "error_code": getattr(error, "code", None),
                        "phases_ms": phases,
                    },
                )
                raise
            return result

        if not apply:
            try:
                body.require_workspace_fence(state.authoring_workspace.identity())
                validate(None)
                result = effect(None)
                return {
                    "schemaVersion": "manyforge.composer_deployment_candidate_operation.v1",
                    "workspace": _workspace(state),
                    "result": result,
                }
            except Exception as error:
                _raise(error)

        return execute(
            body,
            intent["operation_kind"],
            prepare=lambda: intent,
            validate=validate,
            effect=effect,
        )

    def apply_ports(body: DeploymentCandidateRequest) -> "_DraftApplyPorts":
        return _DraftApplyPorts(state, body, authored_candidate)

    @router.post("/api/deployment/preview")
    def preview_deployment(body: ServerApplyRequest) -> dict[str, Any]:
        """Per-component impact of applying the current Draft,
        the Program's outcome, attachment changes and detached components, and
        the ``scope`` the Apply sends back."""
        try:
            scope = activation_scope(state)
            return preview_view(
                apply_ports(server_deployment_fence(state)).preview(), scope
            )
        except Exception as error:
            _raise(error)

    @router.post("/api/deployment/apply")
    def apply_deployment(body: ConfirmedApplyRequest) -> dict[str, Any]:
        """Orchestrated Apply of the current Draft. Answers at acceptance
        with its row (``manyforge.apply_command_reply.v1``); the row's end
        carries the ApplyResult's outcome. Bound to the Preview's ``scope``
        when given."""

        def run() -> dict[str, Any]:
            try:
                with apply_slot():
                    require_activation_scope(state, body.scope)
                    ports = apply_ports(server_deployment_fence(state))
                    # Nothing to apply is refused before any effect, a finished
                    # run's Reset included.
                    ports.refuse_unchanged()
                    return orchestrated_apply(ports, body.confirmed_collision_rule_ids)
            except Exception as error:
                _raise(error)

        return state.apply_commands.run(
            state.headless_client, "deployment_apply", body.request_key, run
        )

    @router.get("/api/deployment/effective")
    def effective_values() -> dict[str, Any]:
        """Every defaulted field's value, default, and whether it is set."""
        document = state.deployment_draft_session.retained_state().get("document")
        return effective_fields(document if isinstance(document, dict) else None)

    return router


def _error_text(error: Exception) -> tuple[str, str] | None:
    """The typed code and message of a step's failure."""

    if isinstance(error, HTTPException) and isinstance(error.detail, dict):
        return str(error.detail.get("code")), str(error.detail.get("message", ""))
    code = getattr(error, "code", None)
    if isinstance(code, str) and code:
        return code, str(error)
    return None


def manager_apply_failure(failure: dict[str, Any], terminal: str) -> ApplyStepFailed:
    """The innermost typed cause of a Manager Apply failure.

    The Manager retains its phase code with the cause's code and details; a
    host composition refusal carries its own typed cause (`cause_code`,
    `cause`, `component_id`) in those details. The deepest typed code wins,
    with its message and the component it names.
    """

    details = failure.get("cause_details")
    details = details if isinstance(details, dict) else {}
    before_effect = (
        terminal == "failed" and failure.get("effect_boundary") == "before_effect"
    )
    if details.get("cause_code"):
        return ApplyStepFailed(
            str(details["cause_code"]),
            str(details.get("cause") or failure.get("message") or ""),
            str(details["component_id"]) if details.get("component_id") else None,
            before_effect=before_effect,
        )
    return ApplyStepFailed(
        str(failure.get("cause_code") or failure.get("code") or f"manager_apply_{terminal}"),
        str(failure.get("message") or f"Manager Apply ended {terminal}"),
        str(details["component_id"]) if details.get("component_id") else None,
        before_effect=before_effect,
    )


def orchestrated_apply(
    ports: Any, confirmed_collision_rule_ids: list[str] | None = None
) -> dict[str, Any]:
    """Run one orchestrated Apply; a refusal before any effect is a 409.

    The caller holds the Apply slot (:func:`apply_slot`).
    ``confirmed_collision_rule_ids`` as in :func:`run_apply`.
    """

    try:
        return run_apply(
            ports,
            error_code=_error_text,
            confirmed_collision_rule_ids=confirmed_collision_rule_ids,
        )
    except ApplyRefused as refusal:
        raise refused_before_effect(
            409, {"code": refusal.code, "message": str(refusal)}
        ) from refusal


class HeadlessApplyPorts:
    """The component, Program-cycle and Manager-status reads of an Apply."""

    _POLL_SECONDS = 0.2

    def __init__(self, state: ComposerState) -> None:
        self._state = state

    @property
    def _client(self):
        if self._state.headless_client is None:
            raise ApplyRefused(
                "deployment_candidate_unavailable",
                "Deployment Apply requires the configured Manager",
            )
        return self._state.headless_client

    def cycle_state(self) -> str | None:
        return self._client.program_cycle_status().get("state")

    def run_held(self) -> bool:
        # The one held-run rule (Headless ``held_run_authoring``).
        return held_run_draft_edit_allowed(
            self._client.program_cycle_status(),
            self._client.execution_control_state(),
        )

    def manual_operations_settled(self) -> bool:
        try:
            response = self._client.execution_control_state()
        except HeadlessClientError as error:
            # No physical product is installed yet (before the first Apply):
            # no direct-control target exists, so no manual operation does.
            if error.code == "execution_owner_not_ready":
                return True
            raise
        control = response.get("control_state")
        manual = control.get("manual_operations") if isinstance(control, dict) else None
        # Unknown is not settled, as in the held-run rule.
        return isinstance(manual, list) and all(
            manual_operation_settled(row) for row in manual
        )

    def reset_cycle(self) -> None:
        """Reset the finished or held run (as the Program tab's Reset, no
        options) and return once that Reset ended ``succeeded``.

        Headless answers the Reset at acceptance with the runner's row; the
        row is followed in the cycle status (``runner_commands``) to its end,
        with no limit (OD-214): any other end fails the Apply step with the
        row's code. The Runner's Idle reaches the Kernel admission through
        the Manager's readiness update, a moment later; the Apply's
        preparation waits for that admission itself (Headless
        ``prepare_activation``).
        """

        cycle = self._client.program_cycle_status()
        # A server-originated Reset: Composer's own `A` and a fresh key
        # (nothing resends it).
        reply = self._client.program_cycle_reset(
            {
                "request_key": new_request_key().hex(),
                "application_start_id": format_application_start_id(
                    self._state.application_start_id
                ),
                "source_label": "composer_apply",
                "program_run_id": cycle.get("program_run_id"),
                "expected_cycle_generation": cycle.get("cycle_generation"),
                "expected_claim_generation": cycle.get("cycle_claim_generation"),
                "terminal_reference": cycle.get("terminal_or_settlement_reference"),
                "settlement_evidence_digest": cycle.get(
                    "terminal_or_settlement_reference"
                ),
                "reload_initial_scene": False,
                "reset_robot_position": False,
            }
        )
        row = reply.get("command")
        if (
            reply.get("schema_version") != "manyforge.program_command_reply.v1"
            or not isinstance(row, dict)
            or row.get("kind") != "reset"
            or not isinstance(row.get("request_key"), str)
        ):
            raise ApplyStepFailed(
                "program_runner_response_invalid",
                "The Program Reset before the Apply returned an invalid reply",
            )
        # Headless forms the key the runner's row carries.
        key = row["request_key"]
        apply_step_source(
            "resetting_cycle",
            {"status": "runner_command", "kind": "reset", "request_key": key},
        )
        while row.get("end") is None:
            time.sleep(self._POLL_SECONDS)
            commands = self._client.program_cycle_status().get("runner_commands")
            commands = commands if isinstance(commands, dict) else {}
            current = next(
                (
                    candidate
                    for candidate in (
                        commands.get("in_progress"),
                        *(commands.get("ended") or ()),
                    )
                    if isinstance(candidate, dict)
                    and candidate.get("request_key") == key
                ),
                None,
            )
            if current is None:
                raise ApplyStepFailed(
                    "program_runner_response_invalid",
                    "The Program cycle status omitted the Reset before the Apply",
                )
            row = current
        end = row["end"]
        if end.get("outcome") != "succeeded":
            code = end.get("code")
            raise ApplyStepFailed(
                code if isinstance(code, str) and code else "program_cycle_reset_failed",
                "The Program Reset before the Apply ended "
                f"{end.get('outcome')}",
            )

    def components(self) -> list[dict[str, Any]]:
        return list(self._client.components())

    def component_action(self, component_id: str, action: str) -> None:
        self._client.component_action(component_id, action)

    def wait_for_manager_apply(self, result: dict[str, Any]) -> None:
        """Wait for an accepted Manager Apply to be terminal.

        No limit of its own (OD-214): an Apply step ends only on its own
        events, and the Manager's stop or the execution host's exit ends the
        application (OD-270); a status read that fails ends the wait with its
        error.
        """

        if result.get("status") != "accepted":
            # `applied`: the exact configuration already is active.
            return
        apply_id = result["apply_id"]
        generation = result["apply_generation"]
        apply_step_source(
            "applying",
            {
                "status": "manager_apply",
                "apply_id": apply_id,
                "apply_generation": generation,
            },
        )
        while True:
            status = self._client.manager_apply_status(
                apply_id,
                generation,
                application_start_id=format_application_start_id(
                    self._state.application_start_id
                ),
            )
            terminal = status.get("state") if status.get("ok") is True else None
            if terminal == "succeeded":
                return
            if terminal in ("failed", "unknown_effect"):
                if terminal == "unknown_effect":
                    apply_effect_unknown()
                # A terminal status carries the Manager's retained `failure`.
                error = status.get("failure") if isinstance(status.get("failure"), dict) else {}
                raise manager_apply_failure(error, terminal)
            time.sleep(self._POLL_SECONDS)

    def wait_for_scene_clear(self, result: dict[str, Any]) -> None:
        """Wait for Headless's Scene clear of a committed Apply without a
        Program (New, and the New inside Load).

        Once such an Apply is committed, Headless clears the Scene through
        owner transactions, which hold the cell's authoring admission; the
        Apply's result returns once that clear has ended, so the next Load or
        Preview is admitted. A clear that ended `failed`, `partial` or
        `unknown` fails the Apply with its typed reason (a Load stops at its
        New); the objects left are in the cycle status. Headless publishes an end for every such Apply (OD-239), so
        the wait has no time limit. It ends at this Apply's end; at a status
        naming a later Apply, or naming no Apply (a direct clear), either of
        which replaced this one's (a direct clear may also have run before
        this Apply's clear began: the Apply then goes on without it); or,
        logged, when the cycle status can't be read.
        """

        manager_apply = result.get("manager_apply")
        if (
            result.get("status") != "accepted"
            or not isinstance(manager_apply, dict)
            or manager_apply.get("replay_status") != "accepted_new"
        ):
            # Headless clears the Scene only for a newly accepted Apply.
            return
        identity = (result["apply_id"], result["apply_generation"])
        apply_step(
            "clearing_scene",
            {
                "status": "scene_clear",
                "apply_id": identity[0],
                "apply_generation": identity[1],
            },
        )
        while True:
            try:
                clear = self._client.program_cycle_status().get("scene_clear")
            except HeadlessClientError as error:
                logging.getLogger(__name__).warning(
                    "Scene clear after Apply %s/%s not observed: %s",
                    *identity, error.code,
                )
                return
            if (
                isinstance(clear, dict)
                and (clear.get("apply_id"), clear.get("apply_generation")) == identity
                and clear.get("state") != "pending"
            ):
                reason = clear.get("reason")
                details = reason.get("details") if isinstance(reason, dict) else None
                if clear.get("state") == "unknown" and isinstance(details, dict):
                    cause = details.get("cause_code")
                    if isinstance(cause, str) and cause:
                        logging.getLogger(__name__).warning(
                            "Scene clear after Apply %s/%s outcome unknown: cause_code=%s cause_details=%s",
                            *identity, cause, details.get("cause_details"),
                        )
                if clear.get("state") in ("failed", "partial", "unknown"):
                    raise ApplyStepFailed(reason["code"], reason["message"])
                return
            if isinstance(clear, dict):
                named = clear.get("apply_id")
                if named is None or (type(named) is int and named > identity[0]):
                    # Another status replaced this one's (OD-239): a later
                    # Apply's (the Manager numbers Applies in increasing
                    # order within an application start) or a direct
                    # clear's, which names no Apply.
                    return
            time.sleep(self._POLL_SECONDS)


class _DraftApplyPorts(HeadlessApplyPorts):
    """Apply of the current Deployment Draft (`POST /api/deployment/apply`)."""

    def __init__(
        self,
        state: ComposerState,
        body: DeploymentCandidateRequest,
        authored_candidate: Callable[..., dict[str, Any]],
    ) -> None:
        super().__init__(state)
        self._body = body
        self._authored_candidate = authored_candidate

    def refuse_unchanged(self) -> None:
        """Refuse a Draft that already matches Applied (409
        ``deployment_apply_nothing_to_apply``); no effect."""
        self._authored_candidate(self._body, apply=False, refuse_unchanged=True)

    def preview(self) -> Preview:
        # The Apply's Preview runs after the run's Reset, before any disable
        # (run_apply).
        response = self._authored_candidate(self._body, apply=False)
        return candidate_preview(response["result"].get("candidate_preview"))

    def apply(self) -> None:
        # Disabling components may advance the authoring admission generation;
        # the server's own Apply uses the current one.
        generation = self._client.authoring_admission().get("admission_generation")
        body = self._body.model_copy(update={"expected_admission_generation": generation})
        response = self._authored_candidate(body, apply=True)
        self.wait_for_manager_apply(response["result"])
