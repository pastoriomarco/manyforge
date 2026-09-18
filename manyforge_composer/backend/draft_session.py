"""Bounded in-memory Composer Draft owner with no runtime or file authority."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import logging
import math
from pathlib import Path
import re
import threading
import uuid
from typing import Any, Callable, NoReturn, TypeVar

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
)
from manyforge_behavior.geometry import (
    GeometryError,
    add_rpy_deg,
    compose_local,
    compose_world,
    normalize_quat,
    rpy_deg_to_xyzw,
)
from manyforge_behavior.program import (
    Program,
    ProgramValidationError,
    merge_param_patch,
    parse_execution_groups,
)
from manyforge_behavior.pick_place_templates import (
    PICK_PLACE_RESULT_FIELDS,
    PickPlaceTemplateError,
    build_pick_place_subtree,
)

from .authoring_forest import (
    AUTHORING_FOREST_VERSION,
    AuthoringForestError,
    derive_candidate,
    empty_authoring,
    from_program,
)


_DRAFT_API_VERSION = "manyforge.composer_draft.v2"
_WORKSPACE_API_VERSION = "manyforge.composer_program_workspace.v2"
_RETAINED_STATE_API_VERSION = "manyforge.composer_retained_draft.v2"
_MAXIMUM_DRAFT_REVISION = 9_007_199_254_740_991
_ROOT_ALIAS = "@root"
SUPPORTED_DRAFT_MUTATIONS = (
    "blackboard_draft_remove_keys",
    "blackboard_draft_remove_namespaces",
    "blackboard_draft_upsert_keys",
    "blackboard_draft_upsert_namespaces",
    "program_draft_remove_parameters",
    "program_draft_upsert_parameters",
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
    "tree_draft_update_node_params",
    "tree_draft_wrap_node",
)
OPERATOR_DRAFT_MUTATIONS = (
    "assistant_draft_materialize_proposal",
    "program_draft_expand_authoring_macro",
    "program_draft_remove_contact_policies",
    "program_draft_replace_roles",
    "program_draft_update_motion_defaults",
    "program_draft_replace_support_contacts",
    "program_draft_store_observed_move",
    "program_draft_store_observed_scene_resource",
    "program_draft_upsert_contact_policies",
    "program_draft_update_settings",
    "program_draft_update_node_policies",
    "viewer_draft_replace",
)
_NODE_POLICY_MUTATION = "program_draft_update_node_policies"
_PROGRAM_MOTION_DEFAULTS = (
    "velocity_scale",
    "acceleration_scale",
    "translation_tol_m",
    "max_blend_deviation_rad",
)
_PROGRAM_MOTION_CEILINGS = frozenset(
    {"velocity_scale", "acceleration_scale"}
)
_ASSISTANT_PROPOSAL_MUTATIONS = frozenset(
    {
        "scene_draft_upsert_objects",
        "tree_draft_insert_node",
    }
)
_PORTABLE_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_.-]*$")
_PORTABLE_ID_MAX_BYTES = 128
_T = TypeVar("_T")
_LOGGER = logging.getLogger(__name__)


def _is_portable_id(value: Any) -> bool:
    return (
        type(value) is str
        and _PORTABLE_ID_PATTERN.fullmatch(value) is not None
        and len(value.encode("utf-8")) <= _PORTABLE_ID_MAX_BYTES
    )


_SKILL_REFERENT_UNAVAILABLE_MESSAGE = (
    "Allowed recovery skills are unavailable: each skill requires an exact "
    "revision-qualified retained referent. Leave Allowed skills empty."
)
_MODIFICATION_POLICY_FIELDS = frozenset(
    {"runtime_override", "persistent_edit", "intervention_delegate"}
)
_RECOVERY_POLICY_FIELDS = frozenset(
    {
        "allow_intervention",
        "allowed_recovery_actions",
        "max_retries",
        "allowed_skills",
        "persistence_ceiling",
    }
)
_MODIFICATION_POLICY_VALUES = {
    "runtime_override": frozenset({"forbidden", "human_approval", "llm_allowed"}),
    "persistent_edit": frozenset({"forbidden", "human_approval", "llm_allowed"}),
    "intervention_delegate": frozenset(
        {"forbidden", "human_only", "llm_allowed", "program_only"}
    ),
}
_RECOVERY_ACTIONS = frozenset({"intervention", "runtime_override"})
_PERSISTENCE_CEILINGS = frozenset(
    {"session_only", "runtime_override", "draft_patch"}
)
_OPERATOR_STOP_POLICIES = frozenset({"inherit", "hold", "continue"})
_DEFAULT_MODIFICATION_POLICY = {
    "runtime_override": "forbidden",
    "persistent_edit": "forbidden",
    "intervention_delegate": "forbidden",
}
_DEFAULT_RECOVERY_POLICY = {
    "allow_intervention": False,
    "allowed_recovery_actions": [],
    "max_retries": 0,
    "allowed_skills": [],
    "persistence_ceiling": "session_only",
}


def draft_applicability(document: dict[str, Any]) -> dict[str, Any]:
    """Return the one Draft-wide C1 applicability projection.

    Canonical Program parsing deliberately accepts retained skill IDs.  C1
    cannot resolve those IDs, so this separate projection is the boundary
    consumed by Apply and the workspace editor.
    """

    affected_instance_ids: list[str] = []
    pending: list[Any] = []
    tree = document.get("tree")
    if type(tree) is dict:
        pending.append(tree)
    while pending:
        node = pending.pop()
        recovery = node.get("recovery_policy")
        skills = recovery.get("allowed_skills") if type(recovery) is dict else None
        if type(skills) is list and skills:
            instance_id = node.get("instance_id")
            if type(instance_id) is str:
                affected_instance_ids.append(instance_id)
        children = node.get("children")
        if type(children) is list:
            pending.extend(
                child for child in reversed(children) if type(child) is dict
            )
    unavailable_reason = (
        {
            "code": "composer_policy_skill_referent_unavailable",
            "message": _SKILL_REFERENT_UNAVAILABLE_MESSAGE,
            "restorationGates": [],
        }
        if affected_instance_ids
        else None
    )
    return {
        "applyReady": not affected_instance_ids,
        "affectedInstanceIds": affected_instance_ids,
        "unavailableReason": unavailable_reason,
    }


class DraftSessionError(RuntimeError):
    """One Draft operation failed before an atomic state change."""

    def __init__(
        self,
        code: str,
        message: str,
        *,
        status_code: int = 400,
        detail: Any | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code
        self.detail = message if detail is None else detail


@dataclass(frozen=True, slots=True)
class DraftMutationResult:
    mutation: str
    details: dict[str, Any]
    snapshot: dict[str, Any]


def _error(
    code: str,
    message: str,
    *,
    status_code: int = 400,
    detail: Any | None = None,
) -> NoReturn:
    raise DraftSessionError(
        code,
        message,
        status_code=status_code,
        detail=detail,
    )


def _tree_ref(
    roots: list[dict[str, Any]],
    instance_id: str,
) -> tuple[dict[str, Any], dict[str, Any] | None, int | None] | None:
    target = (
        roots[0].get("instance_id")
        if instance_id == _ROOT_ALIAS and len(roots) == 1
        else instance_id
    )
    pending: list[tuple[dict[str, Any], dict[str, Any] | None, int | None]] = [
        (root, None, index) for index, root in reversed(list(enumerate(roots)))
    ]
    while pending:
        node, parent, index = pending.pop()
        if node.get("instance_id") == target:
            return node, parent, index
        children = node.get("children")
        if type(children) is list:
            for child_index in range(len(children) - 1, -1, -1):
                child = children[child_index]
                if type(child) is dict:
                    pending.append((child, node, child_index))
    return None


def _tree_ids(roots: list[dict[str, Any]]) -> list[str]:
    result: list[str] = []
    pending = list(reversed(roots))
    while pending:
        node = pending.pop()
        instance_id = node.get("instance_id")
        if type(instance_id) is str:
            result.append(instance_id)
        children = node.get("children")
        if type(children) is list:
            pending.extend(
                child for child in reversed(children) if type(child) is dict
            )
    return result


def _lookup(
    tree: list[dict[str, Any]],
    instance_id: str,
    *,
    label: str,
) -> tuple[dict[str, Any], dict[str, Any] | None, int | None]:
    found = _tree_ref(tree, instance_id)
    if found is None:
        _error(
            "draft_node_not_found",
            f"{label} does not name a Draft node: {instance_id}",
            status_code=404,
            detail={
                "message": f"{label} does not name a Draft node: {instance_id}",
                "validNodeNames": _tree_ids(tree),
                "rootAlias": _ROOT_ALIAS,
            },
        )
    return found


def _lookup_exact_instance(
    tree: list[dict[str, Any]],
    instance_id: str,
) -> dict[str, Any]:
    """Resolve one authored instance, without accepting the root alias."""

    if (
        type(instance_id) is not str
        or instance_id == _ROOT_ALIAS
        or _PORTABLE_ID_PATTERN.fullmatch(instance_id) is None
        or len(instance_id.encode("utf-8")) > _PORTABLE_ID_MAX_BYTES
    ):
        _error(
            "draft_arguments_invalid",
            "instance_id must be a portable lower-case ID of at most 128 bytes",
            status_code=409,
        )
    matches: list[dict[str, Any]] = []
    pending = list(reversed(tree))
    while pending:
        node = pending.pop()
        if node.get("instance_id") == instance_id:
            matches.append(node)
        children = node.get("children")
        if type(children) is list:
            pending.extend(
                child for child in reversed(children) if type(child) is dict
            )
    if not matches:
        _error(
            "draft_node_not_found",
            f"instance_id does not name a Draft node: {instance_id}",
            status_code=404,
            detail={
                "message": f"instance_id does not name a Draft node: {instance_id}",
                "validNodeNames": _tree_ids(tree),
            },
        )
    if len(matches) != 1:
        _error(
            "draft_node_duplicate",
            f"instance_id names more than one Draft node: {instance_id}",
        )
    return matches[0]


def _node_policy_arguments(
    arguments: dict[str, Any],
) -> tuple[str, dict[str, str], dict[str, Any], str]:
    expected_fields = {
        "instance_id",
        "modification_policy",
        "recovery_policy",
        "operator_stop_policy",
    }
    if set(arguments) - {"execution_groups"} != expected_fields:
        _error(
            "draft_arguments_invalid",
            f"{_NODE_POLICY_MUTATION} requires exactly "
            f"{sorted(expected_fields)}, optionally execution_groups",
        )

    instance_id = arguments["instance_id"]
    if (
        type(instance_id) is not str
        or instance_id == _ROOT_ALIAS
        or _PORTABLE_ID_PATTERN.fullmatch(instance_id) is None
        or len(instance_id.encode("utf-8")) > _PORTABLE_ID_MAX_BYTES
    ):
        _error(
            "draft_arguments_invalid",
            "instance_id must be a portable lower-case ID of at most 128 bytes",
            status_code=409,
        )

    modification = arguments["modification_policy"]
    if (
        type(modification) is not dict
        or set(modification) != _MODIFICATION_POLICY_FIELDS
    ):
        _error(
            "draft_arguments_invalid",
            "modification_policy must contain exactly its canonical fields",
        )
    for field, allowed in _MODIFICATION_POLICY_VALUES.items():
        value = modification[field]
        if type(value) is not str or value not in allowed:
            _error(
                "draft_arguments_invalid",
                f"modification_policy.{field} has an unsupported value",
            )

    recovery = arguments["recovery_policy"]
    if type(recovery) is not dict or set(recovery) != _RECOVERY_POLICY_FIELDS:
        _error(
            "draft_arguments_invalid",
            "recovery_policy must contain exactly its canonical fields",
        )
    if type(recovery["allow_intervention"]) is not bool:
        _error(
            "draft_arguments_invalid",
            "recovery_policy.allow_intervention must be boolean",
        )
    actions = recovery["allowed_recovery_actions"]
    if (
        type(actions) is not list
        or len(actions) > len(_RECOVERY_ACTIONS)
        or any(
            type(action) is not str or action not in _RECOVERY_ACTIONS
            for action in actions
        )
        or len(actions) != len(set(actions))
        or actions != sorted(actions)
    ):
        _error(
            "draft_arguments_invalid",
            "recovery_policy.allowed_recovery_actions is invalid",
        )
    retries = recovery["max_retries"]
    if type(retries) is not int or not 0 <= retries <= 255:
        _error(
            "draft_arguments_invalid",
            "recovery_policy.max_retries must be an integer in 0..255",
        )
    skills = recovery["allowed_skills"]
    if type(skills) is list and skills:
        _error(
            "composer_policy_skill_referent_unavailable",
            _SKILL_REFERENT_UNAVAILABLE_MESSAGE,
            status_code=409,
        )
    if type(skills) is not list or len(skills) > 256:
        _error(
            "draft_arguments_invalid",
            "recovery_policy.allowed_skills is invalid",
        )
    for skill in skills:
        if (
            type(skill) is not str
            or re.fullmatch(r"^[a-z][a-z0-9_.-]*$", skill) is None
            or len(skill.encode("ascii", errors="ignore")) != len(skill)
            or len(skill.encode("ascii", errors="ignore")) > 128
        ):
            _error(
                "draft_arguments_invalid",
                "recovery_policy.allowed_skills is invalid",
            )
    if skills != sorted(skills) or len(skills) != len(set(skills)):
        _error(
            "draft_arguments_invalid",
            "recovery_policy.allowed_skills is not canonical-sorted",
        )
    persistence = recovery["persistence_ceiling"]
    if (
        type(persistence) is not str
        or persistence not in _PERSISTENCE_CEILINGS
    ):
        _error(
            "draft_arguments_invalid",
            "recovery_policy.persistence_ceiling is invalid",
        )

    action_set = set(actions)
    if recovery["allow_intervention"] != ("intervention" in action_set):
        _error(
            "composer_draft_validation_failed",
            "recovery_policy.allow_intervention must match the intervention action",
        )
    if "runtime_override" in action_set and modification["runtime_override"] == "forbidden":
        _error(
            "composer_draft_validation_failed",
            "runtime_override recovery requires permitted runtime override modification",
        )
    if persistence == "runtime_override" and modification["runtime_override"] == "forbidden":
        _error(
            "composer_draft_validation_failed",
            "runtime_override persistence requires permitted runtime override modification",
        )
    if persistence == "draft_patch" and modification["persistent_edit"] == "forbidden":
        _error(
            "composer_draft_validation_failed",
            "draft_patch persistence requires permitted persistent edit modification",
        )
    if recovery != _DEFAULT_RECOVERY_POLICY and not action_set:
        _error(
            "composer_draft_validation_failed",
            "non-default recovery policy requires at least one recovery action",
        )

    operator_stop = arguments["operator_stop_policy"]
    if (
        type(operator_stop) is not str
        or operator_stop not in _OPERATOR_STOP_POLICIES
    ):
        _error(
            "draft_arguments_invalid",
            "operator_stop_policy must be inherit, hold, or continue",
        )
    return (
        instance_id,
        deepcopy(modification),
        deepcopy(recovery),
        operator_stop,
    )


def _insert_index(arguments: dict[str, Any], child_count: int) -> int:
    index = arguments.get("index", child_count)
    if type(index) is not int or index < 0 or index > child_count:
        _error(
            "draft_insert_index_invalid",
            f"index must be between 0 and {child_count}",
        )
    return index


def _contains_instance(root: dict[str, Any], instance_id: str) -> bool:
    return _tree_ref([root], instance_id) is not None


def _absolute_orientation(pose: dict[str, Any]) -> list[float]:
    orientation = pose.get("orientation")
    if type(orientation) is not dict:
        _error("draft_pose_invalid", "Draft pose has no absolute orientation")
    try:
        if "xyzw" in orientation:
            return normalize_quat(orientation["xyzw"])
        if "rpy_deg" in orientation:
            return rpy_deg_to_xyzw(orientation["rpy_deg"])
    except GeometryError as error:
        _error("draft_pose_invalid", f"Draft pose orientation is invalid: {error}")
    _error("draft_pose_invalid", "Draft pose has no supported absolute orientation")


def _scene_orientation(
    scene: list[dict[str, Any]],
    scene_resource_id: str,
) -> list[float]:
    for resource in scene:
        if resource.get("scene_resource_id") == scene_resource_id:
            return _absolute_orientation(resource.get("pose", {}))
    _error(
        "draft_scene_resource_not_found",
        f"orientation source does not name a Draft scene resource: {scene_resource_id}",
        status_code=404,
    )


def _resolve_relative_orientation(
    resource: dict[str, Any],
    *,
    scene: list[dict[str, Any]],
    current: dict[str, Any] | None,
) -> dict[str, Any]:
    candidate = deepcopy(resource)
    pose = candidate.get("pose")
    if type(pose) is not dict:
        return candidate
    intent_fields = (
        "rotate_local_rpy_deg",
        "rotate_world_rpy_deg",
        "rpy_delta_deg",
        "orientation_from",
    )
    present = [field for field in intent_fields if field in pose]
    if not present:
        return candidate
    if len(present) != 1 or "orientation" in pose:
        _error(
            "draft_pose_intent_invalid",
            "pose must contain exactly one absolute or relative orientation form",
        )
    current_orientation = (
        _absolute_orientation(current.get("pose", {}))
        if current is not None
        else None
    )
    field = present[0]
    try:
        if field == "rotate_local_rpy_deg":
            if current_orientation is None:
                _error(
                    "draft_pose_reference_unavailable",
                    "local rotation requires an existing Draft orientation",
                )
            resolved = compose_local(
                current_orientation,
                rpy_deg_to_xyzw(pose[field]),
            )
        elif field == "rotate_world_rpy_deg":
            if current_orientation is None:
                _error(
                    "draft_pose_reference_unavailable",
                    "world rotation requires an existing Draft orientation",
                )
            resolved = compose_world(
                current_orientation,
                rpy_deg_to_xyzw(pose[field]),
            )
        elif field == "rpy_delta_deg":
            if current_orientation is None:
                _error(
                    "draft_pose_reference_unavailable",
                    "RPY delta requires an existing Draft orientation",
                )
            resolved = add_rpy_deg(current_orientation, pose[field])
        else:
            reference = pose[field]
            if type(reference) is not dict:
                _error(
                    "draft_pose_reference_invalid",
                    "orientation_from must be an object",
                )
            source = reference.get("source")
            if source == "current_pose":
                if current_orientation is None:
                    _error(
                        "draft_pose_reference_unavailable",
                        "current-pose orientation requires an existing Draft resource",
                    )
                resolved = current_orientation
            elif source == "scene_resource":
                resolved = _scene_orientation(
                    scene,
                    str(reference.get("scene_resource_id", "")),
                )
            elif source == "target_frame":
                _error(
                    "draft_pose_reference_unavailable",
                    "target-frame orientation is not present in the immutable Draft; "
                    "use an absolute orientation or a scene-resource reference",
                )
            else:
                _error(
                    "draft_pose_reference_invalid",
                    "orientation_from source is unsupported",
                )
    except GeometryError as error:
        _error("draft_pose_intent_invalid", f"relative orientation is invalid: {error}")
    next_pose = deepcopy(pose)
    for intent in intent_fields:
        next_pose.pop(intent, None)
    next_pose["orientation"] = {"xyzw": resolved}
    candidate["pose"] = next_pose
    return candidate


def _pose_authoring_parameter_ids(
    catalog: NodeCatalog,
    node_id: str,
) -> frozenset[str]:
    authoring = catalog.schema_registry.types.get("pose_authoring_input")
    if authoring is None or authoring.resolves_to is None:
        _error(
            "composer_draft_catalog_invalid",
            "active catalog has no pose-authoring resolution contract",
            status_code=503,
        )
    try:
        entry = catalog.get(node_id)
    except KeyError:
        _error(
            "draft_node_kind_unknown",
            f"node_id is absent from the active catalog: {node_id}",
            status_code=404,
        )
    return frozenset(
        parameter.id
        for parameter in entry.parameters
        if parameter.schema.ref == authoring.resolves_to
    )


def _merge_pose_authoring_patch(
    existing: Any,
    incoming: dict[str, Any],
    *,
    merge: bool,
    scene: list[dict[str, Any]],
) -> dict[str, Any]:
    old_pose = deepcopy(existing) if type(existing) is dict else {}
    candidate = (
        merge_param_patch(old_pose, incoming)
        if merge
        else deepcopy(incoming)
    )
    intent_fields = (
        "rotate_local_rpy_deg",
        "rotate_world_rpy_deg",
        "rpy_delta_deg",
        "orientation_from",
    )
    if any(field in incoming for field in intent_fields):
        # The old absolute orientation is the reference, not a second intent.
        # Preserve a caller-supplied absolute orientation so the shared
        # resolver rejects an ambiguous absolute+relative update.
        if "orientation" not in incoming:
            candidate.pop("orientation", None)
        resolved = _resolve_relative_orientation(
            {"pose": candidate},
            scene=scene,
            current={"pose": old_pose} if old_pose else None,
        )
        return resolved["pose"]
    if "orientation" in incoming:
        # A nested mapping merge would otherwise retain the old `xyzw` beside
        # a replacement `rpy_deg` (or vice versa), violating the one-form Pose
        # contract.
        candidate["orientation"] = deepcopy(incoming["orientation"])
    return candidate


class ComposerDraftSession:
    """Own one retained authoring forest and its optional strict Program."""

    def __init__(
        self,
        program: Program | None = None,
        *,
        source_path: str | None = None,
    ) -> None:
        if source_path is not None and program is None:
            raise ValueError("source_path requires a Program Draft")
        resolved_source = (
            str(Path(source_path).resolve()) if source_path is not None else None
        )
        self._lock = threading.RLock()
        self._draft_id = f"draft-{uuid.uuid4().hex}"
        self._authoring = from_program(program) if program is not None else None
        self._program = program
        self._diagnostics: list[dict[str, str]] = []
        self._revision = 1 if program is not None else 0
        self._saved_path = resolved_source
        self._saved_program_revision = (
            program.program_revision if resolved_source is not None else None
        )
        self._saved_document = (
            program.to_dict() if resolved_source is not None else None
        )
        self._identity_change_observer: Callable[[], None] | None = None

    def set_identity_change_observer(
        self, observer: Callable[[], None] | None
    ) -> None:
        """Install the one non-authoritative presentation wake callback."""

        if observer is not None and not callable(observer):
            raise TypeError("Draft identity observer must be callable")
        with self._lock:
            self._identity_change_observer = observer

    def _notify_identity_change_locked(self) -> None:
        observer = self._identity_change_observer
        if observer is None:
            return
        try:
            observer()
        except Exception:
            # A presentation wake cannot roll back or relabel an already
            # committed authoring-owner mutation.
            _LOGGER.exception("Composer Draft identity wake failed")

    @property
    def draft_id(self) -> str:
        return self._draft_id

    @property
    def revision(self) -> int:
        with self._lock:
            return self._revision

    @property
    def program_loaded(self) -> bool:
        with self._lock:
            return self._authoring is not None

    def require_identity(self, draft_id: str, expected_revision: int) -> None:
        """Require one exact Program Draft identity without changing it."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )

    def retained_state(self) -> dict[str, Any]:
        """Return the complete bounded local owner state needed after restart."""

        with self._lock:
            return {
                "schema_version": _RETAINED_STATE_API_VERSION,
                "draft_id": self._draft_id,
                "draft_revision": self._revision,
                "authoring": deepcopy(self._authoring),
                "program": (
                    self._program.to_dict() if self._program is not None else None
                ),
                "diagnostics": deepcopy(self._diagnostics),
                "saved_path": self._saved_path,
                "saved_program_revision": self._saved_program_revision,
                "saved_document": deepcopy(self._saved_document),
            }

    def restore_retained_state(
        self, state: dict[str, Any], catalog: NodeCatalog
    ) -> None:
        """Validate and atomically restore one journal-owned local post-image."""

        expected_fields = {
            "schema_version",
            "draft_id",
            "draft_revision",
            "authoring",
            "program",
            "diagnostics",
            "saved_path",
            "saved_program_revision",
            "saved_document",
        }
        if type(state) is not dict or set(state) != expected_fields:
            _error(
                "composer_retained_draft_invalid",
                "retained Composer Draft has an invalid shape",
                status_code=503,
            )
        draft_id = state["draft_id"]
        revision = state["draft_revision"]
        authoring = state["authoring"]
        document = state["program"]
        diagnostics = state["diagnostics"]
        saved_path = state["saved_path"]
        saved_revision = state["saved_program_revision"]
        saved_document = state["saved_document"]
        if (
            state["schema_version"] != _RETAINED_STATE_API_VERSION
            or type(draft_id) is not str
            or not draft_id
            or type(revision) is not int
            or not 0 <= revision <= _MAXIMUM_DRAFT_REVISION
            or (authoring is None) != (revision == 0)
            or type(diagnostics) is not list
            or (saved_path is None)
            != (saved_revision is None or saved_document is None)
            or (saved_revision is None) != (saved_document is None)
        ):
            _error(
                "composer_retained_draft_invalid",
                "retained Composer Draft identities are inconsistent",
                status_code=503,
            )
        program: Program | None = None
        normalized_diagnostics: list[dict[str, str]] = []
        if authoring is not None:
            try:
                program, normalized_diagnostics = derive_candidate(
                    deepcopy(authoring), catalog
                )
            except AuthoringForestError as error:
                _error(
                    "composer_retained_draft_invalid",
                    f"retained Composer authoring forest is invalid: {error}",
                    status_code=503,
                )
            if (
                (program.to_dict() if program is not None else None) != document
                or normalized_diagnostics != diagnostics
            ):
                _error(
                    "composer_retained_draft_invalid",
                    "retained Composer candidate or diagnostics are inconsistent",
                    status_code=503,
                )
        elif document is not None or diagnostics:
            _error(
                "composer_retained_draft_invalid",
                "empty retained Composer Draft has candidate state",
                status_code=503,
            )
        resolved_saved_path: str | None = None
        if saved_document is not None:
            try:
                saved = Program.from_dict(
                    deepcopy(saved_document),
                    catalog,
                    path="<composer-retained-saved-program>",
                )
            except ProgramValidationError as error:
                _error(
                    "composer_retained_draft_invalid",
                    f"retained Saved Program is invalid: {error}",
                    status_code=503,
                )
            if saved.program_revision != saved_revision or type(saved_path) is not str:
                _error(
                    "composer_retained_draft_invalid",
                    "retained Saved Program identity is inconsistent",
                    status_code=503,
                )
            try:
                resolved_saved_path = str(Path(saved_path).resolve())
            except (OSError, RuntimeError, ValueError) as error:
                _error(
                    "composer_retained_draft_invalid",
                    f"retained Saved Program path is invalid: {error}",
                    status_code=503,
                )
        with self._lock:
            prior_identity = (self._draft_id, self._revision)
            self._draft_id = draft_id
            self._revision = revision
            self._authoring = deepcopy(authoring)
            self._program = program
            self._diagnostics = normalized_diagnostics
            self._saved_path = resolved_saved_path
            self._saved_program_revision = saved_revision
            self._saved_document = deepcopy(saved_document)
            if prior_identity != (self._draft_id, self._revision):
                self._notify_identity_change_locked()

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return self._snapshot_locked()

    def workspace_snapshot(
        self,
        *,
        applied_program_revision: str | None = None,
        applied_unavailable_reason: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Project Draft/Saved plus one externally owned Applied identity."""

        with self._lock:
            return self._workspace_snapshot_locked(
                applied_program_revision=applied_program_revision,
                applied_unavailable_reason=applied_unavailable_reason,
            )

    def validate(self, catalog: NodeCatalog) -> dict[str, Any]:
        """Revalidate the authoring source and project diagnostics/candidate."""

        with self._lock:
            if self._authoring is None:
                _error(
                    "composer_draft_unavailable",
                    "Composer has no loaded Program Draft",
                    status_code=404,
                )
            try:
                candidate, diagnostics = derive_candidate(
                    deepcopy(self._authoring), catalog
                )
            except AuthoringForestError as error:
                _error(
                    "composer_draft_validation_failed",
                    str(error),
                    detail=error.detail,
                )
            if (
                (candidate.to_dict() if candidate is not None else None)
                != (self._program.to_dict() if self._program is not None else None)
                or diagnostics != self._diagnostics
            ):
                _error(
                    "composer_draft_validation_failed",
                    "Composer Draft candidate projection is inconsistent",
                    status_code=503,
                )
            return self._snapshot_locked()

    def _snapshot_locked(self) -> dict[str, Any]:
        program = self._program
        matches_saved = self._matches_saved_locked()
        return {
            "schema_version": _DRAFT_API_VERSION,
            "draft_id": self._draft_id,
            "draft_revision": self._revision,
            "program_loaded": self._authoring is not None,
            "authoring": deepcopy(self._authoring),
            "candidate_available": program is not None,
            "program_revision": (
                program.program_revision if program is not None else None
            ),
            "program": program.to_dict() if program is not None else None,
            "diagnostics": deepcopy(self._diagnostics),
            "saved": matches_saved,
            "applied": False,
        }

    def _matches_saved_locked(self) -> bool:
        return (
            self._authoring is not None
            and self._program is not None
            and self._saved_program_revision is not None
            and self._program.program_revision == self._saved_program_revision
        )

    def _workspace_snapshot_locked(
        self,
        *,
        applied_program_revision: str | None = None,
        applied_unavailable_reason: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        program = self._program
        matches_saved = self._matches_saved_locked()
        program_document = program.to_dict() if program is not None else None
        applicability = (
            draft_applicability(program_document)
            if program_document is not None
            else {
                "applyReady": False,
                "affectedInstanceIds": [],
                "unavailableReason": {
                    "code": "composer_authoring_draft_incomplete",
                    "message": (
                        self._diagnostics[0]["message"]
                        if self._diagnostics
                        else "Program Draft has no strict candidate"
                    ),
                    "restorationGates": [],
                },
            }
        )
        applied_available = applied_program_revision is not None
        matches_applied = (
            program is not None
            and applied_program_revision is not None
            and program.program_revision == applied_program_revision
        )
        unavailable_reason = applied_unavailable_reason or {
            "code": "program_apply_unavailable",
            "message": (
                "authoritative Program Apply requires a configured Headless owner"
            ),
            "restorationGates": [],
        }
        return {
            "schemaVersion": _WORKSPACE_API_VERSION,
            "draft": {
                "draftId": self._draft_id,
                "draftRevision": self._revision,
                "programLoaded": self._authoring is not None,
                "authoring": deepcopy(self._authoring),
                "candidateAvailable": program is not None,
                "programRevision": (
                    program.program_revision if program is not None else None
                ),
                "program": program_document,
                "diagnostics": deepcopy(self._diagnostics),
            },
            "applicability": applicability,
            "saved": {
                "available": self._saved_program_revision is not None,
                "matchesDraft": matches_saved,
                "path": self._saved_path,
                "programRevision": self._saved_program_revision,
            },
            "applied": {
                "available": applied_available,
                "matchesDraft": matches_applied if applied_available else None,
                "programRevision": applied_program_revision,
                "unavailableReason": None if applied_available else unavailable_reason,
            },
            "hasUnsavedChanges": self._authoring is not None and not matches_saved,
            "hasUnappliedChanges": (
                self._authoring is not None and not matches_applied
                if applied_available
                else None
            ),
        }

    def _require_identity_locked(
        self,
        *,
        draft_id: str,
        expected_revision: int,
    ) -> None:
        if type(draft_id) is not str or not draft_id:
            _error(
                "composer_draft_id_invalid",
                "Draft identity must be a non-empty string",
            )
        if type(expected_revision) is not int or expected_revision < 0:
            _error(
                "composer_draft_revision_invalid",
                "expected Draft revision must be a non-negative integer",
            )
        if draft_id != self._draft_id:
            _error(
                "composer_draft_id_stale",
                "request names a replaced Composer Draft",
                status_code=409,
                detail={
                    "message": "request names a replaced Composer Draft",
                    "expectedDraftId": self._draft_id,
                    "receivedDraftId": draft_id,
                },
            )
        if expected_revision != self._revision:
            _error(
                "composer_draft_revision_stale",
                "request Draft revision is stale",
                status_code=409,
                detail={
                    "message": "request Draft revision is stale",
                    "expectedDraftRevision": self._revision,
                    "receivedDraftRevision": expected_revision,
                },
            )

    def execute_if_current(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        effect: Callable[[], _T],
    ) -> _T:
        """Run one external owner effect while this Draft fence stays exact.

        Scene authoring is committed by the logical-scene owner, not by this
        session. Holding the existing Draft lock across that single call keeps
        another Composer mutation from invalidating the checked Draft between
        validation and the owner effect. The scene owner still owns admission,
        replay, projection, rollback, and the resulting scene revision.
        """

        if not callable(effect):
            raise TypeError("Draft-fenced effect must be callable")
        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            return effect()

    def require_replace_allowed(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        discard_changes: bool,
    ) -> None:
        """Fail before disk I/O when replacing the current Draft is ineligible."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if (
                self._authoring is not None
                and not self._matches_saved_locked()
                and not discard_changes
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "opening another Program would discard unsaved Draft changes",
                    status_code=409,
                )

    def require_current(self, *, draft_id: str, expected_revision: int) -> None:
        """Reject a stale Draft fence without consulting another owner."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )

    def replace_program(
        self,
        program: Program,
        *,
        source_path: str,
        draft_id: str,
        expected_revision: int,
        discard_changes: bool,
    ) -> dict[str, Any]:
        """Atomically replace Draft and Saved after an independently safe read."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if (
                self._authoring is not None
                and not self._matches_saved_locked()
                and not discard_changes
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "opening another Program would discard unsaved Draft changes",
                    status_code=409,
                )
            self._draft_id = f"draft-{uuid.uuid4().hex}"
            self._authoring = from_program(program)
            self._program = program
            self._diagnostics = []
            self._revision = 1
            self._saved_path = str(Path(source_path).resolve())
            self._saved_program_revision = program.program_revision
            self._saved_document = program.to_dict()
            self._notify_identity_change_locked()
            return self._workspace_snapshot_locked()

    def apply_current_program(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        applier: Callable[[Program], dict[str, Any]],
    ) -> dict[str, Any]:
        """Apply one exact Draft while preventing a concurrent relabeling race."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if self._program is None:
                _error(
                    "composer_authoring_draft_incomplete",
                    "Composer Draft has no strict Program candidate",
                    status_code=409,
                    detail={"diagnostics": deepcopy(self._diagnostics)},
                )
            return applier(self._program)

    def save_program(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        target_path: str | None,
        writer: Callable[[Program, str], str],
    ) -> dict[str, Any]:
        """Serialize one exact Draft while blocking concurrent Draft replacement."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if self._program is None:
                _error(
                    "composer_authoring_draft_incomplete",
                    "Composer Draft has no strict Program candidate",
                    status_code=409,
                    detail={"diagnostics": deepcopy(self._diagnostics)},
                )
            selected_path = target_path or self._saved_path
            if selected_path is None:
                _error(
                    "composer_saved_path_unavailable",
                    "Save requires an existing Program path; use Save As",
                )
            saved_path = writer(self._program, selected_path)
            self._saved_path = str(Path(saved_path).resolve())
            self._saved_program_revision = self._program.program_revision
            self._saved_document = self._program.to_dict()
            return self._workspace_snapshot_locked()

    def new_draft(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        confirm_discard: bool,
        applied_revision_reader: Callable[[], str | None],
        catalog_reader: Callable[[], NodeCatalog],
    ) -> dict[str, Any]:
        """Create one fresh retained empty authoring forest under the old fence."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if (
                self._authoring is not None
                and not self._matches_saved_locked()
                and not confirm_discard
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "creating a new Program would discard unsaved Draft changes",
                    status_code=409,
                )
            if (
                self._authoring is not None
                and not confirm_discard
                and (applied_revision := applied_revision_reader()) is not None
                and (
                    self._program is None
                    or self._program.program_revision != applied_revision
                )
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "creating a new Program would discard unapplied Draft changes",
                    status_code=409,
                )

            catalog = catalog_reader()
            authoring = empty_authoring(catalog)
            program, diagnostics = derive_candidate(authoring, catalog)
            assert program is None

            self._draft_id = f"draft-{uuid.uuid4().hex}"
            self._authoring = authoring
            self._program = program
            self._diagnostics = diagnostics
            self._revision = 1
            self._saved_path = None
            self._saved_program_revision = None
            self._saved_document = None
            self._notify_identity_change_locked()
            return self._workspace_snapshot_locked()

    def discard_to_applied(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        reader: Callable[[], Program],
    ) -> dict[str, Any]:
        """Replace Draft from one action-local authoritative Applied read."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            applied = reader()
            if (
                self._program is not None
                and self._program.to_dict() == applied.to_dict()
                and self._authoring == from_program(applied)
            ):
                return self._workspace_snapshot_locked(
                    applied_program_revision=applied.program_revision,
                )
            if self._revision >= _MAXIMUM_DRAFT_REVISION:
                _error(
                    "composer_draft_revision_exhausted",
                    "Composer Draft revision is exhausted",
                    status_code=503,
                )
            self._authoring = from_program(applied)
            self._program = applied
            self._diagnostics = []
            self._revision += 1
            self._notify_identity_change_locked()
            return self._workspace_snapshot_locked(
                applied_program_revision=applied.program_revision,
            )

    def revert_saved_after_apply(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        prepare: Callable[[dict[str, Any]], tuple[Program, Program, Any]],
        applier: Callable[[Program, Any], dict[str, Any]],
    ) -> tuple[dict[str, Any] | None, dict[str, Any]]:
        """Apply exact Saved content, then publish it locally only on success."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if self._saved_document is None:
                _error(
                    "composer_saved_program_unavailable",
                    "Composer has no retained Saved Program",
                    status_code=404,
                )
            saved_document = deepcopy(self._saved_document)
            validated_saved, applied, applied_context = prepare(saved_document)
            saved_authoring = from_program(validated_saved)
            draft_matches_saved = (
                self._program is not None
                and self._program.to_dict() == saved_document
                and self._authoring == saved_authoring
            )
            applied_matches_saved = applied.to_dict() == saved_document
            if applied_matches_saved:
                if not draft_matches_saved:
                    if self._revision >= _MAXIMUM_DRAFT_REVISION:
                        _error(
                            "composer_draft_revision_exhausted",
                            "Composer Draft revision is exhausted",
                            status_code=503,
                        )
                    self._authoring = saved_authoring
                    self._program = validated_saved
                    self._diagnostics = []
                    self._revision += 1
                    self._notify_identity_change_locked()
                return None, self._workspace_snapshot_locked(
                    applied_program_revision=validated_saved.program_revision,
                )
            if (
                not draft_matches_saved
                and self._revision >= _MAXIMUM_DRAFT_REVISION
            ):
                _error(
                    "composer_draft_revision_exhausted",
                    "Composer Draft revision is exhausted",
                    status_code=503,
                )
            result = applier(validated_saved, applied_context)
            if not isinstance(result, dict) or result.get("status") != "applied":
                _error(
                    "program_apply_response_invalid",
                    "Program Apply did not return one completed Applied result",
                    status_code=502,
                )
            if not draft_matches_saved:
                self._authoring = saved_authoring
                self._program = validated_saved
                self._diagnostics = []
                self._revision += 1
                self._notify_identity_change_locked()
            return result, self._workspace_snapshot_locked(
                applied_program_revision=validated_saved.program_revision,
            )

    def mutate(
        self,
        *,
        tool_id: str,
        arguments: dict[str, Any],
        expected_revision: int,
        catalog: NodeCatalog,
        draft_id: str | None = None,
    ) -> DraftMutationResult:
        if type(arguments) is not dict:
            _error("draft_arguments_invalid", "callback arguments must be an object")
        with self._lock:
            if self._authoring is None:
                _error(
                    "composer_draft_unavailable",
                    "Composer has no loaded Program Draft",
                    status_code=404,
                )
            if draft_id is not None:
                self._require_identity_locked(
                    draft_id=draft_id,
                    expected_revision=expected_revision,
                )
            elif type(expected_revision) is not int or expected_revision < 0:
                _error(
                    "composer_draft_revision_invalid",
                    "expected Draft revision must be a non-negative integer",
                )
            if draft_id is None and expected_revision != self._revision:
                _error(
                    "composer_draft_revision_stale",
                    "callback Draft revision is stale",
                    status_code=409,
                    detail={
                        "message": "callback Draft revision is stale",
                        "expectedDraftRevision": self._revision,
                        "receivedDraftRevision": expected_revision,
                    },
                )
            document = deepcopy(self._authoring)
            try:
                mutation, details = self._edit(
                    document,
                    tool_id=tool_id,
                    arguments=deepcopy(arguments),
                    catalog=catalog,
                )
            except DraftSessionError:
                raise
            except (AttributeError, IndexError, KeyError, TypeError, ValueError) as error:
                _error(
                    "draft_arguments_invalid",
                    f"Draft mutation arguments are invalid for {tool_id}: {error}",
                )
            try:
                candidate, diagnostics = derive_candidate(document, catalog)
            except AuthoringForestError as error:
                _error(
                    error.code,
                    str(error),
                    detail=error.detail,
                )
            if document == self._authoring:
                return DraftMutationResult(
                    mutation=mutation,
                    details=details,
                    snapshot=self._snapshot_locked(),
                )
            if self._revision >= _MAXIMUM_DRAFT_REVISION:
                _error(
                    "composer_draft_revision_exhausted",
                    "Composer Draft revision is exhausted",
                    status_code=503,
                )
            self._authoring = document
            self._program = candidate
            self._diagnostics = diagnostics
            self._revision += 1
            self._notify_identity_change_locked()
            return DraftMutationResult(
                mutation=mutation,
                details=details,
                snapshot=self._snapshot_locked(),
            )

    def preview_mutation(
        self,
        *,
        tool_id: str,
        arguments: dict[str, Any],
        expected_revision: int,
        catalog: NodeCatalog,
        draft_id: str | None = None,
    ) -> DraftMutationResult:
        """Validate one mutation against an isolated exact Draft snapshot."""

        preview = ComposerDraftSession()
        preview.restore_retained_state(self.retained_state(), catalog)
        return preview.mutate(
            tool_id=tool_id,
            arguments=arguments,
            expected_revision=expected_revision,
            catalog=catalog,
            draft_id=draft_id,
        )

    def _edit(
        self,
        document: dict[str, Any],
        *,
        tool_id: str,
        arguments: dict[str, Any],
        catalog: NodeCatalog,
    ) -> tuple[str, dict[str, Any]]:
        handlers: dict[
            str,
            Callable[[dict[str, Any], dict[str, Any], NodeCatalog], dict[str, Any]],
        ] = {
            "assistant_draft_materialize_proposal": self._assistant_proposal_materialize,
            "program_draft_expand_authoring_macro": self._authoring_macro_expand,
            "program_draft_remove_contact_policies": self._contact_policies_remove,
            "program_draft_store_observed_move": self._observed_move_store,
            "program_draft_store_observed_scene_resource": self._observed_scene_resource_store,
            "program_draft_replace_support_contacts": self._support_contacts_replace,
            "program_draft_upsert_contact_policies": self._contact_policies_upsert,
            "scene_draft_add_object": self._scene_add,
            "scene_draft_update_object": self._scene_update,
            "scene_draft_upsert_objects": self._scene_upsert,
            "scene_draft_remove_objects": self._scene_remove,
            "viewer_draft_replace": self._viewer_replace,
            "tree_draft_insert_node": self._tree_insert,
            "tree_draft_update_node_params": self._tree_update,
            "tree_draft_delete_node": self._tree_delete,
            "tree_draft_move_node": self._tree_move,
            "tree_draft_rename_node": self._tree_rename,
            "tree_draft_update_positions": self._tree_update_positions,
            "tree_draft_replace_subtree": self._tree_replace,
            "tree_draft_change_node_kind": self._tree_change_kind,
            "tree_draft_wrap_node": self._tree_wrap,
            "program_draft_upsert_parameters": self._parameters_upsert,
            "program_draft_remove_parameters": self._parameters_remove,
            "program_draft_replace_roles": self._program_roles_replace,
            "program_draft_update_motion_defaults": self._program_motion_defaults_update,
            "program_draft_update_settings": self._program_settings_update,
            _NODE_POLICY_MUTATION: self._node_policies_update,
            "blackboard_draft_upsert_namespaces": self._namespaces_upsert,
            "blackboard_draft_remove_namespaces": self._namespaces_remove,
            "blackboard_draft_upsert_keys": self._keys_upsert,
            "blackboard_draft_remove_keys": self._keys_remove,
        }
        handler = handlers.get(tool_id)
        if handler is None:
            _error(
                "composer_draft_tool_unsupported",
                f"Composer Draft tool is unsupported: {tool_id}",
                status_code=404,
            )
        details = handler(document, arguments, catalog)
        return tool_id, details

    @staticmethod
    def _contact_policy_rows(document: dict[str, Any]) -> list[dict[str, Any]]:
        rows = document.setdefault("contact_policies", [])
        if type(rows) is not list:
            _error(
                "composer_draft_invalid",
                "Program Draft contact_policies is not an array",
            )
        return rows

    @staticmethod
    def _canonical_contact_policy(raw: Any) -> dict[str, Any]:
        if type(raw) is not dict:
            _error("draft_arguments_invalid", "contact policy must be an object")
        policy = deepcopy(raw)
        policy.pop("version", None)
        required = {
            "api_version",
            "policy_id",
            "mode",
            "sensing_mode",
            "default_unmatched_response",
            "zones",
        }
        if set(policy) != required:
            _error(
                "draft_arguments_invalid",
                "contact policy fields do not match the qualified v1 shape",
            )
        if (
            policy["api_version"] != "manyforge.contact_policy.v1"
            or policy["mode"] != "contact_grasp"
            or policy["sensing_mode"] != "geometry_only"
            or policy["default_unmatched_response"] != "hard_stop"
            or not _is_portable_id(policy["policy_id"])
            or type(policy["zones"]) is not list
            or len(policy["zones"]) != 1
        ):
            _error(
                "draft_arguments_invalid",
                "contact policy is outside the qualified geometry-only contact-grasp profile",
            )
        zone = policy["zones"][0]
        zone_fields = {
            "zone_id",
            "body_pairs",
            "violation_response",
            "max_penetration_m",
            "terminal_window_start_fraction",
        }
        if (
            type(zone) is not dict
            or set(zone) != zone_fields
            or not _is_portable_id(zone["zone_id"])
            or type(zone["body_pairs"]) is not list
            or not zone["body_pairs"]
            or zone["violation_response"] != "hard_stop"
            or type(zone["max_penetration_m"]) not in {int, float}
            or not math.isfinite(float(zone["max_penetration_m"]))
            or float(zone["max_penetration_m"]) <= 0.0
            or type(zone["terminal_window_start_fraction"]) not in {int, float}
            or not math.isfinite(float(zone["terminal_window_start_fraction"]))
            or not 0.0 <= float(zone["terminal_window_start_fraction"]) <= 1.0
        ):
            _error("draft_arguments_invalid", "contact policy zone is invalid")
        pair_keys: list[tuple[str, str]] = []
        for pair in zone["body_pairs"]:
            if type(pair) is not dict or set(pair) != {"body_a", "body_b"}:
                _error("draft_arguments_invalid", "contact body pair is invalid")
            selectors = [pair["body_a"], pair["body_b"]]
            if any(type(selector) is not dict for selector in selectors):
                _error("draft_arguments_invalid", "contact selector is invalid")
            component = next(
                (
                    selector
                    for selector in selectors
                    if selector.get("kind") == "component_model_body"
                ),
                None,
            )
            runtime = next(
                (
                    selector
                    for selector in selectors
                    if selector.get("kind") == "runtime_object"
                ),
                None,
            )
            if component is None or runtime is None:
                _error(
                    "draft_arguments_invalid",
                    "contact pair must join one component body to one runtime object",
                )
            if (
                frozenset(component)
                not in {
                    frozenset(
                        {"kind", "component_role_id", "collision_body_role_id"}
                    ),
                    frozenset(
                        {
                            "kind",
                            "component_role_id",
                            "model_role_id",
                            "collision_body_role_id",
                        }
                    ),
                }
                or set(runtime) != {"kind", "runtime_object_id"}
                or any(
                    not _is_portable_id(component.get(field))
                    for field in set(component) - {"kind"}
                )
                or not _is_portable_id(runtime.get("runtime_object_id"))
            ):
                _error("draft_arguments_invalid", "contact selector is invalid")
            pair_keys.append(
                tuple(
                    sorted(
                        (
                            "component_model_body:"
                            + canonical_json_bytes(component).decode("utf-8"),
                            f"runtime_object:{runtime['runtime_object_id']}",
                        )
                    )
                )
            )
        if pair_keys != sorted(set(pair_keys)):
            _error(
                "draft_arguments_invalid",
                "contact body pairs must be unique and canonical-sorted",
            )
        policy["version"] = "sha256:" + ("0" * 64)
        policy["version"] = compute_content_identity(
            policy,
            own_field="version",
            identity_domain="catalog-record-revision/contact_policy",
            api_version="manyforge.contact_policy.v1",
        )
        return policy

    def _contact_policies_upsert(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"contact_policies"} or type(arguments["contact_policies"]) is not list:
            _error(
                "draft_arguments_invalid",
                "program_draft_upsert_contact_policies requires one contact_policies array",
            )
        incoming = [
            self._canonical_contact_policy(row)
            for row in arguments["contact_policies"]
        ]
        inserted, updated = self._upsert_rows(
            self._contact_policy_rows(document), incoming, "policy_id"
        )
        return {"inserted": inserted, "updated": updated}

    def _contact_policies_remove(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"contact_policy_ids"} or type(arguments["contact_policy_ids"]) is not list:
            _error(
                "draft_arguments_invalid",
                "program_draft_remove_contact_policies requires one contact_policy_ids array",
            )
        return self._remove_rows(
            self._contact_policy_rows(document),
            arguments["contact_policy_ids"],
            "policy_id",
            "contact policies",
        )

    def _authoring_macro_expand(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        common = {
            "macro_id",
            "scene_resource_id",
            "target_role",
            "tool_target_role",
            "base_frame_role",
            "tcp_frame_role",
            "carrier_frame_role",
            "tool_position",
            "blackboard_namespace_id",
            "blackboard_key",
            "instance_prefix",
        }
        optional = {
            "parent_instance_id",
            "index",
            "position",
            "planning_profile",
            "motion_options",
            "tool_max_effort",
            "tool_command_duration_s",
            "tool_completion_timeout_s",
        }
        macro_id = arguments.get("macro_id")
        if macro_id == "pick_subtree":
            operation = "pick"
            specific = {"contact_policy_id", "touch_body_roles"}
        elif macro_id == "place_subtree":
            operation = "place"
            specific = {"place_pose", "approach_offset_m"}
        else:
            _error(
                "draft_authoring_macro_unknown",
                f"unknown authoring macro: {macro_id}",
                status_code=404,
            )
        required = common | specific
        if set(arguments) - required - optional or not required <= set(arguments):
            _error(
                "draft_arguments_invalid",
                f"{operation} subtree macro arguments are incomplete or unsupported",
            )
        identifier_fields = {
            "scene_resource_id",
            "target_role",
            "tool_target_role",
            "base_frame_role",
            "tcp_frame_role",
            "carrier_frame_role",
            "blackboard_namespace_id",
            "blackboard_key",
            "instance_prefix",
        }
        if operation == "pick":
            identifier_fields.add("contact_policy_id")
        if any(not _is_portable_id(arguments[field]) for field in identifier_fields):
            _error(
                "draft_arguments_invalid",
                "authoring macro identities must be portable lower-case IDs",
            )
        if operation == "pick" and (
            type(arguments["touch_body_roles"]) is not list
            or not arguments["touch_body_roles"]
            or any(not _is_portable_id(role) for role in arguments["touch_body_roles"])
            or arguments["touch_body_roles"]
            != sorted(set(arguments["touch_body_roles"]))
        ):
            _error(
                "draft_arguments_invalid",
                "touch_body_roles must be a non-empty canonical ID array",
            )
        scene_ids = {row.get("scene_resource_id") for row in self._scene(document)}
        if arguments["scene_resource_id"] not in scene_ids:
            _error(
                "draft_scene_resource_not_found",
                f"{operation} authoring requires an existing Program scene resource",
                status_code=404,
            )
        for role_field in ("target_role", "tool_target_role"):
            if arguments[role_field] not in document.get("target_roles", {}):
                _error(
                    "draft_target_role_not_found",
                    f"{operation} authoring {role_field} is not declared",
                    status_code=404,
                )
        if operation == "pick" and arguments["contact_policy_id"] not in {
            row.get("policy_id") for row in self._contact_policy_rows(document)
        }:
            _error(
                "draft_contact_policy_not_found",
                "pick authoring contact policy is not declared",
                status_code=404,
            )

        blackboard = self._blackboard(document)
        namespace_id = arguments["blackboard_namespace_id"]
        key_id = arguments["blackboard_key"]
        namespaces = {row["namespace_id"]: row for row in blackboard["namespaces"]}
        keys = {row["key"]: row for row in blackboard["keys"]}
        if namespace_id not in namespaces:
            blackboard["namespaces"].append(
                {"namespace_id": namespace_id, "owner": "program"}
            )
            blackboard["namespaces"].sort(key=lambda row: row["namespace_id"])
        elif namespaces[namespace_id].get("owner") != "program":
            _error(
                "draft_declaration_conflict",
                f"{operation} authoring namespace is not Program-owned",
                status_code=409,
            )
        if not key_id.startswith(f"{namespace_id}."):
            _error(
                "draft_arguments_invalid",
                f"{operation} result key must belong to its namespace",
            )
        expected_key = {
            "key": key_id,
            "type": "pick_planning_result",
            "access": "read_write",
            "persistence": "run",
            "fields": deepcopy(PICK_PLACE_RESULT_FIELDS),
        }
        existing_key = keys.get(key_id)
        if existing_key is not None and any(
            existing_key.get(field) != value
            for field, value in expected_key.items()
        ):
            _error(
                "draft_declaration_conflict",
                f"{operation} result key has an incompatible declaration",
                status_code=409,
            )
        if existing_key is None:
            blackboard["keys"].append(expected_key)
            blackboard["keys"].sort(key=lambda row: row["key"])

        tree = self._tree(document)
        existing_ids = set(_tree_ids(tree))
        base_prefix = arguments["instance_prefix"]
        prefix = base_prefix
        suffix = 2
        tool_suffix = "close" if operation == "pick" else "open"
        scene_suffix = "attach" if operation == "pick" else "detach"
        child_suffixes = (
            "plan",
            "approach",
            "descend",
            tool_suffix,
            scene_suffix,
            "retreat",
        )
        while any(
            identity in existing_ids
            for identity in (prefix, *(f"{prefix}.{part}" for part in child_suffixes))
        ):
            prefix = f"{base_prefix}.{suffix}"
            suffix += 1

        template_parameters = {
            "target_role": arguments["target_role"],
            "tool_target_role": arguments["tool_target_role"],
            "base_frame_role": arguments["base_frame_role"],
            "tcp_frame_role": arguments["tcp_frame_role"],
            "carrier_frame_role": arguments["carrier_frame_role"],
            "selected_object_id": arguments["scene_resource_id"],
            "tool_position": deepcopy(arguments["tool_position"]),
            "planning_profile": arguments.get("planning_profile", "first_feasible"),
            "motion_options": deepcopy(arguments.get("motion_options", {})),
        }
        if operation == "pick":
            template_parameters["contact_policy_id"] = arguments["contact_policy_id"]
        else:
            template_parameters["place_pose"] = deepcopy(arguments["place_pose"])
            template_parameters["approach_offset_m"] = deepcopy(
                arguments["approach_offset_m"]
            )
        for field in (
            "tool_max_effort",
            "tool_command_duration_s",
            "tool_completion_timeout_s",
        ):
            if field in arguments:
                template_parameters[field] = deepcopy(arguments[field])
        try:
            wrapper = build_pick_place_subtree(
                operation,
                instance_id=prefix,
                parameters=template_parameters,
                value_sources={},
                result_key=key_id,
                touch_body_roles=(
                    arguments["touch_body_roles"] if operation == "pick" else ()
                ),
            )
        except PickPlaceTemplateError as exc:
            _error("draft_arguments_invalid", str(exc))
        insertion = {"node": wrapper}
        for field in ("parent_instance_id", "index", "position"):
            if field in arguments:
                insertion[field] = deepcopy(arguments[field])
        self._tree_insert(document, insertion, _catalog)
        return {
            "macro_id": macro_id,
            "inserted_instance_ids": [
                prefix,
                *(f"{prefix}.{part}" for part in child_suffixes),
            ],
            "blackboard_key": key_id,
        }
    def _observed_move_store(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {
            "instance_id",
            "expected_node_id",
            "observed_kind",
            "observed",
        } or type(arguments["observed"]) is not dict:
            _error(
                "draft_arguments_invalid",
                "observed move materialization has an unsupported shape",
            )
        observed = arguments["observed"]
        if arguments["observed_kind"] == "joint":
            names = observed.get("source_joint_names")
            positions = observed.get("positions_rad")
            if (
                arguments["expected_node_id"] != "motion.move_to_joint_target"
                or type(names) is not list
                or not names
                or any(type(name) is not str or not name for name in names)
                or names != list(dict.fromkeys(names))
                or type(positions) is not list
                or len(names) != len(positions)
                or any(
                    type(position) not in {int, float}
                    or not math.isfinite(float(position))
                    for position in positions
                )
            ):
                _error("draft_arguments_invalid", "observed joint target is invalid")
            goal = [
                {"joint_name": name, "position_rad": position}
                for name, position in zip(names, positions, strict=True)
            ]
        elif arguments["observed_kind"] == "tcp_pose":
            if arguments["expected_node_id"] != "motion.move_to_pose":
                _error("draft_arguments_invalid", "observed TCP target requires a pose node")
            pose = observed.get("current_tcp_pose")
            try:
                position = pose["position_m"]
                orientation = pose["orientation"]
                goal = {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [position["x_m"], position["y_m"], position["z_m"]],
                    "orientation": {
                        "xyzw": [
                            orientation["x"],
                            orientation["y"],
                            orientation["z"],
                            orientation["w"],
                        ]
                    },
                }
            except (KeyError, TypeError):
                _error("draft_arguments_invalid", "observed TCP pose is invalid")
            values = [*goal["position_m"], *goal["orientation"]["xyzw"]]
            if (
                any(
                    type(value) not in {int, float}
                    or not math.isfinite(float(value))
                    for value in values
                )
                or math.hypot(*[float(value) for value in values[3:]]) <= 1e-12
            ):
                _error("draft_arguments_invalid", "observed TCP pose is invalid")
        else:
            _error("draft_arguments_invalid", "observed move kind is unsupported")
        reference = _tree_ref(self._tree(document), arguments["instance_id"])
        node = reference[0] if reference is not None else None
        if node is None or node.get("node_id") != arguments["expected_node_id"]:
            _error(
                "draft_node_not_found",
                f"Draft node does not exist: {arguments['instance_id']}",
                status_code=404,
            )
        parameters = deepcopy(node.get("parameters", {}))
        parameters["goal"] = goal
        if arguments["observed_kind"] == "tcp_pose":
            for field in ("target_mode", "target_reference", "offset_reference"):
                parameters.pop(field, None)
        value_sources = deepcopy(node.get("value_sources", {}))
        value_sources.pop("goal", None)
        return self._tree_update(
            document,
            {
                "instance_id": arguments["instance_id"],
                "expected_node_id": arguments["expected_node_id"],
                "parameters": parameters,
                "value_sources": value_sources,
                "merge": False,
            },
            catalog,
        )

    def _observed_scene_resource_store(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"scene_resource"} or type(arguments["scene_resource"]) is not dict:
            _error(
                "draft_arguments_invalid",
                "observed scene resource materialization requires one resolved scene_resource",
            )
        return self._scene_upsert(
            document,
            {"scene_resources": [arguments["scene_resource"]]},
            catalog,
        )

    def _support_contacts_replace(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if (
            set(arguments) != {"scene_resource_id", "support_contacts"}
            or not _is_portable_id(arguments["scene_resource_id"])
            or type(arguments["support_contacts"]) is not list
        ):
            _error(
                "draft_arguments_invalid",
                "support-contact replacement requires one scene resource and contact array",
            )
        resource = next(
            (
                row
                for row in self._scene(document)
                if row.get("scene_resource_id") == arguments["scene_resource_id"]
            ),
            None,
        )
        if resource is None:
            _error(
                "draft_scene_resource_not_found",
                "supported Program scene resource does not exist",
                status_code=404,
            )
        metadata = deepcopy(resource.get("metadata", {}))
        if type(metadata) is not dict:
            _error("composer_draft_invalid", "Program scene metadata is not an object")
        contacts = sorted(
            deepcopy(arguments["support_contacts"]),
            key=lambda row: row.get("support_object_id", "")
            if type(row) is dict
            else "",
        )
        if contacts:
            metadata["support_contacts"] = contacts
        else:
            metadata.pop("support_contacts", None)
        if metadata:
            resource["metadata"] = metadata
        else:
            resource.pop("metadata", None)
        return {
            "scene_resource_id": arguments["scene_resource_id"],
            "support_contact_count": len(contacts),
        }

    def _assistant_proposal_materialize(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if (
            set(arguments) != {"proposal_tool_id", "operations"}
            or type(arguments["proposal_tool_id"]) is not str
            or type(arguments["operations"]) is not list
            or not arguments["operations"]
            or len(arguments["operations"]) > 8_192
        ):
            _error("draft_arguments_invalid", "assistant Draft proposal is invalid")
        details: list[dict[str, Any]] = []
        handlers = {
            "scene_draft_upsert_objects": self._scene_upsert,
            "tree_draft_insert_node": self._tree_insert,
        }
        for operation in arguments["operations"]:
            if (
                type(operation) is not dict
                or set(operation) != {"mutation", "arguments"}
                or operation["mutation"] not in _ASSISTANT_PROPOSAL_MUTATIONS
                or type(operation["arguments"]) is not dict
            ):
                _error(
                    "draft_arguments_invalid",
                    "assistant proposal contains an unsupported Draft operation",
                )
            mutation = operation["mutation"]
            details.append(
                {
                    "mutation": mutation,
                    "details": handlers[mutation](
                        document, deepcopy(operation["arguments"]), catalog
                    ),
                }
            )
        return {
            "proposal_tool_id": arguments["proposal_tool_id"],
            "operations": details,
        }

    @staticmethod
    def _program_roles_replace(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"component_roles", "target_roles"}:
            _error(
                "draft_arguments_invalid",
                "program_draft_replace_roles requires exactly component_roles and target_roles",
            )
        component_roles = arguments["component_roles"]
        target_roles = arguments["target_roles"]
        if type(component_roles) is not dict or type(target_roles) is not dict:
            _error(
                "draft_arguments_invalid",
                "Program component_roles and target_roles must be objects",
            )
        document["component_roles"] = {
            key: deepcopy(component_roles[key]) for key in sorted(component_roles)
        }
        document["target_roles"] = {
            key: deepcopy(target_roles[key]) for key in sorted(target_roles)
        }
        return {
            "component_role_count": len(component_roles),
            "target_role_count": len(target_roles),
        }

    @staticmethod
    def _program_settings_update(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        required = {
            "name",
            "description",
            "program_profile",
            "compatibility",
            "execution_groups",
        }
        if set(arguments) != required:
            _error(
                "draft_arguments_invalid",
                "program_draft_update_settings requires exactly name, description, "
                "program_profile, compatibility, and execution_groups",
            )
        name = arguments["name"]
        description = arguments["description"]
        program_profile = arguments["program_profile"]
        compatibility = arguments["compatibility"]
        execution_groups = arguments["execution_groups"]
        if type(name) is not str or not name.strip():
            _error("draft_arguments_invalid", "Program name must be non-empty text")
        if type(description) is not str:
            _error("draft_arguments_invalid", "Program description must be text")
        if type(program_profile) is not str:
            _error("draft_arguments_invalid", "Program profile must be text")
        if type(compatibility) is not dict:
            _error("draft_arguments_invalid", "Program compatibility must be an object")
        if type(execution_groups) is not dict:
            _error(
                "draft_arguments_invalid",
                "execution_groups must be an object",
            )
        for group_id, definition in execution_groups.items():
            if (
                type(group_id) is not str
                or type(definition) is not dict
                or set(definition) != {"execution_policy"}
                or definition["execution_policy"] not in {
                    "independent",
                    "common_start",
                    "common_start_and_finish",
                    "sync_offset_traj",
                }
            ):
                _error("draft_arguments_invalid", "execution group definition is invalid")

        document["name"] = name.strip()
        document["program_profile"] = program_profile
        if description.strip():
            document["description"] = description.strip()
        else:
            document.pop("description", None)
        if compatibility:
            document["compatibility"] = deepcopy(compatibility)
        else:
            document.pop("compatibility", None)
        if execution_groups:
            document["execution_groups"] = {
                key: deepcopy(execution_groups[key]) for key in sorted(execution_groups)
            }
        else:
            document.pop("execution_groups", None)
        return {"updated": True}

    @staticmethod
    def _program_motion_nodes(
        document: dict[str, Any], catalog: NodeCatalog
    ) -> list[tuple[dict[str, Any], tuple[str, ...]]]:
        result: list[tuple[dict[str, Any], tuple[str, ...]]] = []
        pending = list(reversed(document["forest"]))
        while pending:
            node = pending.pop()
            entry = catalog.get(node["node_id"])
            available = {parameter.id for parameter in entry.parameters}
            fields = tuple(
                field for field in _PROGRAM_MOTION_DEFAULTS if field in available
            )
            if fields:
                result.append((node, fields))
            pending.extend(reversed(node.get("children", [])))
        return result

    @classmethod
    def _program_motion_binding_ids(
        cls,
        document: dict[str, Any],
        catalog: NodeCatalog,
    ) -> dict[str, str]:
        """Resolve semantic motion fields from actual Program bindings."""

        declarations = {
            row.get("parameter_id"): row
            for row in document.get("parameters", [])
            if type(row) is dict and type(row.get("parameter_id")) is str
        }
        references: dict[str, set[str]] = {
            field: set() for field in _PROGRAM_MOTION_DEFAULTS
        }
        applicable: dict[str, int] = {
            field: 0 for field in _PROGRAM_MOTION_DEFAULTS
        }
        for node, fields in cls._program_motion_nodes(document, catalog):
            sources = node.get("value_sources", {})
            if type(sources) is not dict:
                continue
            for field in fields:
                applicable[field] += 1
                source = sources.get(field)
                if (
                    type(source) is dict
                    and source.get("kind") == "program_parameter"
                    and type(source.get("reference")) is str
                ):
                    references[field].add(source["reference"])
        resolved: dict[str, str] = {}
        for field, candidates in references.items():
            if not candidates and applicable[field] == 0:
                declaration = declarations.get(field)
                if (
                    type(declaration) is dict
                    and declaration.get("type") in {"float64", "int64"}
                    and type(declaration.get("default")) in {int, float}
                    and not isinstance(declaration.get("default"), bool)
                ):
                    resolved[field] = field
                continue
            if len(candidates) != 1:
                continue
            reference = next(iter(candidates))
            declaration = declarations.get(reference)
            if (
                type(declaration) is dict
                and declaration.get("type") in {"float64", "int64"}
                and type(declaration.get("default")) in {int, float}
                and not isinstance(declaration.get("default"), bool)
            ):
                resolved[field] = reference
        return resolved

    @classmethod
    def _program_motion_config(
        cls, document: dict[str, Any], catalog: NodeCatalog
    ) -> dict[str, Any]:
        bindings = cls._program_motion_binding_ids(document, catalog)
        declarations_by_id = {
            row["parameter_id"]: row for row in document["parameters"]
        }
        declarations = {
            field: deepcopy(declarations_by_id[reference])
            for field, reference in bindings.items()
        }
        nodes: list[dict[str, Any]] = []
        for node, fields in cls._program_motion_nodes(document, catalog):
            parameters = node.get("parameters", {})
            sources = node.get("value_sources", {})
            values: dict[str, Any] = {}
            for field in fields:
                if field in parameters:
                    values[field] = {
                        "kind": "literal",
                        "value": deepcopy(parameters[field]),
                    }
                elif field in sources:
                    values[field] = {
                        "kind": "source",
                        "value": deepcopy(sources[field]),
                    }
                else:
                    values[field] = {"kind": "omitted"}
            nodes.append({"instance_id": node["instance_id"], "values": values})
        return {
            "bindings": bindings,
            "declarations": declarations,
            "nodes": nodes,
        }

    @classmethod
    def _restore_program_motion_config(
        cls,
        document: dict[str, Any],
        config: Any,
        catalog: NodeCatalog,
    ) -> None:
        if type(config) is not dict or set(config) != {
            "bindings", "declarations", "nodes"
        }:
            _error("draft_arguments_invalid", "motion restore config is invalid")
        bindings = config["bindings"]
        declarations = config["declarations"]
        rows = config["nodes"]
        if (
            type(bindings) is not dict
            or type(declarations) is not dict
            or type(rows) is not list
            or set(bindings) != set(_PROGRAM_MOTION_DEFAULTS)
            or set(declarations) != set(_PROGRAM_MOTION_DEFAULTS)
            or any(type(value) is not str or not value for value in bindings.values())
            or len(set(bindings.values())) != len(bindings)
        ):
            _error("draft_arguments_invalid", "motion restore config is invalid")
        current_bindings = cls._program_motion_binding_ids(document, catalog)
        replaced_ids = {*current_bindings.values(), *bindings.values()}
        retained = [
            row
            for row in document["parameters"]
            if row.get("parameter_id") not in replaced_ids
        ]
        for field in _PROGRAM_MOTION_DEFAULTS:
            declaration = declarations[field]
            if (
                type(declaration) is not dict
                or declaration.get("parameter_id") != bindings[field]
            ):
                _error(
                    "draft_arguments_invalid",
                    "motion restore declaration changed binding identity",
                )
            retained.append(deepcopy(declaration))
        document["parameters"] = sorted(retained, key=lambda row: row["parameter_id"])

        current = cls._program_motion_nodes(document, catalog)
        by_id = {
            row.get("instance_id"): row
            for row in rows
            if type(row) is dict and type(row.get("instance_id")) is str
        }
        if len(by_id) != len(rows) or set(by_id) != {
            node["instance_id"] for node, _fields in current
        }:
            _error("draft_arguments_invalid", "motion restore node scope is stale")
        for node, fields in current:
            row = by_id[node["instance_id"]]
            if set(row) != {"instance_id", "values"} or type(row["values"]) is not dict:
                _error("draft_arguments_invalid", "motion restore node row is invalid")
            if set(row["values"]) != set(fields):
                _error("draft_arguments_invalid", "motion restore fields changed")
            parameters = node.setdefault("parameters", {})
            sources = deepcopy(node.get("value_sources", {}))
            for field in fields:
                value = row["values"][field]
                if type(value) is not dict or value.get("kind") not in {
                    "literal", "source", "omitted"
                }:
                    _error("draft_arguments_invalid", "motion restore value is invalid")
                parameters.pop(field, None)
                sources.pop(field, None)
                if value["kind"] == "literal" and set(value) == {"kind", "value"}:
                    parameters[field] = deepcopy(value["value"])
                elif value["kind"] == "source" and set(value) == {"kind", "value"}:
                    sources[field] = deepcopy(value["value"])
                elif value != {"kind": "omitted"}:
                    _error("draft_arguments_invalid", "motion restore value is invalid")
            if sources:
                node["value_sources"] = sources
            else:
                node.pop("value_sources", None)
        if cls._program_motion_binding_ids(document, catalog) != bindings:
            _error(
                "draft_arguments_invalid",
                "motion restore bindings do not match the restored nodes",
            )

    @classmethod
    def _program_motion_defaults_update(
        cls,
        document: dict[str, Any],
        arguments: dict[str, Any],
        catalog: NodeCatalog,
    ) -> dict[str, Any]:
        previous = cls._program_motion_config(document, catalog)
        mode = arguments.get("mode")
        if mode == "restore":
            if set(arguments) != {"mode", "config"}:
                _error("draft_arguments_invalid", "motion restore arguments are invalid")
            cls._restore_program_motion_config(document, arguments["config"], catalog)
            return {
                "previous_config": previous,
                "current_config": cls._program_motion_config(document, catalog),
            }
        if mode != "update" or set(arguments) != {
            "mode",
            "velocity_scale",
            "acceleration_scale",
            "translation_tol_m",
            "max_blend_deviation_rad",
            "parameter_bindings",
            "conflict_resolution",
        }:
            _error("draft_arguments_invalid", "motion-default arguments are invalid")
        values = {
            "velocity_scale": arguments["velocity_scale"],
            "acceleration_scale": arguments["acceleration_scale"],
            "translation_tol_m": arguments["translation_tol_m"],
            "max_blend_deviation_rad": arguments["max_blend_deviation_rad"],
        }
        if any(type(value) not in {int, float} or not math.isfinite(float(value)) for value in values.values()):
            _error("draft_arguments_invalid", "motion defaults must be finite numbers")
        if not 0.0 < float(values["velocity_scale"]) <= 1.0:
            _error("draft_arguments_invalid", "velocity scale must be in (0, 1]")
        if not 0.0 < float(values["acceleration_scale"]) <= 1.0:
            _error("draft_arguments_invalid", "acceleration scale must be in (0, 1]")
        if not float(values["translation_tol_m"]) > 0.0:
            _error("draft_arguments_invalid", "Cartesian translation tolerance must be positive")
        if float(values["max_blend_deviation_rad"]) < 0.0:
            _error("draft_arguments_invalid", "Cartesian blend deviation must be non-negative")
        resolution = arguments["conflict_resolution"]
        if resolution not in {"reject", "reduce"}:
            _error("draft_arguments_invalid", "motion conflict resolution is invalid")
        requested_bindings = arguments["parameter_bindings"]
        actual_bindings = cls._program_motion_binding_ids(document, catalog)
        if (
            type(requested_bindings) is not dict
            or set(requested_bindings) != set(_PROGRAM_MOTION_DEFAULTS)
            or any(
                type(reference) is not str or not reference
                for reference in requested_bindings.values()
            )
            or len(set(requested_bindings.values())) != len(requested_bindings)
        ):
            _error(
                "draft_arguments_invalid",
                "motion parameter bindings must identify distinct Program parameters",
            )
        if requested_bindings != actual_bindings:
            _error(
                "program_motion_binding_stale",
                "Program motion controls do not match the current catalog-field bindings",
                status_code=409,
                detail={
                    "actual_bindings": actual_bindings,
                    "requested_bindings": requested_bindings,
                },
            )

        conflicts: list[dict[str, Any]] = []
        motion_nodes = cls._program_motion_nodes(document, catalog)
        for node, fields in motion_nodes:
            parameters = node.get("parameters", {})
            sources = node.get("value_sources", {})
            for field in fields:
                literal = parameters.get(field)
                source = sources.get(field)
                if field in _PROGRAM_MOTION_CEILINGS and literal is not None and (
                    type(literal) not in {int, float}
                    or float(literal) > float(values[field])
                ):
                    conflicts.append({
                        "instance_id": node["instance_id"],
                        "field": field,
                        "current_value": deepcopy(literal),
                        "new_ceiling": float(values[field]),
                    })
                if source is not None and source != {
                    "kind": "program_parameter",
                    "reference": actual_bindings[field],
                }:
                    _error(
                        "program_motion_source_conflict",
                        f"{node['instance_id']}.{field} uses another dynamic source",
                        status_code=409,
                    )
        if conflicts and resolution == "reject":
            _error(
                "program_motion_limit_conflict",
                "One or more node motion limits exceed the proposed Program ceiling",
                status_code=409,
                detail={"message": "Node motion limits exceed the proposed Program ceiling.", "conflicts": conflicts},
            )

        by_id = {row["parameter_id"]: row for row in document["parameters"]}
        for field, reference in actual_bindings.items():
            declaration = deepcopy(by_id[reference])
            declaration["default"] = float(values[field])
            by_id[reference] = declaration
        document["parameters"] = [by_id[key] for key in sorted(by_id)]

        conflict_keys = {(row["instance_id"], row["field"]) for row in conflicts}
        for node, fields in motion_nodes:
            parameters = node.setdefault("parameters", {})
            sources = deepcopy(node.get("value_sources", {}))
            for field in fields:
                if (node["instance_id"], field) in conflict_keys:
                    parameters[field] = float(values[field])
                    sources.pop(field, None)
                elif field not in parameters and field not in sources:
                    sources[field] = {
                        "kind": "program_parameter",
                        "reference": actual_bindings[field],
                    }
            if sources:
                node["value_sources"] = sources
            else:
                node.pop("value_sources", None)
        return {
            "reduced_node_fields": conflicts,
            "previous_config": previous,
            "current_config": cls._program_motion_config(document, catalog),
        }

    @staticmethod
    def _node_policies_update(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        (
            instance_id,
            modification,
            recovery,
            operator_stop,
        ) = _node_policy_arguments(arguments)
        groups = None
        if "execution_groups" in arguments:
            try:
                groups = parse_execution_groups(arguments["execution_groups"], path="node")
            except ProgramValidationError as error:
                _error("draft_arguments_invalid", str(error))
        node = _lookup_exact_instance(document["forest"], instance_id)
        if modification == _DEFAULT_MODIFICATION_POLICY:
            node.pop("modification_policy", None)
        else:
            node["modification_policy"] = modification
        if recovery == _DEFAULT_RECOVERY_POLICY:
            node.pop("recovery_policy", None)
        else:
            node["recovery_policy"] = recovery
        if operator_stop == "inherit":
            node.pop("operator_stop_policy", None)
        else:
            node["operator_stop_policy"] = operator_stop
        if groups == "inherit":
            node.pop("execution_groups", None)
        elif groups is not None:
            node["execution_groups"] = groups if isinstance(groups, str) else list(groups)
        return {"updated_instance_id": instance_id}

    @staticmethod
    def _viewer_replace(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"viewer"} or type(arguments["viewer"]) is not dict:
            _error(
                "draft_arguments_invalid",
                "viewer_draft_replace requires exactly one viewer object",
            )
        document["viewer"] = deepcopy(arguments["viewer"])
        return {"replaced": True}

    @staticmethod
    def _scene(document: dict[str, Any]) -> list[dict[str, Any]]:
        scene = document.get("scene")
        if type(scene) is not list:
            _error("composer_draft_invalid", "Program Draft scene is not an array")
        return scene

    def _scene_add(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        scene = self._scene(document)
        resource = deepcopy(arguments["scene_resource"])
        identity = resource["scene_resource_id"]
        if any(item.get("scene_resource_id") == identity for item in scene):
            _error(
                "draft_scene_resource_exists",
                f"Draft scene resource already exists: {identity}",
                status_code=409,
            )
        try:
            _absolute_orientation(resource.get("pose", {}))
        except DraftSessionError as error:
            _error(
                "composer_draft_validation_failed",
                f"new scene resource requires a valid absolute orientation: {error}",
            )
        scene.append(resource)
        scene.sort(key=lambda item: item["scene_resource_id"])
        return {"inserted": [identity], "updated": [], "unchanged": []}

    def _scene_update(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        scene = self._scene(document)
        resource = deepcopy(arguments["scene_resource"])
        identity = resource["scene_resource_id"]
        index = next(
            (
                position
                for position, item in enumerate(scene)
                if item.get("scene_resource_id") == identity
            ),
            None,
        )
        if index is None:
            _error(
                "draft_scene_resource_not_found",
                f"Draft scene resource does not exist: {identity}",
                status_code=404,
            )
        existing = scene[index]
        resource = _resolve_relative_orientation(
            resource,
            scene=scene,
            current=existing,
        )
        if "metadata" in existing:
            resource["metadata"] = deepcopy(existing["metadata"])
        scene[index] = resource
        return {"inserted": [], "updated": [identity], "unchanged": []}

    def _scene_upsert(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        scene = self._scene(document)
        by_id = {item["scene_resource_id"]: item for item in scene}
        inserted: list[str] = []
        updated: list[str] = []
        unchanged: list[str] = []
        seen: set[str] = set()
        for raw in arguments["scene_resources"]:
            identity = raw["scene_resource_id"]
            if identity in seen:
                _error(
                    "draft_scene_resource_duplicate",
                    f"scene_resources contains duplicate identity: {identity}",
                )
            seen.add(identity)
            existing = by_id.get(identity)
            if existing is None:
                try:
                    _absolute_orientation(raw.get("pose", {}))
                except DraftSessionError as error:
                    _error(
                        "composer_draft_validation_failed",
                        f"new scene resource requires a valid absolute orientation: {error}",
                    )
            resource = _resolve_relative_orientation(
                raw,
                scene=scene,
                current=existing,
            )
            if existing is not None and "metadata" in existing:
                resource["metadata"] = deepcopy(existing["metadata"])
            if existing is None:
                inserted.append(identity)
            elif resource == existing:
                unchanged.append(identity)
            else:
                updated.append(identity)
            by_id[identity] = resource
        document["scene"] = [by_id[key] for key in sorted(by_id)]
        return {"inserted": inserted, "updated": updated, "unchanged": unchanged}

    def _scene_remove(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        scene = self._scene(document)
        identities = arguments["scene_resource_ids"]
        existing = {item["scene_resource_id"] for item in scene}
        missing = sorted(set(identities) - existing)
        if missing:
            _error(
                "draft_scene_resource_not_found",
                f"Draft scene resources do not exist: {missing}",
                status_code=404,
            )
        removed = set(identities)
        document["scene"] = [
            item for item in scene if item["scene_resource_id"] not in removed
        ]
        return {"removed": list(identities)}

    @staticmethod
    def _tree(document: dict[str, Any]) -> list[dict[str, Any]]:
        forest = document.get("forest")
        if type(forest) is not list:
            _error("composer_draft_invalid", "Program Draft forest is not a list")
        return forest

    def _tree_insert(
        self, document: dict[str, Any], arguments: dict[str, Any], catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node = deepcopy(arguments["node"])
        motion_bindings = self._program_motion_binding_ids(document, catalog)
        entry = catalog.get(node["node_id"])
        motion_fields = {
            parameter.id
            for parameter in entry.parameters
            if parameter.id in _PROGRAM_MOTION_DEFAULTS
            and parameter.id in motion_bindings
        }
        if motion_fields:
            parameters = node.setdefault("parameters", {})
            sources = deepcopy(node.get("value_sources", {}))
            for field in sorted(motion_fields):
                parameters.pop(field, None)
                sources[field] = {
                    "kind": "program_parameter",
                    "reference": motion_bindings[field],
                }
            node["value_sources"] = sources
        if _tree_ref(tree, node["instance_id"]) is not None:
            _error(
                "draft_node_exists",
                f"Draft node already exists: {node['instance_id']}",
                status_code=409,
            )
        parent_id = arguments.get("parent_instance_id")
        index: int
        parent: dict[str, Any] | None
        if parent_id is not None:
            parent = _lookup(tree, parent_id, label="parent_instance_id")[0]
            children = parent["children"]
            index = _insert_index(arguments, len(children))
        elif arguments.get("before_instance_id") is not None or arguments.get("after_instance_id") is not None:
            before = arguments.get("before_instance_id")
            after = arguments.get("after_instance_id")
            sibling_id = before if before is not None else after
            _sibling, parent, sibling_index = _lookup(
                tree, sibling_id, label="sibling instance"
            )
            if sibling_index is None:
                _error(
                    "draft_insert_position_invalid",
                    "sibling insertion position is unavailable",
                )
            children = tree if parent is None else parent["children"]
            index = sibling_index if before is not None else sibling_index + 1
        else:
            parent = None
            children = tree
            index = _insert_index(arguments, len(children))
        children.insert(index, node)
        position = arguments.get("position")
        if position is not None:
            document["positions"][node["instance_id"]] = deepcopy(position)
        return {
            "inserted_instance_id": node["instance_id"],
            "index": index,
            "parent_instance_id": parent["instance_id"] if parent is not None else None,
        }

    def _tree_update(
        self, document: dict[str, Any], arguments: dict[str, Any], catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node = _lookup(tree, arguments["instance_id"], label="instance_id")[0]
        if node["node_id"] != arguments["expected_node_id"]:
            _error(
                "draft_node_kind_stale",
                "expected_node_id does not match the current Draft node",
                status_code=409,
            )
        merge = arguments.get("merge", True)
        incoming_parameters = deepcopy(arguments.get("parameters", {}))
        incoming_sources = deepcopy(arguments.get("value_sources", {}))
        if merge:
            parameters = merge_param_patch(node.get("parameters", {}), incoming_parameters)
            value_sources = {
                **deepcopy(node.get("value_sources", {})),
                **incoming_sources,
            }
        else:
            parameters = incoming_parameters
            value_sources = incoming_sources
        pose_parameter_ids = _pose_authoring_parameter_ids(catalog, node["node_id"])
        for name in pose_parameter_ids & incoming_parameters.keys():
            incoming_pose = incoming_parameters[name]
            if type(incoming_pose) is dict:
                parameters[name] = _merge_pose_authoring_patch(
                    node.get("parameters", {}).get(name),
                    incoming_pose,
                    merge=merge,
                    scene=self._scene(document),
                )
        for name in incoming_parameters:
            value_sources.pop(name, None)
        for name in incoming_sources:
            parameters.pop(name, None)
        node["parameters"] = parameters
        if value_sources:
            node["value_sources"] = value_sources
        else:
            node.pop("value_sources", None)
        return {"updated_instance_id": node["instance_id"]}

    def _tree_delete(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node, parent, index = _lookup(
            tree, arguments["instance_id"], label="instance_id"
        )
        if index is None:
            _error("draft_node_not_found", "the Draft node has no retained position")
        root_index = next(
            root_index
            for root_index, root in enumerate(tree)
            if _contains_instance(root, node["instance_id"])
        )
        direct_children = node["children"]
        (tree if parent is None else parent["children"]).pop(index)
        detached_index = root_index if parent is None else root_index + 1
        tree[detached_index:detached_index] = direct_children
        document["positions"].pop(node["instance_id"], None)
        return {
            "deleted_instance_id": node["instance_id"],
            "detached_child_instance_ids": [
                child["instance_id"] for child in direct_children
            ],
        }

    def _tree_move(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node, old_parent, old_index = _lookup(
            tree, arguments["instance_id"], label="instance_id"
        )
        if old_index is None:
            _error("draft_node_not_found", "the Draft node has no retained position")
        new_parent_id = arguments.get("new_parent_instance_id")
        new_parent = (
            _lookup(tree, new_parent_id, label="new_parent_instance_id")[0]
            if new_parent_id is not None
            else None
        )
        if new_parent is not None and _contains_instance(node, new_parent["instance_id"]):
            _error(
                "draft_tree_cycle_invalid",
                "a Draft node cannot move under itself or a descendant",
            )
        (tree if old_parent is None else old_parent["children"]).pop(old_index)
        children = tree if new_parent is None else new_parent["children"]
        index = _insert_index(arguments, len(children))
        children.insert(index, node)
        positions = arguments.get("positions")
        if positions is not None:
            document["positions"].update(deepcopy(positions))
        return {
            "moved_instance_id": node["instance_id"],
            "new_parent_instance_id": (
                new_parent["instance_id"] if new_parent is not None else None
            ),
            "index": index,
        }

    def _tree_rename(
        self,
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"instance_id", "new_instance_id"}:
            _error(
                "draft_arguments_invalid",
                "tree_draft_rename_node requires exactly instance_id and "
                "new_instance_id",
            )
        previous = arguments["instance_id"]
        replacement = arguments["new_instance_id"]
        tree = self._tree(document)
        node = _lookup_exact_instance(tree, previous)
        if (
            type(replacement) is not str
            or replacement == _ROOT_ALIAS
            or _PORTABLE_ID_PATTERN.fullmatch(replacement) is None
            or len(replacement.encode("utf-8")) > _PORTABLE_ID_MAX_BYTES
        ):
            _error(
                "draft_arguments_invalid",
                "new_instance_id must be a portable lower-case ID of at most 128 bytes",
                status_code=409,
            )
        if replacement != previous and _tree_ref(tree, replacement) is not None:
            _error(
                "draft_node_exists",
                f"Draft node already exists: {replacement}",
                status_code=409,
            )
        if replacement != previous:
            node["instance_id"] = replacement
            position = document["positions"].pop(previous, None)
            if position is not None:
                document["positions"][replacement] = position
        return {
            "previous_instance_id": previous,
            "renamed_instance_id": replacement,
        }

    def _tree_replace(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        old_node, parent, index = _lookup(
            tree, arguments["instance_id"], label="instance_id"
        )
        subtree = deepcopy(arguments["subtree"])
        if parent is None:
            assert index is not None
            tree[index] = subtree
        else:
            assert index is not None
            parent["children"][index] = subtree
        retained_ids = set(_tree_ids([subtree]))
        for instance_id in _tree_ids([old_node]):
            if instance_id not in retained_ids:
                document["positions"].pop(instance_id, None)
        return {"replacement_instance_id": subtree["instance_id"]}

    def _tree_change_kind(
        self, document: dict[str, Any], arguments: dict[str, Any], catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node = _lookup(tree, arguments["instance_id"], label="instance_id")[0]
        replacement = deepcopy(arguments["replacement"])
        new_id = replacement["node_id"]
        try:
            entry = catalog.get(new_id)
        except KeyError:
            _error(
                "draft_node_kind_unknown",
                f"replacement node_id is absent from the active catalog: {new_id}",
                status_code=404,
            )
        accepted = {parameter.id for parameter in entry.parameters}
        old_parameters = deepcopy(node.get("parameters", {}))
        old_sources = deepcopy(node.get("value_sources", {}))
        mode = arguments["parameters_mode"]
        replacement_parameters = deepcopy(replacement.get("parameters", {}))
        replacement_sources = deepcopy(replacement.get("value_sources", {}))
        if mode == "replace":
            parameters = replacement_parameters
            sources = replacement_sources
            dropped: list[str] = []
        else:
            combined_parameters = {**old_parameters, **replacement_parameters}
            combined_sources = {**old_sources, **replacement_sources}
            dropped = sorted(
                (set(combined_parameters) | set(combined_sources)) - accepted
            )
            if mode == "strict" and dropped:
                _error(
                    "draft_node_parameters_incompatible",
                    "existing parameters are incompatible with the replacement kind",
                    detail={
                        "message": "existing parameters are incompatible with the replacement kind",
                        "droppedParams": dropped,
                        "acceptedParamNames": sorted(accepted),
                    },
                )
            parameters = {
                key: value
                for key, value in combined_parameters.items()
                if key in accepted
            }
            sources = {
                key: value for key, value in combined_sources.items() if key in accepted
            }
        for name in replacement_parameters:
            sources.pop(name, None)
        for name in replacement_sources:
            parameters.pop(name, None)
        previous = node["node_id"]
        node["node_id"] = new_id
        node["parameters"] = parameters
        if sources:
            node["value_sources"] = sources
        else:
            node.pop("value_sources", None)
        return {
            "instance_id": node["instance_id"],
            "old_node_id": previous,
            "new_node_id": new_id,
            "dropped_parameters": dropped,
        }

    def _tree_wrap(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        target, parent, index = _lookup(
            tree,
            arguments["target_instance_id"],
            label="target_instance_id",
        )
        wrapper = deepcopy(arguments["wrapper"])
        if "children" in wrapper:
            _error(
                "draft_wrap_intent_invalid",
                "wrapper.children is forbidden; the target becomes its only child",
            )
        if _tree_ref(tree, wrapper["instance_id"]) is not None:
            _error(
                "draft_node_exists",
                f"Draft node already exists: {wrapper['instance_id']}",
                status_code=409,
            )
        wrapper["children"] = [target]
        if parent is None:
            assert index is not None
            tree[index] = wrapper
        else:
            assert index is not None
            parent["children"][index] = wrapper
        return {
            "target_instance_id": target["instance_id"],
            "wrapper_instance_id": wrapper["instance_id"],
        }

    @staticmethod
    def _tree_update_positions(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        if set(arguments) != {"positions"} or type(arguments["positions"]) is not dict:
            _error(
                "draft_arguments_invalid",
                "tree_draft_update_positions requires one positions object",
            )
        document["positions"].update(deepcopy(arguments["positions"]))
        return {"updated_instance_ids": sorted(arguments["positions"])}

    @staticmethod
    def _upsert_rows(
        existing: list[dict[str, Any]],
        incoming: list[dict[str, Any]],
        identity_field: str,
    ) -> tuple[list[str], list[str]]:
        by_id = {row[identity_field]: row for row in existing}
        inserted: list[str] = []
        updated: list[str] = []
        seen: set[str] = set()
        for row in incoming:
            identity = row[identity_field]
            if identity in seen:
                _error(
                    "draft_declaration_duplicate",
                    f"incoming declarations contain duplicate identity: {identity}",
                )
            seen.add(identity)
            (updated if identity in by_id else inserted).append(identity)
            by_id[identity] = deepcopy(row)
        existing[:] = [by_id[key] for key in sorted(by_id)]
        return inserted, updated

    def _parameters_upsert(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        inserted, updated = self._upsert_rows(
            document["parameters"], arguments["parameters"], "parameter_id"
        )
        return {"inserted": inserted, "updated": updated}

    def _parameters_remove(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        return self._remove_rows(
            document["parameters"],
            arguments["parameter_ids"],
            "parameter_id",
            "Program parameters",
        )

    @staticmethod
    def _blackboard(document: dict[str, Any]) -> dict[str, Any]:
        blackboard = document.get("blackboard")
        if type(blackboard) is not dict:
            document["blackboard"] = {"namespaces": [], "keys": []}
        return document["blackboard"]

    def _namespaces_upsert(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        inserted, updated = self._upsert_rows(
            self._blackboard(document)["namespaces"],
            arguments["namespaces"],
            "namespace_id",
        )
        return {"inserted": inserted, "updated": updated}

    def _namespaces_remove(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        return self._remove_rows(
            self._blackboard(document)["namespaces"],
            arguments["namespace_ids"],
            "namespace_id",
            "blackboard namespaces",
        )

    def _keys_upsert(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        inserted, updated = self._upsert_rows(
            self._blackboard(document)["keys"], arguments["keys"], "key"
        )
        return {"inserted": inserted, "updated": updated}

    def _keys_remove(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        return self._remove_rows(
            self._blackboard(document)["keys"],
            arguments["keys"],
            "key",
            "blackboard keys",
        )

    @staticmethod
    def _remove_rows(
        rows: list[dict[str, Any]],
        identities: list[str],
        identity_field: str,
        label: str,
    ) -> dict[str, Any]:
        existing = {row[identity_field] for row in rows}
        missing = sorted(set(identities) - existing)
        if missing:
            _error(
                "draft_declaration_not_found",
                f"{label} do not exist: {missing}",
                status_code=404,
            )
        removed = set(identities)
        rows[:] = [row for row in rows if row[identity_field] not in removed]
        return {"removed": list(identities)}


__all__ = [
    "ComposerDraftSession",
    "DraftMutationResult",
    "DraftSessionError",
    "OPERATOR_DRAFT_MUTATIONS",
    "SUPPORTED_DRAFT_MUTATIONS",
]
