"""Bounded in-memory Composer Draft owner with no runtime or file authority."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path
import re
import threading
import uuid
from typing import Any, Callable, NoReturn

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    compute_content_identity,
)
from manyforge_behavior.catalog import NodeCatalog
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
)


_DRAFT_API_VERSION = "manyforge.composer_draft.v1"
_WORKSPACE_API_VERSION = "manyforge.composer_program_workspace.v1"
_RETAINED_STATE_API_VERSION = "manyforge.composer_retained_draft.v1"
_PROGRAM_API_VERSION = "manyforge.program.v1"
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
    "tree_draft_replace_subtree",
    "tree_draft_update_node_params",
    "tree_draft_wrap_node",
)
OPERATOR_DRAFT_MUTATIONS = (
    "program_draft_update_node_policies",
    "viewer_draft_replace",
)
_NODE_POLICY_MUTATION = "program_draft_update_node_policies"
_PROGRAM_REVISION_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")
_PORTABLE_ID_PATTERN = re.compile(r"^[a-z][a-z0-9_.-]*$")
_PORTABLE_ID_MAX_BYTES = 128
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


def _identified(document: dict[str, Any]) -> dict[str, Any]:
    candidate = deepcopy(document)
    candidate["program_revision"] = compute_content_identity(
        candidate,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version=_PROGRAM_API_VERSION,
    )
    return candidate


def _tree_ref(
    root: dict[str, Any],
    instance_id: str,
) -> tuple[dict[str, Any], dict[str, Any] | None, int | None] | None:
    target = root.get("instance_id") if instance_id == _ROOT_ALIAS else instance_id
    pending: list[tuple[dict[str, Any], dict[str, Any] | None, int | None]] = [
        (root, None, None)
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


def _tree_ids(root: dict[str, Any]) -> list[str]:
    result: list[str] = []
    pending = [root]
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
    tree: dict[str, Any],
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
    tree: dict[str, Any],
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
    pending = [tree]
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
) -> tuple[str, str, dict[str, str], dict[str, Any], str]:
    expected_fields = {
        "expected_program_revision",
        "instance_id",
        "modification_policy",
        "recovery_policy",
        "operator_stop_policy",
    }
    if set(arguments) != expected_fields:
        _error(
            "draft_arguments_invalid",
            f"{_NODE_POLICY_MUTATION} requires exactly "
            f"{sorted(expected_fields)}",
        )

    expected_program_revision = arguments["expected_program_revision"]
    if (
        type(expected_program_revision) is not str
        or _PROGRAM_REVISION_PATTERN.fullmatch(expected_program_revision) is None
    ):
        _error(
            "draft_arguments_invalid",
            "expected_program_revision must be an exact SHA-256 identity",
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
        expected_program_revision,
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
    return _tree_ref(root, instance_id) is not None


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
    """Own exactly one validated in-memory Program Draft."""

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
        self._program = program
        self._revision = 1 if program is not None else 0
        self._saved_path = resolved_source
        self._saved_program_revision = (
            program.program_revision if resolved_source is not None else None
        )
        self._saved_document = (
            program.to_dict() if resolved_source is not None else None
        )

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
            return self._program is not None

    def retained_state(self) -> dict[str, Any]:
        """Return the complete bounded local owner state needed after restart."""

        with self._lock:
            return {
                "schema_version": _RETAINED_STATE_API_VERSION,
                "draft_id": self._draft_id,
                "draft_revision": self._revision,
                "program": (
                    self._program.to_dict() if self._program is not None else None
                ),
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
            "program",
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
        document = state["program"]
        saved_path = state["saved_path"]
        saved_revision = state["saved_program_revision"]
        saved_document = state["saved_document"]
        if (
            state["schema_version"] != _RETAINED_STATE_API_VERSION
            or type(draft_id) is not str
            or not draft_id
            or type(revision) is not int
            or not 0 <= revision <= _MAXIMUM_DRAFT_REVISION
            or (document is None) != (revision == 0)
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
        if document is not None:
            try:
                program = Program.from_dict(
                    deepcopy(document), catalog, path="<composer-retained-draft>"
                )
            except ProgramValidationError as error:
                _error(
                    "composer_retained_draft_invalid",
                    f"retained Composer Draft is invalid: {error}",
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
            self._draft_id = draft_id
            self._revision = revision
            self._program = program
            self._saved_path = resolved_saved_path
            self._saved_program_revision = saved_revision
            self._saved_document = deepcopy(saved_document)

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
        """Revalidate the complete Draft against one exact active catalog."""

        with self._lock:
            if self._program is None:
                _error(
                    "composer_draft_unavailable",
                    "Composer has no loaded Program Draft",
                    status_code=404,
                )
            document = self._program.to_dict()
            try:
                Program.from_dict(document, catalog, path="<composer-draft>")
            except ProgramValidationError as error:
                _error(
                    "composer_draft_validation_failed",
                    str(error),
                    detail=str(error),
                )
            return self._snapshot_locked()

    def _snapshot_locked(self) -> dict[str, Any]:
        program = self._program
        matches_saved = self._matches_saved_locked()
        return {
            "schema_version": _DRAFT_API_VERSION,
            "draft_id": self._draft_id,
            "draft_revision": self._revision,
            "program_loaded": program is not None,
            "program_revision": (
                program.program_revision if program is not None else None
            ),
            "program": program.to_dict() if program is not None else None,
            "saved": matches_saved,
            "applied": False,
        }

    def _matches_saved_locked(self) -> bool:
        return (
            self._program is not None
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
        applicability = draft_applicability(program_document) if program_document is not None else {
            "applyReady": True,
            "affectedInstanceIds": [],
            "unavailableReason": None,
        }
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
                "programLoaded": program is not None,
                "programRevision": (
                    program.program_revision if program is not None else None
                ),
                "program": program_document,
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
            "hasUnsavedChanges": program is not None and not matches_saved,
            "hasUnappliedChanges": (
                program is not None and not matches_applied
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
                self._program is not None
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
                self._program is not None
                and not self._matches_saved_locked()
                and not discard_changes
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "opening another Program would discard unsaved Draft changes",
                    status_code=409,
                )
            self._draft_id = f"draft-{uuid.uuid4().hex}"
            self._program = program
            self._revision = 1
            self._saved_path = str(Path(source_path).resolve())
            self._saved_program_revision = program.program_revision
            self._saved_document = program.to_dict()
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
                    "composer_draft_unavailable",
                    "Composer has no loaded Program Draft",
                    status_code=404,
                )
            return applier(self._program)

    def load_and_apply_program(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        discard_changes: bool,
        current_applied_program_revision: str | None,
        loader: Callable[[], tuple[Program, str]],
        applier: Callable[[Program], dict[str, Any]],
        replacement_draft_id: str | None = None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Run normative Load under one Draft fence; publish locally last."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            has_unapplied_changes = (
                self._program is not None
                and self._program.program_revision
                != current_applied_program_revision
            )
            if (
                self._program is not None
                and (not self._matches_saved_locked() or has_unapplied_changes)
                and not discard_changes
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "loading another Program would discard unsaved or unapplied "
                    "Draft changes",
                    status_code=409,
                )
            program, source_path = loader()
            result = applier(program)
            self._draft_id = replacement_draft_id or f"draft-{uuid.uuid4().hex}"
            self._program = program
            self._revision = 1
            self._saved_path = str(Path(source_path).resolve())
            self._saved_program_revision = program.program_revision
            self._saved_document = program.to_dict()
            return result, self._workspace_snapshot_locked(
                applied_program_revision=program.program_revision,
            )

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
                    "composer_draft_unavailable",
                    "Composer has no loaded Program Draft",
                    status_code=404,
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
        """Create one fresh, validated minimal Program under the old fence."""

        with self._lock:
            self._require_identity_locked(
                draft_id=draft_id,
                expected_revision=expected_revision,
            )
            if (
                self._program is not None
                and not self._matches_saved_locked()
                and not confirm_discard
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "creating a new Program would discard unsaved Draft changes",
                    status_code=409,
                )
            if (
                self._program is not None
                and not confirm_discard
                and (applied_revision := applied_revision_reader()) is not None
                and self._program.program_revision != applied_revision
            ):
                _error(
                    "composer_draft_discard_confirmation_required",
                    "creating a new Program would discard unapplied Draft changes",
                    status_code=409,
                )

            catalog = catalog_reader()
            document = _identified(
                {
                    "api_version": _PROGRAM_API_VERSION,
                    "program_id": f"program-{uuid.uuid4().hex}",
                    "program_revision": "sha256:" + ("0" * 64),
                    "program_profile": "composable-cell-v1",
                    "name": "Untitled Program",
                    "catalog": {
                        "api_version": catalog.api_version,
                        "content_version": catalog.content_version,
                    },
                    "component_roles": {"component": {}},
                    "target_roles": {},
                    "tree": {
                        "node_id": "logic.check_bool",
                        "instance_id": "root",
                        "parameters": {
                            "actual_value": True,
                            "expected_value": True,
                        },
                        "children": [],
                    },
                }
            )
            try:
                program = Program.from_dict(
                    document,
                    catalog,
                    path="<composer-new-program>",
                )
            except ProgramValidationError as error:
                _error(
                    "composer_new_program_validation_failed",
                    str(error),
                    status_code=503,
                    detail=str(error),
                )

            self._draft_id = f"draft-{uuid.uuid4().hex}"
            self._program = program
            self._revision = 1
            self._saved_path = None
            self._saved_program_revision = None
            self._saved_document = None
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
            self._program = applied
            self._revision += 1
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
            draft_matches_saved = (
                self._program is not None
                and self._program.to_dict() == saved_document
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
                    self._program = validated_saved
                    self._revision += 1
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
                self._program = validated_saved
                self._revision += 1
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
            if self._program is None:
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
            if tool_id == _NODE_POLICY_MUTATION:
                expected_program_revision = arguments.get("expected_program_revision")
                if (
                    type(expected_program_revision) is not str
                    or _PROGRAM_REVISION_PATTERN.fullmatch(expected_program_revision)
                    is None
                ):
                    _error(
                        "draft_arguments_invalid",
                        "expected_program_revision must be an exact SHA-256 identity",
                    )
                if expected_program_revision != self._program.program_revision:
                    _error(
                        "composer_program_revision_stale",
                        "request Program revision is stale",
                        status_code=409,
                        detail={
                            "message": "request Program revision is stale",
                            "expectedProgramRevision": self._program.program_revision,
                            "receivedProgramRevision": expected_program_revision,
                        },
                    )
            document = self._program.to_dict()
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
                identified = _identified(document)
            except (StrictArtifactError, TypeError, ValueError) as error:
                _error(
                    "composer_draft_validation_failed",
                    str(error),
                    detail=str(error),
                )
            try:
                candidate = Program.from_dict(
                    identified,
                    catalog,
                    path="<composer-draft>",
                )
            except ProgramValidationError as error:
                _error(
                    "composer_draft_validation_failed",
                    str(error),
                    detail=str(error),
                )
            if (
                tool_id == _NODE_POLICY_MUTATION
                and candidate.to_dict() == self._program.to_dict()
            ):
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
            self._program = candidate
            self._revision += 1
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
            "scene_draft_add_object": self._scene_add,
            "scene_draft_update_object": self._scene_update,
            "scene_draft_upsert_objects": self._scene_upsert,
            "scene_draft_remove_objects": self._scene_remove,
            "viewer_draft_replace": self._viewer_replace,
            "tree_draft_insert_node": self._tree_insert,
            "tree_draft_update_node_params": self._tree_update,
            "tree_draft_delete_node": self._tree_delete,
            "tree_draft_move_node": self._tree_move,
            "tree_draft_replace_subtree": self._tree_replace,
            "tree_draft_change_node_kind": self._tree_change_kind,
            "tree_draft_wrap_node": self._tree_wrap,
            "program_draft_upsert_parameters": self._parameters_upsert,
            "program_draft_remove_parameters": self._parameters_remove,
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
    def _node_policies_update(
        document: dict[str, Any],
        arguments: dict[str, Any],
        _catalog: NodeCatalog,
    ) -> dict[str, Any]:
        (
            _expected_program_revision,
            instance_id,
            modification,
            recovery,
            operator_stop,
        ) = _node_policy_arguments(arguments)
        node = _lookup_exact_instance(document["tree"], instance_id)
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
    def _tree(document: dict[str, Any]) -> dict[str, Any]:
        tree = document.get("tree")
        if type(tree) is not dict:
            _error("composer_draft_invalid", "Program Draft tree is not an object")
        return tree

    def _tree_insert(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node = deepcopy(arguments["node"])
        if _tree_ref(tree, node["instance_id"]) is not None:
            _error(
                "draft_node_exists",
                f"Draft node already exists: {node['instance_id']}",
                status_code=409,
            )
        parent_id = arguments.get("parent_instance_id")
        index: int
        if parent_id is not None:
            parent = _lookup(tree, parent_id, label="parent_instance_id")[0]
            children = parent["children"]
            index = _insert_index(arguments, len(children))
        else:
            before = arguments.get("before_instance_id")
            after = arguments.get("after_instance_id")
            sibling_id = before if before is not None else after
            _sibling, parent, sibling_index = _lookup(
                tree, sibling_id, label="sibling instance"
            )
            if parent is None or sibling_index is None:
                _error(
                    "draft_insert_position_invalid",
                    "cannot insert before or after the Draft root",
                )
            children = parent["children"]
            index = sibling_index if before is not None else sibling_index + 1
        children.insert(index, node)
        return {
            "inserted_instance_id": node["instance_id"],
            "index": index,
            "parent_instance_id": parent["instance_id"],
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
        if parent is None or index is None:
            _error("draft_root_edit_invalid", "the Draft root cannot be deleted")
        parent["children"].pop(index)
        return {"deleted_instance_id": node["instance_id"]}

    def _tree_move(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        node, old_parent, old_index = _lookup(
            tree, arguments["instance_id"], label="instance_id"
        )
        if old_parent is None or old_index is None:
            _error("draft_root_edit_invalid", "the Draft root cannot be moved")
        new_parent = _lookup(
            tree,
            arguments["new_parent_instance_id"],
            label="new_parent_instance_id",
        )[0]
        if _contains_instance(node, new_parent["instance_id"]):
            _error(
                "draft_tree_cycle_invalid",
                "a Draft node cannot move under itself or a descendant",
            )
        old_parent["children"].pop(old_index)
        children = new_parent["children"]
        index = _insert_index(arguments, len(children))
        children.insert(index, node)
        return {
            "moved_instance_id": node["instance_id"],
            "new_parent_instance_id": new_parent["instance_id"],
            "index": index,
        }

    def _tree_replace(
        self, document: dict[str, Any], arguments: dict[str, Any], _catalog: NodeCatalog
    ) -> dict[str, Any]:
        tree = self._tree(document)
        _node, parent, index = _lookup(
            tree, arguments["instance_id"], label="instance_id"
        )
        subtree = deepcopy(arguments["subtree"])
        if parent is None:
            document["tree"] = subtree
        else:
            assert index is not None
            parent["children"][index] = subtree
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
            document["tree"] = wrapper
        else:
            assert index is not None
            parent["children"][index] = wrapper
        return {
            "target_instance_id": target["instance_id"],
            "wrapper_instance_id": wrapper["instance_id"],
        }

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
