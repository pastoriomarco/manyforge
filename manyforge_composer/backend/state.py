"""ComposerState - Holds the runtime state shared across all API routes."""
from __future__ import annotations

import atexit
import copy
import hashlib
import logging
import math
import os
import threading
import time
import uuid
import weakref
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional
from urllib.parse import urlparse

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import (
    Program,
    apply_tree_param_overrides,
    compute_program_tree_hash,
    find_tree_node_by_name,
    merge_param_patch,
)
from manyforge_behavior.deployment import Deployment, PoseSourceSceneAdmissionConfig
from manyforge_behavior.cycle_manager import CycleManager
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.kernel_client import (
    KernelClient,
    SceneEditRequest,
    SceneObjectSnapshot,
    SceneShape,
)
from manyforge_behavior.skill_loader import SkillCatalog, load_skill_catalog, default_skills_dir
from .event_recorder import EventRecorder
from .log_throttle import ThrottledLog
from .security import normalize_allowed_roots

logger = logging.getLogger(__name__)

# Audit 6F: bounded hot-path diagnostics (one record per window per key).
_log_throttle = ThrottledLog(min_interval_s=5.0)
_MAX_ASSISTANT_REQUEST_RECORDS = 200
_MAX_ASSISTANT_PROPOSAL_RECORDS = 500
_COMPOSER_STATE_INSTANCES: weakref.WeakSet[Any] = weakref.WeakSet()


def stop_all_visual_state_publishers(*, timeout_s: float = 0.5) -> None:
    for state in list(_COMPOSER_STATE_INSTANCES):
        try:
            state.stop_visual_state_publisher(timeout_s=timeout_s)
        except Exception:
            pass


atexit.register(stop_all_visual_state_publishers)


def _format_stop_capability_issue(issue: dict) -> str:
    profile = issue.get("profile") or "unknown_profile"
    mode = issue.get("stop_mode")
    mode_text = mode if isinstance(mode, str) and mode else "missing"
    reason = issue.get("reason")
    if isinstance(reason, str) and reason:
        return f"{profile}={mode_text} ({reason})"
    return f"{profile}={mode_text}"


def _normalize_scene_snapshot_base_url(value: str, *, field_name: str) -> str:
    candidate = value.strip().rstrip("/")
    if not candidate:
        raise RuntimeError(f"{field_name} is empty")

    parsed = urlparse(candidate)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise RuntimeError(
            f"{field_name} must be an absolute http(s) URL with a host"
        )
    if parsed.username or parsed.password:
        raise RuntimeError(
            f"{field_name} must not include embedded credentials"
        )
    return candidate


def _pose_to_matrix(pos: list[float], orient_xyzw: list[float]) -> list[list[float]]:
    """Build a rigid 4x4 transform from a position and xyzw quaternion."""
    x, y, z, w = orient_xyzw
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm <= 0.0 or not math.isfinite(norm):
        raise ValueError("orientation quaternion is not finite")
    x, y, z, w = x / norm, y / norm, z / norm, w / norm
    return [
        [1 - 2 * y * y - 2 * z * z, 2 * x * y - 2 * z * w, 2 * x * z + 2 * y * w, pos[0]],
        [2 * x * y + 2 * z * w, 1 - 2 * x * x - 2 * z * z, 2 * y * z - 2 * x * w, pos[1]],
        [2 * x * z - 2 * y * w, 2 * y * z + 2 * x * w, 1 - 2 * x * x - 2 * y * y, pos[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _matrix_to_pose(matrix: list[list[float]]) -> tuple[list[float], list[float]]:
    """Extract position and xyzw quaternion from a rigid 4x4 transform."""
    pos = [matrix[0][3], matrix[1][3], matrix[2][3]]
    trace = matrix[0][0] + matrix[1][1] + matrix[2][2]
    if trace > 0.0:
        s = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * s
        x = (matrix[2][1] - matrix[1][2]) / s
        y = (matrix[0][2] - matrix[2][0]) / s
        z = (matrix[1][0] - matrix[0][1]) / s
    elif matrix[0][0] > matrix[1][1] and matrix[0][0] > matrix[2][2]:
        s = math.sqrt(1.0 + matrix[0][0] - matrix[1][1] - matrix[2][2]) * 2.0
        w = (matrix[2][1] - matrix[1][2]) / s
        x = 0.25 * s
        y = (matrix[0][1] + matrix[1][0]) / s
        z = (matrix[0][2] + matrix[2][0]) / s
    elif matrix[1][1] > matrix[2][2]:
        s = math.sqrt(1.0 + matrix[1][1] - matrix[0][0] - matrix[2][2]) * 2.0
        w = (matrix[0][2] - matrix[2][0]) / s
        x = (matrix[0][1] + matrix[1][0]) / s
        y = 0.25 * s
        z = (matrix[1][2] + matrix[2][1]) / s
    else:
        s = math.sqrt(1.0 + matrix[2][2] - matrix[0][0] - matrix[1][1]) * 2.0
        w = (matrix[1][0] - matrix[0][1]) / s
        x = (matrix[0][2] + matrix[2][0]) / s
        y = (matrix[1][2] + matrix[2][1]) / s
        z = 0.25 * s
    norm = math.sqrt(x * x + y * y + z * z + w * w)
    if norm <= 0.0 or not math.isfinite(norm):
        raise ValueError("computed quaternion is not finite")
    return pos, [x / norm, y / norm, z / norm, w / norm]


def _multiply_matrices(a: list[list[float]], b: list[list[float]]) -> list[list[float]]:
    return [
        [sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
        for i in range(4)
    ]


def _invert_matrix(matrix: list[list[float]]) -> list[list[float]]:
    rt = [[matrix[j][i] for j in range(3)] for i in range(3)]
    t = [matrix[0][3], matrix[1][3], matrix[2][3]]
    nt = [-(rt[i][0] * t[0] + rt[i][1] * t[1] + rt[i][2] * t[2]) for i in range(3)]
    return [
        [rt[0][0], rt[0][1], rt[0][2], nt[0]],
        [rt[1][0], rt[1][1], rt[1][2], nt[1]],
        [rt[2][0], rt[2][1], rt[2][2], nt[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _relative_pose_delta(
    baseline_pose: dict[str, Any],
    observed_pose: dict[str, Any],
) -> tuple[float, float]:
    baseline_matrix = _pose_to_matrix(
        list(baseline_pose["position_m"]),
        list(baseline_pose["orientation_xyzw"]),
    )
    observed_matrix = _pose_to_matrix(
        list(observed_pose["position_m"]),
        list(observed_pose["orientation_xyzw"]),
    )
    delta_position, delta_orientation = _matrix_to_pose(
        _multiply_matrices(_invert_matrix(baseline_matrix), observed_matrix)
    )
    translation_m = math.sqrt(sum(float(value) * float(value) for value in delta_position))
    x, y, z, w = [float(value) for value in delta_orientation]
    vector_norm = math.sqrt(x * x + y * y + z * z)
    rotation_rad = 2.0 * math.atan2(vector_norm, abs(w))
    return translation_m, rotation_rad


@dataclass
class RuntimeOverrideRecord:
    node_name: str
    params: dict[str, Any]
    authority_source: str
    approval_mode: str
    reason: str
    session_id: Optional[str]
    trigger_node_instance: Optional[str]
    base_program_hash: str = ""
    requested_at_ns: int = field(default_factory=time.monotonic_ns)
    activation_state: str = "active"  # active | staged
    activated_at_ns: Optional[int] = None


@dataclass
class PublishedSceneSnapshot:
    snapshot_id: str
    image_bytes: bytes
    media_type: str
    width: int
    height: int
    format: str
    sha256: str
    created_at_ns: int = field(default_factory=time.monotonic_ns)


@dataclass
class AssistantProposalRecord:
    proposal_id: str
    conversation_id: str
    proposal: dict[str, Any]
    source: dict[str, Any] = field(default_factory=dict)
    lifecycle_status: str = "staged"
    created_at_ns: int = field(default_factory=time.monotonic_ns)
    updated_at_ns: int = field(default_factory=time.monotonic_ns)


@dataclass
class LiveToolCallEntry:
    """A single tool call observed during an in-flight assistant request.

    Populated as the bridge endpoint executes each tool, so the UI can
    render tool calls in real time instead of waiting for the run to
    complete. The audit log keeps the authoritative record; this is
    the operator-facing view of "what's the assistant doing right now".
    """

    tool_id: str
    status: str           # "running" | "completed" | "failed"
    started_at_ns: int
    finished_at_ns: Optional[int] = None
    args_summary: str = ""        # truncated JSON, never raw secrets
    result_summary: str = ""      # truncated JSON, never raw secrets
    error: Optional[str] = None
    draft_mutated: bool = False


@dataclass
class AssistantRequestRecord:
    request_id: str
    conversation_id: str
    provider_id: str
    assistant_mode: Optional[str] = None
    status: str = "running"
    message: str = ""
    error: dict[str, Any] | None = None
    warnings: list[str] = field(default_factory=list)
    draft_mutated: bool = False
    field_policy: dict[str, Any] = field(default_factory=dict)
    tool_call_count: int = 0
    tool_calls: list[LiveToolCallEntry] = field(default_factory=list)
    started_at_ns: int = field(default_factory=time.monotonic_ns)
    updated_at_ns: int = field(default_factory=time.monotonic_ns)
    finished_at_ns: Optional[int] = None


class ComposerState:
    """Holds the runtime state shared across all API routes."""

    def __init__(
        self,
        catalog_path: str,
        kernel: KernelClient,
        kernel_factory: Optional[Callable[[Optional[Deployment]], tuple[KernelClient, Any]]] = None,
        kernel_bundle: Any = None,
        trace_dir: str | None = None,
        failure_archive_dir: str | None = None,
        failure_archive_enabled: bool = True,
        failure_archive_max_age_s: float | None = None,
        failure_archive_max_bytes: int | None = None,
        trace_max_age_s: float | None = None,
        trace_max_bytes: int | None = None,
        allowed_fs_roots: list[str] | None = None,
    ):
        self.catalog = NodeCatalog(catalog_path)
        self._catalog_path = str(Path(catalog_path).resolve())
        self._kernel = kernel
        self._kernel_factory = kernel_factory
        self.kernel_bundle = kernel_bundle
        self.program: Optional[Program] = None
        self._effective_program: Optional[Program] = None
        self.program_source_path: str = ""
        self.deployment: Optional[Deployment] = None
        self.deployment_source_path: str = ""
        self.cycle_manager: Optional[CycleManager] = None
        self.public_api_base_url: str | None = None
        self._on_program_loaded: Optional[Callable[["ComposerState"], None]] = None
        self.started_at_ns = time.monotonic_ns()
        self.event_recorder = EventRecorder(
            trace_dir=trace_dir,
            failure_archive_dir=failure_archive_dir,
            failure_archive_enabled=failure_archive_enabled,
            failure_archive_max_age_s=failure_archive_max_age_s,
            failure_archive_max_bytes=failure_archive_max_bytes,
            trace_max_age_s=trace_max_age_s,
            trace_max_bytes=trace_max_bytes,
        )
        self._initialize_kernel_observability(trace_dir)
        self.scene_edit_lock = threading.Lock()
        # Viewer-side visibility state. ComposerState is the source of truth
        # for the native viewer. Robot/tool frames default visible. Object
        # frames default hidden and are opt-in so runtime scene additions do
        # not clutter the operator view.
        self._scene_viewer_lock = threading.Lock()
        # Explicit operator overrides. A frame in `_hidden_frames` is
        # forced off; a frame in `_explicit_visible_frames` is forced
        # on; otherwise the caller-supplied default (computed from
        # whether the frame carries a semantic role) decides.
        self._hidden_frames: set[str] = set()
        self._explicit_visible_frames: set[str] = set()
        self._visible_object_frames: set[str] = set()
        self._frame_gizmo_size_m: float = 0.1
        self._object_axes_gizmo_size_m: float = 0.1
        # Drivable joint metadata + last-known joint vector. The visual
        # publisher runs concurrently with planning/validation and must not
        # poll RoboPlan's mutable scene for stable group metadata on every
        # frame: native get_joint_names() is protected by the planner scene
        # mutex and can queue behind collision validation. Cache the ordered
        # joint list at runtime setup, then use execution snapshots for live
        # positions.
        self._joint_state_lock = threading.Lock()
        self._drivable_joint_names: list[str] = []
        self._last_known_joint_positions_rad: list[float] | None = None
        if kernel is not None:
            self.refresh_drivable_joint_names(kernel=kernel)
        self.allowed_fs_roots = normalize_allowed_roots(allowed_fs_roots)
        self._runtime_overrides: dict[str, RuntimeOverrideRecord] = {}
        self._runtime_override_lock = threading.RLock()
        self._runtime_override_revision = 0
        self._runtime_override_next_tree_cache_key: tuple[int, str, int] | None = None
        self._runtime_override_next_tree_cache: dict[str, Any] | None = None
        # Serializes read-modify-write edits of self.program. Draft/tool
        # handlers may hold this while calling apply_models(), so it must be
        # reentrant.
        self._program_edit_lock = threading.RLock()
        self._program_tree_hash: str = ""
        self._program_session_generation: int = 0
        self._scene_snapshot_lock = threading.Lock()
        self._published_scene_snapshots: dict[str, PublishedSceneSnapshot] = {}
        self.assistant_provider: Any = None
        self._assistant_proposal_lock = threading.Lock()
        self._assistant_proposals: dict[str, AssistantProposalRecord] = {}
        self._assistant_request_lock = threading.Lock()
        self._assistant_requests: dict[str, AssistantRequestRecord] = {}
        # Maps an OpenClaw bridge `principal` to the chat request it is
        # currently servicing. The bridge service registers this just
        # before forwarding a chat to OpenClaw so the MCP bridge
        # subprocess (which carries its own bridge-process conversation
        # id, not the chat's) can still be correlated back to the
        # active chat for live tool-call streaming. Single-tenant per
        # sandbox so this is one-entry-per-principal.
        self._assistant_principal_bindings: dict[str, str] = {}
        # Per-conversation consecutive-failure counter. Incremented on
        # every bridge tool-call rejection, reset on every success.
        # Used by the bridge endpoint to rotate the recovery nudge it
        # appends to error responses, so consecutive failures of the
        # same class produce different prefixes for the model and break
        # repetition loops on thinking-off configurations.
        self._assistant_failure_counts: dict[str, int] = {}
        self._external_runtime_status_lock = threading.Lock()
        self._external_runtime_statuses: dict[str, dict[str, Any]] = {}
        self._simulation_control_lock = threading.Lock()
        self._pose_source_lock = threading.Lock()
        self._pose_source_snapshots: dict[str, dict[str, Any]] = {}
        self._pose_source_drift_lock = threading.Lock()
        self._pose_source_drift_baselines: dict[tuple[str, str], dict[str, Any]] = {}
        self._pose_source_drift_states: dict[tuple[str, str], dict[str, Any]] = {}
        self._pose_source_drift_exceeded_since_ns: dict[tuple[str, str], int] = {}
        self._pose_source_drift_raised: set[tuple[str, str]] = set()
        self._pose_source_apply_lock = threading.Lock()
        self._pose_source_apply_pending: dict[str, dict[str, Any]] = {}
        self._pose_source_apply_running = False
        self._pose_source_apply_last_result: dict[str, Any] | None = None
        self._pose_source_apply_generation = 0
        self._pose_source_apply_signatures: dict[
            tuple[str, str, str],
            dict[str, tuple[Any, ...]],
        ] = {}
        self._pose_source_apply_metrics: dict[str, int] = {
            "snapshots_received": 0,
            "apply_coalesced": 0,
            "geometry_applied": 0,
            "freshness_only_applied": 0,
            "no_op_skipped": 0,
            "stale_generation_dropped": 0,
            "errors": 0,
        }
        self._pose_source_apply_object_metrics: dict[str, dict[str, int]] = {}
        self._visual_scene_objects_lock = threading.Lock()
        self._visual_scene_objects: list[SceneObjectSnapshot] = []
        self._visual_scene_object_attachment_offsets: dict[str, dict[str, Any]] = {}
        self._visual_scene_objects_timestamp_ns = 0
        self._visual_scene_objects_refresh_attempt_ns = 0
        self._visual_scene_objects_refresh_running = False
        self._visual_scene_objects_refresh_error: str | None = None
        self._visual_state_lock = threading.Lock()
        self._visual_state_snapshot: Any | None = None
        self._visual_state_timestamp_ns = 0
        self._visual_state_last_duration_ms: float | None = None
        self._visual_state_last_error: str | None = None
        self._visual_state_last_error_detail: str | None = None
        self._visual_state_last_reason: str = ""
        self._visual_state_next_reason: str = ""
        self._visual_state_publisher_running = False
        self._visual_state_publisher_thread: threading.Thread | None = None
        self._visual_state_publisher_wake = threading.Event()
        self._visual_state_publisher_stop = threading.Event()
        self._visual_state_publisher_builder: Callable[[], Any] | None = None
        self._visual_state_publisher_period_s = 1.0 / 15.0
        self.skill_catalog: SkillCatalog = load_skill_catalog(
            default_skills_dir(),
            catalog=self.catalog,
            strict=True,
        )
        _COMPOSER_STATE_INSTANCES.add(self)

    def _prune_assistant_requests_locked(self) -> None:
        if len(self._assistant_requests) <= _MAX_ASSISTANT_REQUEST_RECORDS:
            return
        removable = sorted(
            (
                record
                for record in self._assistant_requests.values()
                if record.status not in {"running", "cancel_requested"}
            ),
            key=lambda record: (record.updated_at_ns, record.request_id),
        )
        for record in removable:
            if len(self._assistant_requests) <= _MAX_ASSISTANT_REQUEST_RECORDS:
                break
            self._assistant_requests.pop(record.request_id, None)
            for principal, request_id in list(self._assistant_principal_bindings.items()):
                if request_id == record.request_id:
                    self._assistant_principal_bindings.pop(principal, None)
        if len(self._assistant_failure_counts) > _MAX_ASSISTANT_REQUEST_RECORDS:
            active_conversations = {
                record.conversation_id
                for record in self._assistant_requests.values()
                if record.status in {"running", "cancel_requested"}
            }
            for conversation_id in list(self._assistant_failure_counts.keys()):
                if conversation_id not in active_conversations:
                    self._assistant_failure_counts.pop(conversation_id, None)
                if len(self._assistant_failure_counts) <= _MAX_ASSISTANT_REQUEST_RECORDS:
                    break

    def _prune_assistant_proposals_locked(self) -> None:
        if len(self._assistant_proposals) <= _MAX_ASSISTANT_PROPOSAL_RECORDS:
            return
        removable = sorted(
            (
                record
                for record in self._assistant_proposals.values()
                if record.lifecycle_status != "staged"
            ),
            key=lambda record: (record.updated_at_ns, record.proposal_id),
        )
        for record in removable:
            if len(self._assistant_proposals) <= _MAX_ASSISTANT_PROPOSAL_RECORDS:
                break
            self._assistant_proposals.pop(record.proposal_id, None)
        if len(self._assistant_proposals) <= _MAX_ASSISTANT_PROPOSAL_RECORDS:
            return
        staged = sorted(
            self._assistant_proposals.values(),
            key=lambda record: (record.created_at_ns, record.proposal_id),
        )
        for record in staged:
            if len(self._assistant_proposals) <= _MAX_ASSISTANT_PROPOSAL_RECORDS:
                break
            self._assistant_proposals.pop(record.proposal_id, None)

    def bump_assistant_failure(self, conversation_id: str) -> int:
        """Increment and return the consecutive-failure counter for a conversation.

        Returns the value AFTER the increment. The first failure in a
        fresh conversation returns 1, the second returns 2, and so on.
        Conversations missing or empty count under the empty-string key.
        """
        key = conversation_id or ""
        with self._assistant_request_lock:
            current = self._assistant_failure_counts.get(key, 0) + 1
            self._assistant_failure_counts[key] = current
            return current

    def reset_assistant_failure(self, conversation_id: str) -> None:
        """Reset the consecutive-failure counter for a conversation."""
        key = conversation_id or ""
        with self._assistant_request_lock:
            self._assistant_failure_counts.pop(key, None)

    def get_assistant_tool_history(
        self,
        *,
        conversation_id: str,
        principal: str = "",
        limit: int = 20,
    ) -> list[dict[str, Any]]:
        """Return a compact summary of recent tool calls on the active request.

        Resolution mirrors record_assistant_request_tool_call: try
        conversation_id first, fall back to principal binding. Each
        returned entry carries {tool, status, error_class} so the
        bridge can build an impasse summary without re-classifying.
        Order is oldest-first; most recent at the end.
        """
        with self._assistant_request_lock:
            record = None
            if conversation_id:
                for candidate in self._assistant_requests.values():
                    if candidate.conversation_id == conversation_id and candidate.status in (
                        "running",
                        "cancel_requested",
                    ):
                        record = candidate
                        break
            if record is None and principal:
                bound = self._assistant_principal_bindings.get(principal)
                if bound is not None:
                    candidate = self._assistant_requests.get(bound)
                    if candidate is not None and candidate.status in (
                        "running",
                        "cancel_requested",
                    ):
                        record = candidate
            if record is None:
                return []
            entries = list(record.tool_calls)[-limit:]
        # Don't classify here — the recovery module owns the rules.
        # Return raw fields and let the caller classify.
        result: list[dict[str, Any]] = []
        for entry in entries:
            result.append(
                {
                    "tool": entry.tool_id,
                    "status": entry.status,
                    "error": entry.error,
                    "draft_mutated": entry.draft_mutated,
                }
            )
        return result

    def start_assistant_request(
        self,
        *,
        request_id: str,
        conversation_id: str,
        provider_id: str,
        assistant_mode: str | None = None,
        field_policy: dict[str, Any] | None = None,
    ) -> AssistantRequestRecord:
        now_ns = time.monotonic_ns()
        record = AssistantRequestRecord(
            request_id=request_id,
            conversation_id=conversation_id,
            provider_id=provider_id,
            assistant_mode=assistant_mode,
            field_policy=dict(field_policy or {}),
            status="running",
            started_at_ns=now_ns,
            updated_at_ns=now_ns,
        )
        with self._assistant_request_lock:
            self._assistant_requests[request_id] = record
            self._prune_assistant_requests_locked()
        return record

    def assistant_request_field_policy(
        self,
        *,
        request_id: str = "",
        conversation_id: str = "",
        principal: str = "",
    ) -> dict[str, Any]:
        """Return the field policy bound to an active assistant request."""
        with self._assistant_request_lock:
            if request_id:
                record = self._assistant_requests.get(request_id)
                if record is not None:
                    return dict(record.field_policy)
            if conversation_id:
                for record in self._assistant_requests.values():
                    if (
                        record.conversation_id == conversation_id
                        and record.status in ("running", "cancel_requested")
                    ):
                        return dict(record.field_policy)
            if principal:
                bound_request_id = self._assistant_principal_bindings.get(principal)
                if bound_request_id is not None:
                    record = self._assistant_requests.get(bound_request_id)
                    if record is not None:
                        return dict(record.field_policy)
        return {}

    def finish_assistant_request(
        self,
        request_id: str,
        *,
        status: str,
        message: str = "",
        error: dict[str, Any] | None = None,
        warnings: list[str] | None = None,
        draft_mutated: bool = False,
        tool_call_count: int = 0,
    ) -> AssistantRequestRecord | None:
        now_ns = time.monotonic_ns()
        with self._assistant_request_lock:
            record = self._assistant_requests.get(request_id)
            if record is None:
                return None
            record.status = status
            record.message = message
            record.error = error
            record.warnings = list(warnings or [])
            record.draft_mutated = draft_mutated
            record.tool_call_count = tool_call_count
            record.updated_at_ns = now_ns
            record.finished_at_ns = now_ns
            self._prune_assistant_requests_locked()
            return record

    def bind_principal_to_request(
        self,
        *,
        principal: str,
        request_id: str,
    ) -> None:
        """Tell Composer that bridge tool calls bearing this principal
        belong to this in-flight chat assistant request.

        Used by the OpenClaw bridge service: the MCP bridge subprocess
        substitutes its own bridge-process conversation id in tool-call
        envelopes, so we can't correlate by conversationId alone. The
        bridge service knows both the chat and the principal, so it
        registers the binding here and the bridge endpoint can resolve
        principal → request when the conversationId doesn't match.
        Single-tenant per sandbox: one binding per principal.
        """
        principal = (principal or "").strip()
        if not principal:
            return
        with self._assistant_request_lock:
            self._assistant_principal_bindings[principal] = request_id

    def unbind_principal(self, principal: str) -> None:
        principal = (principal or "").strip()
        if not principal:
            return
        with self._assistant_request_lock:
            self._assistant_principal_bindings.pop(principal, None)

    def record_assistant_request_tool_call(
        self,
        *,
        conversation_id: str,
        tool_id: str,
        status: str,
        args_summary: str = "",
        result_summary: str = "",
        error: Optional[str] = None,
        draft_mutated: bool = False,
        started_at_ns: Optional[int] = None,
        principal: str = "",
    ) -> Optional[LiveToolCallEntry]:
        """Append a live tool-call entry to the in-flight assistant request
        that owns this tool call.

        Resolution order:

        1. Match by ``conversation_id`` against the active request's
           ``conversation_id``. This is the natural path on the direct
           lane: the bridge service forwards the chat's conversationId
           and the bridge endpoint sees it.

        2. Fall back to ``principal`` binding (set by the OpenClaw
           bridge service via ``bind_principal_to_request``). This is
           the OpenClaw-lane path: the MCP bridge subprocess substitutes
           its own bridge-process conversation id in the envelope, so
           conversationId can't correlate, but principal can — there is
           one chat per sandbox at a time.

        If neither resolves to an active request, drop the entry
        silently. Live streaming is a UX feature, not a correctness
        gate; the audit log keeps the authoritative record.
        """
        now_ns = time.monotonic_ns()
        entry = LiveToolCallEntry(
            tool_id=tool_id,
            status=status,
            started_at_ns=started_at_ns if started_at_ns is not None else now_ns,
            finished_at_ns=now_ns if status != "running" else None,
            args_summary=args_summary,
            result_summary=result_summary,
            error=error,
            draft_mutated=draft_mutated,
        )
        with self._assistant_request_lock:
            # 1. conversationId match
            if conversation_id:
                for record in self._assistant_requests.values():
                    if record.conversation_id != conversation_id:
                        continue
                    if record.status not in ("running", "cancel_requested"):
                        continue
                    record.tool_calls.append(entry)
                    record.updated_at_ns = now_ns
                    return entry
            # 2. principal-binding fallback (OpenClaw lane)
            if principal:
                bound_request_id = self._assistant_principal_bindings.get(principal)
                if bound_request_id is not None:
                    record = self._assistant_requests.get(bound_request_id)
                    if record is not None and record.status in (
                        "running",
                        "cancel_requested",
                    ):
                        record.tool_calls.append(entry)
                        record.updated_at_ns = now_ns
                        return entry
        return None

    def mark_assistant_request_cancel_requested(
        self,
        request_id: str,
        *,
        message: str = "Cancellation requested.",
    ) -> AssistantRequestRecord | None:
        now_ns = time.monotonic_ns()
        with self._assistant_request_lock:
            record = self._assistant_requests.get(request_id)
            if record is None:
                return None
            if record.status == "running":
                record.status = "cancel_requested"
                record.message = message
                record.updated_at_ns = now_ns
            return record

    def get_assistant_request(self, request_id: str) -> AssistantRequestRecord | None:
        with self._assistant_request_lock:
            return self._assistant_requests.get(request_id)

    def stage_assistant_proposals(
        self,
        *,
        conversation_id: str,
        proposals: list[dict[str, Any]],
        source: dict[str, Any] | None = None,
    ) -> list[AssistantProposalRecord]:
        """Store reviewable assistant proposals without changing program state."""
        now_ns = time.monotonic_ns()
        staged: list[AssistantProposalRecord] = []
        source_payload = dict(source or {})
        with self._assistant_proposal_lock:
            for proposal in proposals:
                proposal_id = str(proposal["id"])
                record = AssistantProposalRecord(
                    proposal_id=proposal_id,
                    conversation_id=conversation_id,
                    proposal=dict(proposal),
                    source=source_payload,
                    lifecycle_status="staged",
                    created_at_ns=now_ns,
                    updated_at_ns=now_ns,
                )
                self._assistant_proposals[proposal_id] = record
                staged.append(record)
            self._prune_assistant_proposals_locked()
        return staged

    def list_assistant_proposals(
        self,
        *,
        lifecycle_status: str | None = None,
        conversation_id: str | None = None,
    ) -> list[AssistantProposalRecord]:
        with self._assistant_proposal_lock:
            records = list(self._assistant_proposals.values())
        if lifecycle_status:
            records = [
                record for record in records
                if record.lifecycle_status == lifecycle_status
            ]
        if conversation_id:
            records = [
                record for record in records
                if record.conversation_id == conversation_id
            ]
        return sorted(records, key=lambda item: (item.created_at_ns, item.proposal_id))

    def get_assistant_proposal(self, proposal_id: str) -> AssistantProposalRecord | None:
        with self._assistant_proposal_lock:
            return self._assistant_proposals.get(proposal_id)

    def discard_assistant_proposal(
        self,
        proposal_id: str,
        *,
        reason: str = "",
    ) -> AssistantProposalRecord | None:
        with self._assistant_proposal_lock:
            record = self._assistant_proposals.get(proposal_id)
            if record is None:
                return None
            record.lifecycle_status = "discarded"
            record.updated_at_ns = time.monotonic_ns()
            if reason:
                record.source = {**record.source, "discardReason": reason}
            return record

    def _clear_published_scene_snapshots(self) -> None:
        with self._scene_snapshot_lock:
            self._published_scene_snapshots.clear()

    def _prune_published_scene_snapshots(self, *, now_ns: Optional[int] = None) -> None:
        now = now_ns or time.monotonic_ns()
        ttl_ns = 10 * 60 * 1_000_000_000
        max_entries = 64
        expired_ids = [
            snapshot_id
            for snapshot_id, snapshot in self._published_scene_snapshots.items()
            if now - snapshot.created_at_ns > ttl_ns
        ]
        for snapshot_id in expired_ids:
            self._published_scene_snapshots.pop(snapshot_id, None)
        if len(self._published_scene_snapshots) <= max_entries:
            return
        oldest_first = sorted(
            self._published_scene_snapshots.values(),
            key=lambda item: item.created_at_ns,
        )
        while len(oldest_first) > max_entries:
            stale = oldest_first.pop(0)
            self._published_scene_snapshots.pop(stale.snapshot_id, None)

    def publish_scene_snapshot(
        self,
        image_bytes: bytes,
        *,
        width: int,
        height: int,
        format: str,
        fetch_base_url: Optional[str] = None,
        model_base_url: Optional[str] = None,
    ) -> dict[str, Any]:
        resolved_fetch_base_url = self.resolve_scene_snapshot_fetch_base_url(
            fetch_base_url=fetch_base_url,
            model_base_url=model_base_url,
        )
        media_type = "image/png" if format.lower() == "png" else "image/jpeg"
        snapshot_id = uuid.uuid4().hex
        snapshot = PublishedSceneSnapshot(
            snapshot_id=snapshot_id,
            image_bytes=bytes(image_bytes),
            media_type=media_type,
            width=width,
            height=height,
            format=format,
            sha256=hashlib.sha256(image_bytes).hexdigest(),
        )
        with self._scene_snapshot_lock:
            self._prune_published_scene_snapshots()
            self._published_scene_snapshots[snapshot_id] = snapshot
            self._prune_published_scene_snapshots()
        return {
            "snapshot_id": snapshot_id,
            "url": f"{resolved_fetch_base_url.rstrip('/')}/api/scene/snapshots/{snapshot_id}",
            "sha256": snapshot.sha256,
            "format": format,
            "width": width,
            "height": height,
            "byte_size": len(image_bytes),
        }

    def resolve_scene_snapshot_fetch_base_url(
        self,
        *,
        fetch_base_url: Optional[str] = None,
        model_base_url: Optional[str] = None,
    ) -> str:
        explicit = (fetch_base_url or "").strip()
        if explicit:
            return _normalize_scene_snapshot_base_url(
                explicit,
                field_name="Scene snapshot fetch base URL",
            )

        public_base = (self.public_api_base_url or "").strip()
        if model_base_url and public_base:
            model_parts = urlparse(model_base_url)
            public_parts = urlparse(public_base)
            if (
                model_parts.scheme
                and model_parts.hostname
                and public_parts.scheme
                and public_parts.hostname
            ):
                auth_host = model_parts.hostname
                if model_parts.port is not None:
                    auth_host = f"{auth_host}:{model_parts.port}"
                public_host = public_parts.hostname
                if public_parts.port is not None:
                    public_host = f"{public_host}:{public_parts.port}"
                derived = public_parts._replace(
                    netloc=auth_host,
                    path="",
                    params="",
                    query="",
                    fragment="",
                )
                return _normalize_scene_snapshot_base_url(
                    derived.geturl(),
                    field_name="Derived scene snapshot fetch base URL",
                )

        if public_base:
            return _normalize_scene_snapshot_base_url(
                public_base,
                field_name="Composer public API base URL",
            )

        raise RuntimeError("No scene snapshot fetch base URL is configured")

    def get_published_scene_snapshot(
        self,
        snapshot_id: str,
    ) -> Optional[PublishedSceneSnapshot]:
        with self._scene_snapshot_lock:
            self._prune_published_scene_snapshots()
            return self._published_scene_snapshots.get(snapshot_id)

    def _bind_cycle_manager_callbacks(self) -> None:
        if self.cycle_manager is None:
            return
        self.cycle_manager.set_parent_run_id_provider(
            self._current_or_last_run_id_for_recovery,
        )
        self.cycle_manager.set_on_delegated_recovery_requested(
            self._record_delegated_recovery_requested,
        )
        self.cycle_manager.set_on_reset_phase_event(
            self._record_cycle_reset_phase,
        )

    def _initialize_kernel_observability(self, trace_dir: str) -> None:
        """Wire the kernel flight-ring dump directory, ingest dumps left by a
        previous fail-closed death, and install the monotonic<->wall clock
        anchor. Best-effort by design: observability wiring failure must
        never block Composer startup (spec 500 §1)."""
        flight_dir = os.path.join(str(trace_dir), "flight")
        try:
            os.makedirs(flight_dir, exist_ok=True)
            self._kernel.set_flight_recorder_dir(flight_dir)
        except Exception:  # noqa: BLE001 - telemetry wiring is best-effort.
            logger.warning("kernel flight recorder wiring failed", exc_info=True)
        try:
            ingested = self.event_recorder.ingest_kernel_flight_dumps(flight_dir)
            if ingested:
                logger.warning(
                    "ingested kernel flight dumps from a previous fail-closed "
                    "termination",
                    extra={"count": ingested, "flight_dir": flight_dir},
                )
        except Exception:  # noqa: BLE001 - telemetry wiring is best-effort.
            logger.warning("kernel flight dump ingest failed", exc_info=True)
        self.refresh_clock_anchor(min_interval_s=0.0)

    def refresh_clock_anchor(self, *, min_interval_s: float = 3600.0) -> None:
        """Record a fresh kernel monotonic<->wall anchor, rate-limited.

        Called at startup and periodically from the state hub so long-running
        deployments keep an anchor near every event in the retained window.
        """
        now = time.monotonic()
        last = getattr(self, "_last_clock_anchor_monotonic", 0.0)
        if min_interval_s > 0.0 and now - last < min_interval_s:
            return
        try:
            anchor = self._kernel.clock_anchor()
            self.event_recorder.set_clock_anchor(
                monotonic_ns=anchor["monotonic_ns"],
                wall_time_ns=anchor["wall_time_ns"],
            )
            self._last_clock_anchor_monotonic = now
        except Exception:  # noqa: BLE001 - telemetry wiring is best-effort.
            logger.warning("clock anchor refresh failed", exc_info=True)

    def _current_or_last_run_id_for_recovery(self) -> Optional[str]:
        self.event_recorder.observe(self, force=True)
        return self.event_recorder.current_run_id or self.event_recorder.last_run_id

    def _record_delegated_recovery_requested(self, request) -> None:
        self.event_recorder.record_delegated_recovery_requested(request)

    def _record_cycle_reset_phase(self, event: dict[str, Any]) -> None:
        self.event_recorder.record_cycle_reset_phase(event)

    @property
    def kernel(self) -> Optional[KernelClient]:
        """Access the kernel client."""
        return self._kernel

    @property
    def effective_program(self) -> Optional[Program]:
        """Program snapshot currently bound into the runtime runner."""
        return self._effective_program

    @property
    def program_tree_hash(self) -> str:
        """Current base program tree hash."""
        return self._program_tree_hash

    @property
    def program_session_generation(self) -> int:
        """Monotonic generation for the loaded/applied program snapshot."""
        return self._program_session_generation

    @property
    def runtime_overrides(self) -> dict[str, RuntimeOverrideRecord]:
        """Return a shallow copy of the runtime override ledger keyed by node name."""
        with self._runtime_override_lock:
            return dict(self._runtime_overrides)

    def _update_program_tree_hash(self) -> None:
        """Recompute and cache the program tree hash from the current base program."""
        if self.program is not None and self.program.tree is not None:
            self._program_tree_hash = compute_program_tree_hash(self.program.tree)
        else:
            self._program_tree_hash = ""
        with self._runtime_override_lock:
            self._mark_runtime_overrides_changed_locked()

    def _mark_runtime_overrides_changed_locked(self) -> None:
        self._runtime_override_revision += 1
        self._runtime_override_next_tree_cache_key = None
        self._runtime_override_next_tree_cache = None

    def _override_is_stale(self, record: RuntimeOverrideRecord) -> bool:
        """An override is stale when its base_program_hash doesn't match the current program."""
        return (
            bool(record.base_program_hash)
            and record.base_program_hash != self._program_tree_hash
        )

    def _has_stale_overrides(self) -> bool:
        with self._runtime_override_lock:
            return any(self._override_is_stale(r) for r in self._runtime_overrides.values())

    def _stale_override_names(self) -> list[str]:
        with self._runtime_override_lock:
            return [
                name for name, r in self._runtime_overrides.items()
                if self._override_is_stale(r)
            ]

    def _deployment_capability_keys(self) -> set[str] | None:
        if self.deployment is None:
            return None
        return self.deployment.capability_keys()

    def _build_program_with_overrides(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        with self._runtime_override_lock:
            stale_names = [
                r.node_name for r in self._runtime_overrides.values()
                if self._override_is_stale(r)
                and (include_staged or r.activation_state == "active")
            ]
            overrides_by_name = {
                record.node_name: dict(record.params)
                for record in self._runtime_overrides.values()
                if include_staged or record.activation_state == "active"
            }
        if stale_names:
            raise RuntimeError(
                f"Cannot build effective program: stale overrides must be "
                f"revalidated or discarded first: {', '.join(stale_names)}"
            )

        effective_tree = apply_tree_param_overrides(self.program.tree, overrides_by_name)
        program_data = self.program.to_dict()
        program_data["tree"] = effective_tree.to_dict()
        return Program.from_dict(
            program_data,
            self.catalog,
            path=self.program_source_path or "<memory>",
            skill_catalog=self.skill_catalog,
            deployment_capability_keys=self._deployment_capability_keys(),
        )

    def _set_runtime_override_states(self, activation_state: str) -> None:
        now_ns = time.monotonic_ns()
        with self._runtime_override_lock:
            for record in self._runtime_overrides.values():
                record.activation_state = activation_state
                if activation_state == "active":
                    record.activated_at_ns = now_ns
            self._mark_runtime_overrides_changed_locked()

    def _selected_runtime_override_records(
        self,
        *,
        include_staged: bool,
    ) -> list[RuntimeOverrideRecord]:
        with self._runtime_override_lock:
            records = [
                record
                for record in self._runtime_overrides.values()
                if include_staged or record.activation_state == "active"
            ]
        if not records:
            selected = "active/staged" if include_staged else "active"
            raise RuntimeError(f"No {selected} runtime overrides selected")
        return records

    def _validate_persistent_edit_authority(
        self,
        records: list[RuntimeOverrideRecord],
        *,
        authority_source: str,
        approval_mode: str,
    ) -> None:
        if authority_source not in {"human", "llm"}:
            raise RuntimeError("authority_source must be one of: human, llm")
        if approval_mode not in {"human_approved", "not_required"}:
            raise RuntimeError("approval_mode must be one of: human_approved, not_required")
        if authority_source == "human" and approval_mode != "human_approved":
            raise RuntimeError("human authority_source requires approval_mode=human_approved")
        if authority_source == "llm" and approval_mode == "human_approved":
            raise RuntimeError("approval_mode=human_approved is only valid for human authority_source")

        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        for record in records:
            target_node = find_tree_node_by_name(self.program.tree, record.node_name)
            if target_node is None:
                raise RuntimeError(f"Unknown node instance name: {record.node_name}")
            policy = target_node.modification_policy.get("persistent_edit", "forbidden")
            if policy == "forbidden":
                raise RuntimeError(
                    f"Node '{record.node_name}' does not allow persistent edits"
                )
            if policy == "human_approval" and authority_source != "human":
                raise RuntimeError(
                    f"Node '{record.node_name}' requires human approval for persistent edits"
                )

    def _runtime_override_run_context(
        self,
        session_id: Optional[str],
    ) -> tuple[Optional[str], Optional[str], Optional[str]]:
        run_id = self.event_recorder.current_run_id or self.event_recorder.last_run_id
        authority_source = None
        trigger_node_instance = None
        intervention = (
            self.cycle_manager.intervention
            if self.cycle_manager is not None
            else None
        )
        if intervention is not None and intervention.session_id == session_id:
            authority_source = intervention.authority_source
            trigger_node_instance = intervention.trigger_node_instance
            run_id = intervention.parent_run_id or f"intervention-{intervention.session_id}"
        return run_id, authority_source, trigger_node_instance

    def _clear_runtime_overrides(self) -> None:
        with self._runtime_override_lock:
            self._runtime_overrides.clear()
            self._mark_runtime_overrides_changed_locked()

    def _override_entry_dict(self, record: RuntimeOverrideRecord) -> dict[str, Any]:
        """Serialize a single override record for REST/MCP consumption."""
        return {
            "node_name": record.node_name,
            "params": dict(record.params),
            "authority_source": record.authority_source,
            "approval_mode": record.approval_mode,
            "reason": record.reason,
            "session_id": record.session_id,
            "trigger_node_instance": record.trigger_node_instance,
            "base_program_hash": record.base_program_hash,
            "requested_at_ns": record.requested_at_ns,
            "activation_state": record.activation_state,
            "activated_at_ns": record.activated_at_ns,
            "is_stale": self._override_is_stale(record),
        }

    def runtime_override_status(self) -> dict[str, Any]:
        """Return ledger state optimized for REST/MCP/UI consumption."""
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        with self._runtime_override_lock:
            active = [
                record for record in self._runtime_overrides.values()
                if record.activation_state == "active"
            ]
            staged = [
                record for record in self._runtime_overrides.values()
                if record.activation_state == "staged"
            ]
            stale = [
                record for record in self._runtime_overrides.values()
                if self._override_is_stale(record)
            ]
            requires_reset_to_activate = bool(staged) and cycle_state == "stopped"
            can_activate_now = (
                bool(staged) and cycle_state == "idle" and not stale
            )
            next_effective_tree = None
            if self.program is not None and staged and not stale:
                cache_key = (
                    self._runtime_override_revision,
                    self._program_tree_hash,
                    self._program_session_generation,
                )
                if (
                    self._runtime_override_next_tree_cache_key == cache_key
                    and self._runtime_override_next_tree_cache is not None
                ):
                    next_effective_tree = copy.deepcopy(
                        self._runtime_override_next_tree_cache
                    )
                else:
                    try:
                        next_effective_tree = self._build_program_with_overrides(
                            include_staged=True
                        ).tree.to_dict()
                    except Exception:
                        next_effective_tree = None
                    if next_effective_tree is not None:
                        self._runtime_override_next_tree_cache_key = cache_key
                        self._runtime_override_next_tree_cache = copy.deepcopy(
                            next_effective_tree
                        )
        return {
            "active": [self._override_entry_dict(r) for r in active],
            "staged": [self._override_entry_dict(r) for r in staged],
            "stale": [self._override_entry_dict(r) for r in stale],
            "active_count": len(active),
            "staged_count": len(staged),
            "stale_count": len(stale),
            "has_active": bool(active),
            "has_staged": bool(staged),
            "has_stale": bool(stale),
            "can_activate_now": can_activate_now,
            "requires_reset_to_activate": requires_reset_to_activate,
            "current_tree_origin": "runtime_override" if active else "saved_program",
            "base_program_hash": self._program_tree_hash,
            "next_effective_tree": next_effective_tree,
            "timestamp_ns": time.monotonic_ns(),
        }

    def materialized_runtime_override_program(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        """Return the program snapshot produced by materializing runtime overrides."""
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")
        with self._runtime_override_lock:
            has_runtime_overrides = bool(self._runtime_overrides)
        if not has_runtime_overrides:
            return self.program
        return self._build_program_with_overrides(include_staged=include_staged)

    def _current_cycle_state(self) -> str:
        return self.cycle_manager.state.value if self.cycle_manager is not None else "idle"

    def promote_runtime_overrides_to_loaded_program(
        self,
        *,
        include_staged: bool,
    ) -> Program:
        """Materialize selected runtime overrides into the loaded in-memory program."""
        cycle_state = self._current_cycle_state()
        if cycle_state not in {"idle", "stopped"}:
            raise RuntimeError(
                "Runtime overrides may only be promoted into the loaded program in IDLE or STOPPED"
            )
        with self._runtime_override_lock:
            has_staged_overrides = any(
                record.activation_state == "staged"
                for record in self._runtime_overrides.values()
            )
        if not include_staged and has_staged_overrides:
            raise RuntimeError(
                "Staged runtime overrides exist; either include staged overrides in the promotion or clear them first"
            )
        selected_records = self._selected_runtime_override_records(
            include_staged=include_staged,
        )
        materialized_program = self.materialized_runtime_override_program(
            include_staged=include_staged,
        )
        self.apply_models(
            materialized_program,
            self.deployment,
            force_discard_overrides=True,
        )
        for record in selected_records:
            self.event_recorder.record_runtime_override_event(
                action="promoted_loaded_program",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=self.event_recorder.last_run_id,
                session_id=record.session_id,
                authority_source=record.authority_source,
                trigger_node_instance=record.trigger_node_instance,
            )
        return materialized_program

    def save_materialized_runtime_override_program(
        self,
        target_path: str,
        *,
        include_staged: bool,
        authority_source: str,
        approval_mode: str,
    ) -> Program:
        """Save a materialized program view produced from runtime overrides."""
        cycle_state = self._current_cycle_state()
        if cycle_state == "running":
            raise RuntimeError(
                "Materialized runtime override saves are not allowed while execution is active"
            )
        selected_records = self._selected_runtime_override_records(
            include_staged=include_staged,
        )
        self._validate_persistent_edit_authority(
            selected_records,
            authority_source=authority_source,
            approval_mode=approval_mode,
        )
        materialized_program = self.materialized_runtime_override_program(
            include_staged=include_staged,
        )
        materialized_program.save(target_path)
        for record in selected_records:
            self.event_recorder.record_runtime_override_event(
                action="saved_materialized_program",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=self.event_recorder.last_run_id,
                session_id=record.session_id,
                authority_source=authority_source,
                trigger_node_instance=record.trigger_node_instance,
                details={"path": target_path},
            )
        return materialized_program

    def activate_staged_runtime_overrides(self) -> None:
        """Promote staged overrides into the runtime by rebuilding the effective program."""
        with self._runtime_override_lock:
            has_staged_overrides = any(
                record.activation_state == "staged"
                for record in self._runtime_overrides.values()
            )
        if not has_staged_overrides:
            return
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state != "idle":
            raise RuntimeError("Staged runtime overrides can only be activated from IDLE")
        effective_program = self._build_program_with_overrides(include_staged=True)
        with self._runtime_override_lock:
            previous_states = {
                node_name: (record.activation_state, record.activated_at_ns)
                for node_name, record in self._runtime_overrides.items()
            }
        self._set_runtime_override_states("active")
        try:
            self._rebuild_runtime(
                self.program,
                self.deployment,
                effective_program=effective_program,
            )
        except Exception:
            with self._runtime_override_lock:
                for node_name, (activation_state, activated_at_ns) in previous_states.items():
                    record = self._runtime_overrides.get(node_name)
                    if record is None:
                        continue
                    record.activation_state = activation_state
                    record.activated_at_ns = activated_at_ns
                self._mark_runtime_overrides_changed_locked()
            raise
        with self._runtime_override_lock:
            activated_records = list(self._runtime_overrides.values())
        for record in activated_records:
            run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
                record.session_id
            )
            self.event_recorder.record_runtime_override_event(
                action="activated",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

    def remove_runtime_override(self, node_name: str) -> bool:
        """Remove a runtime override and rebuild immediately if it was active."""
        cycle_state = self._ensure_runtime_override_mutation_allowed()
        with self._runtime_override_lock:
            record = self._runtime_overrides.pop(node_name, None)
            if record is not None:
                self._mark_runtime_overrides_changed_locked()
        if record is None:
            return False
        if cycle_state == "direct_control" and record.activation_state == "active":
            with self._runtime_override_lock:
                self._runtime_overrides[node_name] = record
                self._mark_runtime_overrides_changed_locked()
            raise RuntimeError(
                "Cannot remove an active runtime override during DIRECT_CONTROL; return to IDLE first"
            )
        if record.activation_state == "active":
            with self._runtime_override_lock:
                has_remaining_overrides = bool(self._runtime_overrides)
            effective_program = (
                self._build_program_with_overrides(include_staged=False)
                if has_remaining_overrides
                else self.program
            )
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=effective_program,
                )
            except Exception:
                with self._runtime_override_lock:
                    self._runtime_overrides[node_name] = record
                    self._mark_runtime_overrides_changed_locked()
                raise
        run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
            record.session_id
        )
        self.event_recorder.record_runtime_override_event(
            action="removed",
            node_name=record.node_name,
            params=record.params,
            activation_state=record.activation_state,
            base_program_hash=record.base_program_hash,
            run_id=run_id,
            session_id=record.session_id,
            authority_source=authority_source or record.authority_source,
            trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
        )
        return True

    def clear_runtime_overrides(self) -> None:
        """Drop all runtime overrides and return the runtime to the base program."""
        cycle_state = self._ensure_runtime_override_mutation_allowed()
        with self._runtime_override_lock:
            removed = list(self._runtime_overrides.values())
        if cycle_state == "direct_control" and any(
            record.activation_state == "active" for record in removed
        ):
            raise RuntimeError(
                "Cannot clear active runtime overrides during DIRECT_CONTROL; return to IDLE first"
            )
        had_active = any(record.activation_state == "active" for record in removed)
        self._clear_runtime_overrides()
        if had_active and self.program is not None:
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=self.program,
                )
            except Exception:
                with self._runtime_override_lock:
                    self._runtime_overrides = {
                        record.node_name: record
                        for record in removed
                    }
                    self._mark_runtime_overrides_changed_locked()
                raise
        for record in removed:
            run_id, authority_source, trigger_node_instance = self._runtime_override_run_context(
                record.session_id
            )
            self.event_recorder.record_runtime_override_event(
                action="cleared",
                node_name=record.node_name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=record.base_program_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

    def revalidate_stale_overrides(self) -> dict[str, str]:
        """Re-validate stale overrides against the current program.

        Returns a mapping of node_name -> outcome where outcome is one of:
        ``"revalidated"`` (hash updated) or ``"invalid"`` (removed from ledger).
        """
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")

        with self._runtime_override_lock:
            stale = [
                (name, record)
                for name, record in self._runtime_overrides.items()
                if self._override_is_stale(record)
            ]
        if not stale:
            return {}

        # Determine outcome for each stale override and snapshot record data
        # for event emission before any mutations.
        outcomes: list[tuple[str, str, RuntimeOverrideRecord]] = []
        for name, record in stale:
            target_node = find_tree_node_by_name(self.program.tree, name)
            if target_node is None:
                outcomes.append((name, "invalid", record))
                continue

            policy = target_node.modification_policy.get("runtime_override", "forbidden")
            if policy == "forbidden":
                outcomes.append((name, "invalid", record))
                continue

            try:
                test_overrides = {name: dict(record.params)}
                apply_tree_param_overrides(self.program.tree, test_overrides)
            except Exception:
                outcomes.append((name, "invalid", record))
                continue

            outcomes.append((name, "revalidated", record))

        # Apply mutations and emit events.
        results: dict[str, str] = {}
        for name, outcome, record in outcomes:
            old_hash = record.base_program_hash
            with self._runtime_override_lock:
                if outcome == "revalidated":
                    record.base_program_hash = self._program_tree_hash
                else:
                    self._runtime_overrides.pop(name, None)
                self._mark_runtime_overrides_changed_locked()
            results[name] = outcome

            run_id, authority_source, trigger_node_instance = (
                self._runtime_override_run_context(record.session_id)
            )
            action = "revalidated" if outcome == "revalidated" else "invalidated"
            self.event_recorder.record_runtime_override_event(
                action=action,
                node_name=name,
                params=record.params,
                activation_state=record.activation_state,
                base_program_hash=old_hash,
                run_id=run_id,
                session_id=record.session_id,
                authority_source=authority_source or record.authority_source,
                trigger_node_instance=trigger_node_instance or record.trigger_node_instance,
            )

        return results

    def apply_runtime_override(
        self,
        *,
        node_name: str,
        params: dict[str, Any],
        authority_source: str,
        approval_mode: str,
        reason: str = "",
        session_id: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
    ) -> RuntimeOverrideRecord:
        """Validate, record, and apply or stage a runtime override."""
        if self.program is None or self.program.tree is None:
            raise RuntimeError("No program loaded")
        if not isinstance(params, dict) or not params:
            raise RuntimeError("Runtime override params must be a non-empty mapping")

        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state not in {"idle", "direct_control"}:
            raise RuntimeError("Runtime overrides may only be changed in IDLE or DIRECT_CONTROL")
        with self._runtime_override_lock:
            has_active_override = any(
                record.activation_state == "active"
                for record in self._runtime_overrides.values()
            )
        if cycle_state == "direct_control" and has_active_override:
            raise RuntimeError(
                "Cannot stage additional runtime overrides during DIRECT_CONTROL while active overrides are already bound into the runner; return to IDLE first"
            )
        if authority_source not in {"human", "llm"}:
            raise RuntimeError("authority_source must be one of: human, llm")
        if approval_mode not in {"human_approved", "not_required"}:
            raise RuntimeError("approval_mode must be one of: human_approved, not_required")
        if authority_source == "human" and approval_mode != "human_approved":
            raise RuntimeError("human authority_source requires approval_mode=human_approved")
        if authority_source == "llm" and approval_mode == "human_approved":
            raise RuntimeError("approval_mode=human_approved is only valid for human authority_source")

        target_node = find_tree_node_by_name(self.program.tree, node_name)
        if target_node is None:
            raise RuntimeError(f"Unknown node instance name: {node_name}")

        policy = target_node.modification_policy.get("runtime_override", "forbidden")
        if policy == "forbidden":
            raise RuntimeError(f"Node '{node_name}' does not allow runtime overrides")
        if policy == "human_approval" and authority_source != "human":
            raise RuntimeError(f"Node '{node_name}' requires human approval for runtime override")

        with self._runtime_override_lock:
            existing = self._runtime_overrides.get(node_name)
        merged_params = merge_param_patch(
            existing.params if existing is not None else {},
            params,
        )

        candidate = RuntimeOverrideRecord(
            node_name=node_name,
            params=merged_params,
            authority_source=authority_source,
            approval_mode=approval_mode,
            reason=reason,
            session_id=session_id,
            trigger_node_instance=trigger_node_instance,
            base_program_hash=self._program_tree_hash,
            activation_state="active" if cycle_state == "idle" else "staged",
            activated_at_ns=time.monotonic_ns() if cycle_state == "idle" else None,
        )
        with self._runtime_override_lock:
            self._runtime_overrides[node_name] = candidate
            self._mark_runtime_overrides_changed_locked()
        try:
            effective_program = self._build_program_with_overrides(include_staged=True)
        except Exception:
            with self._runtime_override_lock:
                if existing is None:
                    self._runtime_overrides.pop(node_name, None)
                else:
                    self._runtime_overrides[node_name] = existing
                self._mark_runtime_overrides_changed_locked()
            raise

        if cycle_state == "idle":
            try:
                self._rebuild_runtime(
                    self.program,
                    self.deployment,
                    effective_program=effective_program,
                )
            except Exception:
                with self._runtime_override_lock:
                    if existing is None:
                        self._runtime_overrides.pop(node_name, None)
                    else:
                        self._runtime_overrides[node_name] = existing
                    self._mark_runtime_overrides_changed_locked()
                raise
        run_id, event_authority_source, event_trigger_node_instance = self._runtime_override_run_context(
            candidate.session_id
        )
        self.event_recorder.record_runtime_override_event(
            action="applied",
            node_name=candidate.node_name,
            params=candidate.params,
            activation_state=candidate.activation_state,
            base_program_hash=candidate.base_program_hash,
            run_id=run_id,
            session_id=candidate.session_id,
            authority_source=event_authority_source or candidate.authority_source,
            trigger_node_instance=event_trigger_node_instance or candidate.trigger_node_instance,
            details={"reason": candidate.reason},
        )
        return candidate

    def _ensure_runtime_override_mutation_allowed(self) -> str:
        cycle_state = self.cycle_manager.state.value if self.cycle_manager is not None else "idle"
        if cycle_state not in {"idle", "direct_control"}:
            raise RuntimeError("Runtime overrides may only be changed in IDLE or DIRECT_CONTROL")
        return cycle_state

    def refresh_drivable_joint_names(self, *, kernel: Any | None = None) -> list[str]:
        """Refresh and return the ordered drivable joint-name cache.

        This may cross into the native backend. Call it during runtime setup or
        explicit validation, not from observer/publisher hot paths.
        """

        source = kernel if kernel is not None else self.kernel
        names: list[str] = []
        if source is not None:
            try:
                names = [str(name) for name in source.get_joint_names()]
            except Exception:
                logger.debug(
                    "drivable joint-name refresh failed",
                    exc_info=True,
                    extra={"event": "manyforge.visual.drivable_joint_names_refresh_failed"},
                )
        with self._joint_state_lock:
            self._drivable_joint_names = list(names)
            return list(self._drivable_joint_names)

    def get_drivable_joint_names(
        self,
        *,
        allow_backend_refresh: bool = False,
    ) -> list[str]:
        """Return cached ordered drivable joint names.

        Observer paths should leave `allow_backend_refresh` false so rendering
        cannot block behind planner scene locks. Non-hot validation/setup code
        may opt into a backend refresh when the cache is empty or stale.
        """

        with self._joint_state_lock:
            names = list(self._drivable_joint_names)
        if names or not allow_backend_refresh:
            return names
        return self.refresh_drivable_joint_names()

    def record_current_joint_positions(self, positions: list[float]) -> None:
        with self._joint_state_lock:
            self._last_known_joint_positions_rad = list(positions)

    def get_current_joint_positions(
        self,
        *,
        joint_names: list[str] | None = None,
        allow_backend_joint_names: bool = True,
    ) -> Optional[list[float]]:
        """Return the best-known full joint vector, or None if unknown.

        Day-5c.1 lifted this resolution chain into routes_scene for the
        /api/scene/state payload; routes_direct's joint-jog handler
        needed the same answer when assembling the full overlay before
        submitting an intervention. The chain:

        1. Live execution snapshot if it carries non-empty positions.
        2. The state-cached `_last_known_joint_positions_rad`, refreshed
           on every read with valid live positions or on the
           optimistic write that follows a successful jog.
        3. Deployment's `adapters.execution.config.initial_joint_positions_rad`
           — the same vector the kernel uses as its initial config on
           deployment load.

        Without this, joint-jog called outside an active execution
        would pad the snapshot's `None` to zeros and the planner would
        receive a goal where every untouched joint should be 0 —
        the operator's "slider drives the robot to all-zero" report.
        """
        if self.kernel is None:
            return None
        joint_names = list(
            joint_names
            if joint_names is not None
            else self.get_drivable_joint_names(
                allow_backend_refresh=allow_backend_joint_names
            )
        )
        if not joint_names:
            return None
        snapshot = (
            self.cycle_manager.get_execution_snapshot()
            if self.cycle_manager is not None
            else self.kernel.get_execution_snapshot()
        )
        snapshot_positions = getattr(snapshot, "joint_positions_rad", None)
        if snapshot_positions and len(snapshot_positions) == len(joint_names):
            live = list(snapshot_positions)
            self.record_current_joint_positions(live)
            return live
        with self._joint_state_lock:
            last_known = (
                list(self._last_known_joint_positions_rad)
                if self._last_known_joint_positions_rad is not None
                else None
            )
        if last_known is not None and len(last_known) == len(joint_names):
            return last_known
        if self.deployment is not None:
            try:
                execution_adapter = self.deployment.adapters.get("execution")
            except Exception:
                execution_adapter = None
            if execution_adapter is not None:
                initial = execution_adapter.config.get("initial_joint_positions_rad")
                if isinstance(initial, list) and len(initial) == len(joint_names):
                    seeded = [float(v) for v in initial]
                    self.record_current_joint_positions(seeded)
                    return seeded
        return None

    def update_visual_scene_objects_snapshot(
        self,
        objects: list[SceneObjectSnapshot],
        *,
        error: str | None = None,
    ) -> None:
        """Replace the observer-side object snapshot with immutable copies."""
        visual_snapshot, _meta = self.visual_state_snapshot()
        link_poses = (
            copy.deepcopy(getattr(visual_snapshot, "link_poses", {}) or {})
            if visual_snapshot is not None
            else {}
        )
        with self._visual_scene_objects_lock:
            copied = copy.deepcopy(list(objects))
            incoming_ids = {obj.object_id for obj in copied}
            self._visual_scene_object_attachment_offsets = {
                object_id: offset
                for object_id, offset in self._visual_scene_object_attachment_offsets.items()
                if object_id in incoming_ids
            }
            for obj in copied:
                if not obj.attached:
                    self._visual_scene_object_attachment_offsets.pop(obj.object_id, None)
                    continue
                if obj.frame_id and obj.pose_in_universe:
                    frame_pose = link_poses.get(obj.frame_id)
                    if frame_pose is not None:
                        try:
                            self._visual_scene_object_attachment_offsets[obj.object_id] = (
                                self._visual_scene_relative_pose(
                                    parent_pose=frame_pose,
                                    child_pose=obj.pose_in_universe,
                                )
                            )
                        except Exception:
                            logger.debug(
                                "failed to derive visual attachment offset from backend snapshot",
                                exc_info=True,
                                extra={
                                    "event": "manyforge.visual.attachment_offset_derive_failed",
                                    "object_id": obj.object_id,
                                    "frame_id": obj.frame_id,
                                },
                            )
            self._visual_scene_objects = copied
            self._visual_scene_objects_timestamp_ns = time.monotonic_ns()
            self._visual_scene_objects_refresh_error = error

    def clear_visual_scene_objects_snapshot(self) -> None:
        with self._visual_scene_objects_lock:
            self._visual_scene_objects.clear()
            self._visual_scene_object_attachment_offsets.clear()
            self._visual_scene_objects_timestamp_ns = 0
            self._visual_scene_objects_refresh_attempt_ns = 0
            self._visual_scene_objects_refresh_error = None

    def visual_scene_objects_snapshot(
        self,
    ) -> tuple[list[SceneObjectSnapshot], dict[str, Any]]:
        """Return cached scene objects and freshness metadata.

        This cache is the object half of the visual-snapshot migration. Routes
        can read it without touching the mutable planner scene. A background
        refresh may still block on the backend, but observer HTTP requests do
        not.
        """
        with self._visual_scene_objects_lock:
            # Shallow list copy: the cache list is replaced wholesale on
            # refresh (never mutated in place) and elements are read-only
            # by contract — the 15 Hz publish path deep-copied every
            # object on every tick.
            objects = list(self._visual_scene_objects)
            timestamp_ns = self._visual_scene_objects_timestamp_ns
            attempt_ns = self._visual_scene_objects_refresh_attempt_ns
            running = self._visual_scene_objects_refresh_running
            error = self._visual_scene_objects_refresh_error
        age_ms = None
        if timestamp_ns > 0:
            age_ms = (time.monotonic_ns() - timestamp_ns) / 1_000_000.0
        refresh_attempt_age_ms = None
        if attempt_ns > 0:
            refresh_attempt_age_ms = (time.monotonic_ns() - attempt_ns) / 1_000_000.0
        return objects, {
            "timestamp_ns": timestamp_ns,
            "age_ms": age_ms,
            "refresh_attempt_ns": attempt_ns,
            "refresh_attempt_age_ms": refresh_attempt_age_ms,
            "refresh_running": running,
            "refresh_error": error,
        }

    def visual_scene_objects_for_link_poses(
        self,
        link_poses: dict[str, dict[str, Any]] | None,
    ) -> tuple[list[SceneObjectSnapshot], dict[str, Any]]:
        """Return cached visual objects, composing attached poses from link poses.

        Attached-object canonical visuals should follow the visual robot model
        without polling the mutable planner scene. Attachment offsets are
        recorded when Composer-owned scene-edit paths attach or update an
        attached object.
        """

        objects, meta = self.visual_scene_objects_snapshot()
        if not link_poses:
            return objects, meta
        with self._visual_scene_objects_lock:
            # Shallow copy: offset pose dicts are read-only downstream
            # (matrix helpers copy their inputs).
            offsets = dict(self._visual_scene_object_attachment_offsets)
            for obj in objects:
                if (
                    not obj.attached
                    or not obj.frame_id
                    or obj.pose_in_universe is None
                    or obj.object_id in offsets
                ):
                    continue
                frame_pose = link_poses.get(obj.frame_id)
                if frame_pose is None:
                    continue
                try:
                    offset = self._visual_scene_relative_pose(
                        parent_pose=frame_pose,
                        child_pose=obj.pose_in_universe,
                    )
                except Exception:
                    logger.debug(
                        "failed to lazily derive visual attachment offset",
                        exc_info=True,
                        extra={
                            "event": "manyforge.visual.attachment_offset_lazy_derive_failed",
                            "object_id": obj.object_id,
                            "frame_id": obj.frame_id,
                        },
                    )
                    continue
                self._visual_scene_object_attachment_offsets[obj.object_id] = offset
                offsets[obj.object_id] = offset
        composed: list[SceneObjectSnapshot] = []
        for obj in objects:
            # Copy-on-write: only an attached object whose pose is being
            # recomposed needs its own instance, and a SHALLOW copy at
            # that — the recomposed pose is assigned as a brand-new dict,
            # every other field stays shared read-only. Unattached
            # objects pass through untouched. The previous version
            # deep-copied every object on every 15 Hz publish tick.
            if obj.attached and obj.frame_id:
                offset = offsets.get(obj.object_id)
                frame_pose = link_poses.get(obj.frame_id)
                if offset is not None and frame_pose is not None:
                    try:
                        frame_matrix = _pose_to_matrix(
                            list(frame_pose["position_m"]),
                            list(frame_pose["orientation_xyzw"]),
                        )
                        local_matrix = _pose_to_matrix(
                            list(offset["position_m"]),
                            list(offset["orientation_xyzw"]),
                        )
                        position, orientation = _matrix_to_pose(
                            _multiply_matrices(frame_matrix, local_matrix)
                        )
                        next_obj = copy.copy(obj)
                        next_obj.pose_in_universe = {
                            "position_m": position,
                            "orientation_xyzw": orientation,
                        }
                        composed.append(next_obj)
                        continue
                    except Exception:
                        logger.debug(
                            "failed to compose visual attached-object pose",
                            exc_info=True,
                            extra={
                                "event": "manyforge.visual.attached_object_compose_failed",
                                "object_id": obj.object_id,
                                "frame_id": obj.frame_id,
                            },
                        )
            composed.append(obj)
        return composed, meta

    def _visual_scene_world_frame(self) -> str:
        deployment = self.deployment
        context_adapter = None
        if deployment is not None:
            context_adapter = deployment.adapters.get("context")
        context_config = getattr(context_adapter, "config", {}) or {}
        for key in ("scene_world_frame", "world_frame", "sceneRootFrame", "scene_root_frame"):
            candidate = context_config.get(key)
            if isinstance(candidate, str) and candidate.strip():
                return candidate.strip()
        return "universe"

    def _visual_scene_is_world_frame(self, frame_id: str | None) -> bool:
        frame = str(frame_id or "").strip()
        return frame in {"", "universe", self._visual_scene_world_frame()}

    def _visual_scene_shape_from_edit_request(
        self,
        request: SceneEditRequest,
    ) -> SceneShape | None:
        if not request.shape_type:
            return None
        return SceneShape(
            type=request.shape_type,
            box_dimensions_m=(
                list(request.box_dimensions_m)
                if request.box_dimensions_m is not None
                else None
            ),
            sphere_radius_m=request.sphere_radius_m,
            cylinder_radius_m=request.cylinder_radius_m,
            cylinder_height_m=request.cylinder_height_m,
            mesh_resource_uri=request.mesh_resource_uri,
            mesh_scale=(
                list(request.mesh_scale)
                if request.mesh_scale is not None
                else None
            ),
        )

    def _visual_scene_pose_dict(
        self,
        position_m: list[float] | None,
        orientation_xyzw: list[float] | None,
    ) -> dict[str, Any]:
        return {
            "position_m": [float(v) for v in (position_m or [0.0, 0.0, 0.0])],
            "orientation_xyzw": [
                float(v) for v in (orientation_xyzw or [0.0, 0.0, 0.0, 1.0])
            ],
        }

    def _visual_scene_compose_world_pose(
        self,
        *,
        frame_id: str | None,
        local_pose: dict[str, Any],
        link_poses: dict[str, dict[str, Any]],
    ) -> dict[str, Any] | None:
        if self._visual_scene_is_world_frame(frame_id):
            return copy.deepcopy(local_pose)
        frame_pose = link_poses.get(str(frame_id or ""))
        if frame_pose is None:
            return None
        frame_matrix = _pose_to_matrix(
            list(frame_pose["position_m"]),
            list(frame_pose["orientation_xyzw"]),
        )
        local_matrix = _pose_to_matrix(
            list(local_pose["position_m"]),
            list(local_pose["orientation_xyzw"]),
        )
        position, orientation = _matrix_to_pose(
            _multiply_matrices(frame_matrix, local_matrix)
        )
        return {"position_m": position, "orientation_xyzw": orientation}

    def _visual_scene_relative_pose(
        self,
        *,
        parent_pose: dict[str, Any],
        child_pose: dict[str, Any],
    ) -> dict[str, Any]:
        parent_matrix = _pose_to_matrix(
            list(parent_pose["position_m"]),
            list(parent_pose["orientation_xyzw"]),
        )
        child_matrix = _pose_to_matrix(
            list(child_pose["position_m"]),
            list(child_pose["orientation_xyzw"]),
        )
        position, orientation = _matrix_to_pose(
            _multiply_matrices(_invert_matrix(parent_matrix), child_matrix)
        )
        return {"position_m": position, "orientation_xyzw": orientation}

    def apply_visual_scene_edit_success(
        self,
        request: SceneEditRequest,
        *,
        publish: bool = True,
    ) -> dict[str, Any]:
        """Reflect a successful Composer-owned scene edit into visual state.

        Returns whether the observer cache was updated directly and whether a
        backend refresh is still needed. This enforces the visual snapshot
        single-writer discipline for HTTP scene edits and pose-source applies:
        route/read paths should not rediscover successful mutations later by
        polling the mutable planning scene.
        """

        visual_snapshot, _meta = self.visual_state_snapshot()
        link_poses = (
            copy.deepcopy(getattr(visual_snapshot, "link_poses", {}) or {})
            if visual_snapshot is not None
            else {}
        )
        now_ns = time.monotonic_ns()
        updated = False
        needs_refresh = False
        with self._visual_scene_objects_lock:
            objects = copy.deepcopy(self._visual_scene_objects)
            index_by_id = {obj.object_id: index for index, obj in enumerate(objects)}
            object_index = index_by_id.get(request.object_id)

            try:
                if request.operation == "remove":
                    if object_index is not None:
                        objects.pop(object_index)
                    self._visual_scene_object_attachment_offsets.pop(request.object_id, None)
                    updated = True
                elif request.operation in {"add", "upsert"}:
                    shape = self._visual_scene_shape_from_edit_request(request)
                    if shape is None:
                        needs_refresh = True
                    else:
                        frame_id = request.frame_id or self._visual_scene_world_frame()
                        local_pose = self._visual_scene_pose_dict(
                            request.pose_position_m,
                            request.pose_orientation_xyzw,
                        )
                        world_pose = self._visual_scene_compose_world_pose(
                            frame_id=frame_id,
                            local_pose=local_pose,
                            link_poses=link_poses,
                        )
                        if world_pose is None:
                            needs_refresh = True
                        else:
                            attached = not self._visual_scene_is_world_frame(frame_id)
                            snapshot = SceneObjectSnapshot(
                                object_id=request.object_id,
                                shape=shape,
                                pose_in_universe=world_pose,
                                attached=attached,
                                frame_id=frame_id,
                            )
                            if object_index is None:
                                objects.append(snapshot)
                            else:
                                objects[object_index] = snapshot
                            if attached:
                                self._visual_scene_object_attachment_offsets[
                                    request.object_id
                                ] = local_pose
                            else:
                                self._visual_scene_object_attachment_offsets.pop(
                                    request.object_id, None
                                )
                            updated = True
                elif request.operation == "update_pose":
                    if object_index is None:
                        needs_refresh = True
                    else:
                        existing = objects[object_index]
                        frame_id = request.frame_id or existing.frame_id or self._visual_scene_world_frame()
                        local_pose = self._visual_scene_pose_dict(
                            request.pose_position_m,
                            request.pose_orientation_xyzw,
                        )
                        world_pose = self._visual_scene_compose_world_pose(
                            frame_id=frame_id,
                            local_pose=local_pose,
                            link_poses=link_poses,
                        )
                        if world_pose is None:
                            needs_refresh = True
                        else:
                            existing.pose_in_universe = world_pose
                            existing.frame_id = frame_id
                            existing.attached = not self._visual_scene_is_world_frame(frame_id)
                            objects[object_index] = existing
                            if existing.attached:
                                self._visual_scene_object_attachment_offsets[
                                    request.object_id
                                ] = local_pose
                            else:
                                self._visual_scene_object_attachment_offsets.pop(
                                    request.object_id, None
                                )
                            updated = True
                elif request.operation == "attach":
                    if object_index is None or not request.frame_id:
                        needs_refresh = True
                    else:
                        existing = objects[object_index]
                        frame_pose = link_poses.get(request.frame_id)
                        if frame_pose is None or existing.pose_in_universe is None:
                            needs_refresh = True
                        else:
                            local_pose = self._visual_scene_relative_pose(
                                parent_pose=frame_pose,
                                child_pose=existing.pose_in_universe,
                            )
                            existing.attached = True
                            existing.frame_id = request.frame_id
                            objects[object_index] = existing
                            self._visual_scene_object_attachment_offsets[
                                request.object_id
                            ] = local_pose
                            updated = True
                elif request.operation == "detach":
                    if object_index is None:
                        needs_refresh = True
                    else:
                        existing = objects[object_index]
                        offset = self._visual_scene_object_attachment_offsets.get(
                            request.object_id
                        )
                        if existing.attached and existing.frame_id and offset is not None:
                            world_pose = self._visual_scene_compose_world_pose(
                                frame_id=existing.frame_id,
                                local_pose=offset,
                                link_poses=link_poses,
                            )
                            if world_pose is not None:
                                existing.pose_in_universe = world_pose
                        existing.attached = False
                        existing.frame_id = request.frame_id or self._visual_scene_world_frame()
                        self._visual_scene_object_attachment_offsets.pop(
                            request.object_id, None
                        )
                        objects[object_index] = existing
                        updated = True
                else:
                    needs_refresh = True
            except Exception as exc:
                logger.debug(
                    "visual scene edit cache update failed",
                    exc_info=True,
                    extra={
                        "event": "manyforge.visual.scene_edit_cache_update_failed",
                        "operation": request.operation,
                        "object_id": request.object_id,
                        "error": str(exc),
                    },
                )
                needs_refresh = True
                updated = False

            if updated:
                self._visual_scene_objects = objects
                self._visual_scene_objects_timestamp_ns = now_ns
                self._visual_scene_objects_refresh_error = None

        if updated and publish:
            self.request_visual_state_publish(reason=f"scene_edit_{request.operation}")
        return {"updated": updated, "needs_refresh": needs_refresh}

    def refresh_visual_scene_objects_sync(self, *, reason: str = "") -> dict[str, Any]:
        """Refresh cached visual objects from the backend on a non-observer path."""
        if self.kernel is None:
            self.update_visual_scene_objects_snapshot([], error="kernel unavailable")
            return {"success": False, "error": "kernel unavailable", "reason": reason}
        started = time.monotonic()
        with self._visual_scene_objects_lock:
            self._visual_scene_objects_refresh_attempt_ns = time.monotonic_ns()
        try:
            objects = list(self.kernel.get_scene_objects())
        except Exception as exc:
            with self._visual_scene_objects_lock:
                self._visual_scene_objects_refresh_error = str(exc)
            return {"success": False, "error": str(exc), "reason": reason}
        self.update_visual_scene_objects_snapshot(objects)
        return {
            "success": True,
            "count": len(objects),
            "reason": reason,
            "duration_ms": round((time.monotonic() - started) * 1000.0, 1),
        }

    def request_visual_scene_objects_refresh(self, *, reason: str = "") -> dict[str, Any]:
        """Start a latest-wins background refresh for observer-side objects."""
        with self._visual_scene_objects_lock:
            if self._visual_scene_objects_refresh_running:
                return {
                    "success": True,
                    "queued": False,
                    "running": True,
                    "reason": reason,
                }
            self._visual_scene_objects_refresh_running = True
            self._visual_scene_objects_refresh_attempt_ns = time.monotonic_ns()

        def worker() -> None:
            try:
                result = self.refresh_visual_scene_objects_sync(reason=reason)
                if not result.get("success", False):
                    logger.debug(
                        "visual scene object refresh failed: %s",
                        result.get("error"),
                    )
            finally:
                with self._visual_scene_objects_lock:
                    self._visual_scene_objects_refresh_running = False

        threading.Thread(
            target=worker,
            name="manyforge_visual_scene_objects_refresh",
            daemon=True,
        ).start()
        return {"success": True, "queued": True, "running": False, "reason": reason}

    def update_visual_state_snapshot(
        self,
        snapshot: Any | None,
        *,
        duration_ms: float | None = None,
        error: str | None = None,
        error_detail: str | None = None,
        reason: str = "",
    ) -> None:
        """Publish the latest immutable visual-state snapshot.

        Observer routes read this cache by cheap copy. Potentially blocking
        operations such as execution-snapshot reads, gripper observer calls, FK,
        and scene-object snapshots belong in the visual publisher, never in
        request handling.
        """
        with self._visual_state_lock:
            self._visual_state_snapshot = copy.deepcopy(snapshot)
            self._visual_state_timestamp_ns = time.monotonic_ns() if snapshot is not None else 0
            self._visual_state_last_duration_ms = duration_ms
            self._visual_state_last_error = error
            self._visual_state_last_error_detail = error_detail
            self._visual_state_last_reason = reason

    def clear_visual_state_snapshot(self) -> None:
        self.update_visual_state_snapshot(
            None,
            duration_ms=None,
            error=None,
            reason="cleared",
        )

    def record_visual_state_publish_error(
        self,
        *,
        duration_ms: float,
        error: str,
        error_detail: str | None = None,
        reason: str,
    ) -> None:
        """Record publisher failure without discarding the last good snapshot."""
        with self._visual_state_lock:
            self._visual_state_last_duration_ms = duration_ms
            self._visual_state_last_error = error
            self._visual_state_last_error_detail = error_detail
            self._visual_state_last_reason = reason

    def visual_state_snapshot(self) -> tuple[Any | None, dict[str, Any]]:
        """Return the latest published visual snapshot and freshness metadata.

        The snapshot is the SHARED published object — read-only by
        contract. A consumer that needs a mutated variant must use
        `snapshot.model_copy(update=...)`, never assign to its fields.
        """
        with self._visual_state_lock:
            # SHARED, not copied: the published snapshot is replaced
            # wholesale by the publisher and treated read-only by every
            # consumer (routes needing a variant use model_copy). The
            # previous per-read deepcopy ran on two 10 Hz poller routes.
            snapshot = self._visual_state_snapshot
            timestamp_ns = self._visual_state_timestamp_ns
            duration_ms = self._visual_state_last_duration_ms
            error = self._visual_state_last_error
            error_detail = self._visual_state_last_error_detail
            reason = self._visual_state_last_reason
            running = self._visual_state_publisher_running
            period_s = self._visual_state_publisher_period_s
        age_ms = None
        if timestamp_ns > 0:
            age_ms = (time.monotonic_ns() - timestamp_ns) / 1_000_000.0
        sequence_number = None
        if snapshot is not None:
            sequence_number = getattr(snapshot, "sequence_number", None)
        return snapshot, {
            "timestamp_ns": timestamp_ns,
            "age_ms": age_ms,
            "last_duration_ms": duration_ms,
            "last_error": error,
            "last_error_detail": error_detail,
            "last_reason": reason,
            "publisher_running": running,
            "publisher_period_s": period_s,
            "sequence_number": sequence_number,
        }

    def publish_visual_state_snapshot_sync(
        self,
        builder: Callable[[], Any],
        *,
        reason: str = "manual",
    ) -> dict[str, Any]:
        """Build and publish a visual snapshot outside route request handling."""
        started = time.monotonic()
        try:
            snapshot = builder()
        except Exception as exc:
            duration_ms = (time.monotonic() - started) * 1000.0
            error_code = str(getattr(exc, "reason", "") or "").strip()
            error_detail = str(getattr(exc, "detail", "") or str(exc)).strip()
            if not error_code:
                error_code = error_detail or exc.__class__.__name__
            if not error_detail:
                error_detail = error_code
            self.record_visual_state_publish_error(
                duration_ms=duration_ms,
                error=error_code,
                error_detail=error_detail,
                reason=reason,
            )
            return {
                "success": False,
                "error": error_code,
                "error_detail": error_detail,
                "reason": reason,
                "duration_ms": round(duration_ms, 1),
            }
        duration_ms = (time.monotonic() - started) * 1000.0
        self.update_visual_state_snapshot(
            snapshot,
            duration_ms=duration_ms,
            error=None,
            reason=reason,
        )
        return {
            "success": True,
            "reason": reason,
            "duration_ms": round(duration_ms, 1),
            "sequence_number": getattr(snapshot, "sequence_number", None),
        }

    def ensure_visual_state_publisher(
        self,
        builder: Callable[[], Any],
        *,
        period_s: float,
    ) -> None:
        """Start or update the background visual-state publisher."""
        period = max(0.02, min(float(period_s), 5.0))
        with self._visual_state_lock:
            self._visual_state_publisher_builder = builder
            self._visual_state_publisher_period_s = period
            thread = self._visual_state_publisher_thread
            if thread is not None and thread.is_alive():
                return
            self._visual_state_publisher_stop.clear()
            self._visual_state_publisher_running = True
            thread = threading.Thread(
                target=self._visual_state_publisher_loop,
                name="manyforge_visual_state_publisher",
                daemon=True,
            )
            self._visual_state_publisher_thread = thread
            thread.start()

    def request_visual_state_publish(self, *, reason: str = "") -> None:
        with self._visual_state_lock:
            if reason:
                self._visual_state_next_reason = reason
        self._visual_state_publisher_wake.set()

    def stop_visual_state_publisher(self, *, timeout_s: float = 1.0) -> None:
        self._visual_state_publisher_stop.set()
        self._visual_state_publisher_wake.set()
        with self._visual_state_lock:
            thread = self._visual_state_publisher_thread
        if (
            thread is not None
            and thread.is_alive()
            and thread is not threading.current_thread()
        ):
            thread.join(timeout=max(0.0, timeout_s))
        with self._visual_state_lock:
            if (
                self._visual_state_publisher_thread is thread
                and (thread is None or not thread.is_alive())
            ):
                self._visual_state_publisher_thread = None
                self._visual_state_publisher_running = False

    def _visual_state_publisher_loop(self) -> None:
        while not self._visual_state_publisher_stop.is_set():
            with self._visual_state_lock:
                builder = self._visual_state_publisher_builder
                period_s = self._visual_state_publisher_period_s
                reason = self._visual_state_next_reason or "periodic"
                self._visual_state_next_reason = ""
            if builder is not None:
                result = self.publish_visual_state_snapshot_sync(
                    builder,
                    reason=reason,
                )
                if not result.get("success", False):
                    logger.debug(
                        "visual-state publisher failed: %s",
                        result.get("error"),
                        extra={
                            "event": "manyforge.visual.snapshot_publish_failed",
                            "reason": reason,
                            "duration_ms": result.get("duration_ms"),
                        },
                    )
                else:
                    duration_ms = float(result.get("duration_ms") or 0.0)
                    logger.debug(
                        "visual-state snapshot published: reason=%s sequence=%s duration_ms=%.1f",
                        reason,
                        result.get("sequence_number"),
                        duration_ms,
                        extra={
                            "event": "manyforge.visual.snapshot_published",
                            "reason": reason,
                            "duration_ms": duration_ms,
                            "sequence_number": result.get("sequence_number"),
                        },
                    )
                    if duration_ms >= 100.0:
                        _log_throttle.warning(
                            logger,
                            "visual.snapshot_build_slow",
                            "visual-state publisher slow: reason=%s duration_ms=%.1f",
                            reason,
                            duration_ms,
                            extra={
                                "event": "manyforge.visual.snapshot_build_slow",
                                "reason": reason,
                                "duration_ms": duration_ms,
                                "sequence_number": result.get("sequence_number"),
                            },
                        )
            self._visual_state_publisher_wake.wait(period_s)
            self._visual_state_publisher_wake.clear()
        with self._visual_state_lock:
            self._visual_state_publisher_running = False

    # Viewer-state accessors: frame + object-frame visibility + gizmo size.
    # These are state-owned so the native viewer and observer routes share one
    # source of truth.

    def get_frame_visibility(self, frame_name: str, default: bool = True) -> bool:
        with self._scene_viewer_lock:
            if frame_name in self._hidden_frames:
                return False
            if frame_name in self._explicit_visible_frames:
                return True
            return default

    def set_frame_visibility(self, frame_name: str, visible: bool) -> None:
        with self._scene_viewer_lock:
            if visible:
                self._hidden_frames.discard(frame_name)
                self._explicit_visible_frames.add(frame_name)
            else:
                self._explicit_visible_frames.discard(frame_name)
                self._hidden_frames.add(frame_name)

    def get_object_frame_visibility(self, object_id: str) -> bool:
        with self._scene_viewer_lock:
            return object_id in self._visible_object_frames

    def set_object_frame_visibility(self, object_id: str, visible: bool) -> None:
        with self._scene_viewer_lock:
            if visible:
                self._visible_object_frames.add(object_id)
            else:
                self._visible_object_frames.discard(object_id)

    def get_frame_gizmo_size(self) -> float:
        with self._scene_viewer_lock:
            return self._frame_gizmo_size_m

    def set_frame_gizmo_size(self, axes_length_m: float) -> None:
        size = float(axes_length_m)
        if size <= 0:
            raise ValueError("frame gizmo size must be positive")
        with self._scene_viewer_lock:
            self._frame_gizmo_size_m = size

    def get_object_axes_gizmo_size(self) -> float:
        with self._scene_viewer_lock:
            return self._object_axes_gizmo_size_m

    def set_object_axes_gizmo_size(self, axes_length_m: float) -> None:
        size = float(axes_length_m)
        if size <= 0:
            raise ValueError("object axes gizmo size must be positive")
        with self._scene_viewer_lock:
            self._object_axes_gizmo_size_m = size

    def _resolve_scene_snapshot_provider(
        self,
    ) -> Optional[Callable[[int, int, str], Optional[bytes]]]:
        """Pick the active snapshot source for the runner.

        The native viewer broker is the only supported render source. If no
        browser viewer is connected, the provider returns ``None`` and the
        model-facing snapshot request fails closed.
        """
        broker = getattr(self, "snapshot_broker", None)
        if broker is None:
            return None

        def provider(width: int, height: int, format: str) -> Optional[bytes]:
            if broker.has_clients():
                return broker.render_sync(width, height, format)
            return None

        return provider

    def _rebuild_runtime(
        self,
        program: Program,
        deployment: Optional[Deployment],
        *,
        effective_program: Optional[Program] = None,
    ) -> None:
        """Rebuild runner + cycle manager from already-validated in-memory models."""
        scene_snapshot_provider = self._resolve_scene_snapshot_provider()
        if self.cycle_manager is not None:
            self.cycle_manager.reset()

        if deployment is not None and self._kernel_factory is not None:
            self._kernel, self.kernel_bundle = self._kernel_factory(deployment)
        drivable_joints = self.refresh_drivable_joint_names(kernel=self._kernel)
        if deployment is not None and drivable_joints:
            deployment.validate_robot_control_joint_limits_against(drivable_joints)

        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=self.program_source_path or "<memory>",
            kernel=self._kernel,
            deployment_path=self.deployment_source_path or None,
            program=effective_program or program,
            deployment=deployment,
            skill_catalog=self.skill_catalog,
            event_recorder=self.event_recorder,
            scene_snapshot_provider=scene_snapshot_provider,
            scene_snapshot_url_publisher=self.publish_scene_snapshot,
            pose_source_apply_callback=self.apply_pose_sources_before_planning,
            pose_source_between_nodes_callback=self.apply_pose_sources_between_nodes,
            simulation_control_callback=self.simulation_control_for_behavior_node,
        )
        runner.load()

        self.program = program
        self._effective_program = effective_program or program
        self.deployment = deployment
        self._evaluate_stop_capability()
        self._update_program_tree_hash()
        self._program_session_generation += 1
        self._clear_published_scene_snapshots()
        self.clear_pose_source_snapshots()
        self.clear_visual_state_snapshot()
        self.cycle_manager = CycleManager(runner, self._kernel)
        self._bind_cycle_manager_callbacks()
        self.reset_backend_authoritative_sim_if_needed(reason="program_load")
        # Explicit load-time scene seed: populate the live planning scene with
        # the program's authored objects now, so they are visible at idle before
        # the first Run. Scene-only (no tree, no robot reset); the first Run/Reset
        # re-seeds via _prepare_fresh_cycle. Replaces the removed implicit
        # frontend authored-scene mirror; the live scene is now written only by
        # the program cycle, explicit Reset, and this explicit load seed.
        self.cycle_manager.seed_authored_scene()

        if self._on_program_loaded:
            try:
                self._on_program_loaded(self)
            except Exception:
                logger.exception("on_program_loaded callback failed")

    @property
    def stop_capability_status(self) -> dict:
        """Cached result of the load-time stop-capability check.

        Keys: ``supported_stop_modes`` (sorted list, or None when the active
        backend does not expose a capability — e.g. the mock kernel),
        ``configured_stop_modes`` (mapping of profile slot -> declared mode),
        ``configured_stop_deceleration_times`` (mapping of profile slot ->
        declared ``deceleration_time_s``), ``unsupported`` (list of
        {profile, stop_mode, reason} the backend does not advertise or could
        not verify), ``capability_query_failed``, and ``capability_profile_id``.
        Additional audit fields mirror the backend capability payload:
        ``hard_stop_level``, ``derivative_proof_level``,
        ``scaling_source_visibility``, ``scaling_source_probe_status``, and
        ``unsupported_reasons``. Empty before any load.
        """
        return getattr(self, "_stop_capability_status", {})

    def motion_arm_admission_block_reason(self) -> Optional[dict]:
        """Authoritative Composer-side pre-motion admission gate.

        Every surface that can arm motion through Composer must call this
        immediately before dispatch. It re-queries the backend capability at
        arm time (not on runtime-status polls), so dispatch fails closed if
        the backend can no longer report its capability.
        """
        self._evaluate_stop_capability(reason="motion_arm")
        return self.stop_capability_block_reason()

    def stop_capability_block_reason(self) -> Optional[dict]:
        """Shared fail-closed pre-motion gate. Returns a structured
        ``{message, error_code}`` when the active deployment's configured stop
        profile cannot be honored by the backend (unsupported mode, or the
        capability could not be verified), else None. Every motion-arming
        surface (cycle run, MCP run, future) must consult this so the gate is
        defined once, not duplicated per-route (which is how surfaces drift
        out of coverage)."""
        stop_cap = self.stop_capability_status
        unsupported = stop_cap.get("unsupported") or []
        if not unsupported:
            return None
        desc = ", ".join(_format_stop_capability_issue(u) for u in unsupported)
        if stop_cap.get("capability_query_failed"):
            return {
                "message": (
                    "Execution blocked: the execution backend could not report its "
                    f"supported stop modes, so the configured stop profile(s) ({desc}) "
                    "cannot be verified. Reload once the backend is healthy."
                ),
                "error_code": "stop_capability_query_failed",
            }
        reasons = {u.get("reason") for u in unsupported}
        if "stop_profile_required" in reasons:
            return {
                "message": (
                    "Execution blocked: executable deployments must configure both "
                    "operator and fault controlled-stop profiles. Configure a real "
                    f"stop profile for: {desc}."
                ),
                "error_code": "stop_profile_required",
            }
        if "hard_stop_floor_unavailable" in reasons:
            return {
                "message": (
                    "Execution blocked: the execution backend did not report a valid "
                    "hard-stop floor. Controlled stops need a mandatory hard-stop "
                    f"floor for refused, timed-out, or unconfirmed stops ({desc})."
                ),
                "error_code": "hard_stop_floor_unavailable",
            }
        advertised = ", ".join(stop_cap.get("supported_stop_modes") or []) or "none"
        return {
            "message": (
                "Execution blocked: the deployment's configured stop profile uses a "
                f"stop mode the execution backend does not support ({desc}). Backend "
                f"advertises: {advertised}. Choose a supported stop profile."
            ),
            "error_code": "stop_profile_unsupported",
        }

    def _evaluate_stop_capability(self, *, reason: str = "load") -> None:
        """Evaluate once, at program load, whether the deployment's configured
        operator/fault stop profiles are advertised by the active execution
        backend, and cache the result for the runtime-status gate.

        Fail-closed contract:
        - If a configured stop mode is not in the backend's
          ``supported_stop_modes`` -> block (reason ``stop_mode_not_supported``).
        - If the deployment declares stop profiles but the backend capability
          query FAILS (throws, or returns a non-dict) -> block (reason
          ``capability_query_failed``); an unverifiable profile is never allowed
          to run.
        - If there is no capability accessor at all and the deployment declares
          stop profiles, the configured profiles are unverifiable -> block
          (reason ``capability_query_failed``). Deployments without configured
          stop profiles remain ungated for composer-only/test lanes.

        The backend capability is authoritative (computed in C++ from the
        deployment's controller config). Runtime-status reads consume this
        cache without polling. Motion-arm admission re-runs this method so a
        backend that can no longer report capability is caught before dispatch.
        """
        status: dict = {
            "supported_stop_modes": None,
            "configured_stop_modes": {},
            "configured_stop_deceleration_times": {},
            "unsupported": [],
            "unsupported_reasons": {},
            "capability_profile_id": None,
            "hard_stop_level": None,
            "derivative_proof_level": None,
            "scaling_source_visibility": None,
            "scaling_source_probe_status": None,
            "capability_query_failed": False,
            "evaluated_at_ns": time.monotonic_ns(),
            "evaluation_reason": reason,
        }
        deployment = self.deployment
        required_slots = ("operator_stop_profile", "default_fault_stop_profile")
        execution_config_present = False
        # Read the configured stop profiles FIRST, independently of the backend
        # query, so that even if the query fails we still know whether the
        # deployment declared profiles that must be verified (fail-closed).
        configured: dict = {}
        configured_deceleration_times: dict = {}
        if deployment is not None:
            execution = deployment.adapters.get("execution")
            cfg = getattr(execution, "config", None) or {}
            execution_config_present = execution is not None
            for slot in required_slots:
                profile = cfg.get(slot)
                if isinstance(profile, dict) and profile.get("stop_mode"):
                    configured[slot] = profile["stop_mode"]
                    decel = profile.get("deceleration_time_s")
                    if isinstance(decel, (int, float)) and not isinstance(decel, bool):
                        configured_deceleration_times[slot] = float(decel)
        status["configured_stop_modes"] = configured
        status["configured_stop_deceleration_times"] = configured_deceleration_times

        bundle = self.kernel_bundle
        # The native path exposes the raw NativeKernelBundle; the ros2_control
        # path wraps it (Ros2ControlRuntimeBundle.native_bundle). Reach the
        # underlying bundle that carries the capability accessor either way.
        get_cap = getattr(bundle, "get_motion_interrupt_stop_capability", None)
        if not callable(get_cap):
            inner = getattr(bundle, "native_bundle", None)
            get_cap = getattr(inner, "get_motion_interrupt_stop_capability", None)
        if not callable(get_cap):
            get_cap = getattr(self._kernel, "get_motion_interrupt_stop_capability", None)

        if deployment is None:
            self._stop_capability_status = status
            return
        if not callable(get_cap):
            if configured:
                status["capability_query_failed"] = True
                for slot, mode in configured.items():
                    status["unsupported"].append(
                        {"profile": slot, "stop_mode": mode,
                         "reason": "capability_query_failed"}
                    )
            self._stop_capability_status = status
            return

        cap = None
        query_failed = False
        try:
            cap = get_cap()
        except Exception:
            logger.exception("stop-capability query failed during %s", reason)
            query_failed = True
        if not isinstance(cap, dict):
            query_failed = True

        if query_failed:
            # A real backend could not report its stop capability: any
            # configured stop profile is unverifiable -> block, fail-closed.
            if configured:
                status["capability_query_failed"] = True
                for slot, mode in configured.items():
                    status["unsupported"].append(
                        {"profile": slot, "stop_mode": mode,
                         "reason": "capability_query_failed"}
                    )
            self._stop_capability_status = status
            return

        supported = set(cap.get("supported_stop_modes") or [])
        status["supported_stop_modes"] = sorted(supported)
        status["capability_profile_id"] = cap.get("capability_profile_id")
        status["hard_stop_level"] = cap.get("hard_stop_level")
        status["derivative_proof_level"] = cap.get("derivative_proof_level")
        status["scaling_source_visibility"] = cap.get("scaling_source_visibility")
        status["scaling_source_probe_status"] = cap.get("scaling_source_probe_status")
        if isinstance(cap.get("unsupported_reasons"), dict):
            status["unsupported_reasons"] = dict(cap["unsupported_reasons"])
        if execution_config_present:
            for slot in required_slots:
                if slot not in configured:
                    status["unsupported"].append(
                        {"profile": slot, "stop_mode": "",
                         "reason": "stop_profile_required"}
                    )
        hard_stop_level = status["hard_stop_level"]
        hard_stop_level_text = hard_stop_level if isinstance(hard_stop_level, str) else ""
        if execution_config_present and (
            not hard_stop_level_text.strip()
            or hard_stop_level_text == "unavailable"
        ):
            status["unsupported"].append(
                {"profile": "hard_stop_floor",
                 "stop_mode": hard_stop_level_text or "",
                 "reason": "hard_stop_floor_unavailable"}
            )
        for slot, mode in configured.items():
            if mode not in supported:
                backend_reason = status["unsupported_reasons"].get(mode)
                status["unsupported"].append(
                    {"profile": slot, "stop_mode": mode,
                     "reason": backend_reason or "stop_mode_not_supported"}
                )
        self._stop_capability_status = status

    def apply_models(
        self,
        program: Program,
        deployment: Optional[Deployment],
        *,
        force_discard_overrides: bool = False,
    ) -> None:
        with self._program_edit_lock:
            self._apply_models_locked(
                program,
                deployment,
                force_discard_overrides=force_discard_overrides,
            )

    def _apply_models_locked(
        self,
        program: Program,
        deployment: Optional[Deployment],
        *,
        force_discard_overrides: bool = False,
    ) -> None:
        """Apply validated program/deployment models to the runtime session.

        If runtime overrides exist, this method rejects by default.
        Pass ``force_discard_overrides=True`` to explicitly discard them,
        or materialize/clear them before calling this method.
        """
        with self._runtime_override_lock:
            has_runtime_overrides = bool(self._runtime_overrides)
        if has_runtime_overrides:
            if force_discard_overrides:
                self._clear_runtime_overrides()
            else:
                raise RuntimeError(
                    "Active or staged runtime overrides exist; "
                    "materialize (promote) or clear them before changing the program, "
                    "or pass force_discard_overrides=True to discard"
                )
        # Frame visibility overrides are scoped to a (program, deployment)
        # pair: switching to a different robot can reuse link names with
        # entirely different semantics, so stale toggles must not carry
        # over.
        with self._scene_viewer_lock:
            self._hidden_frames.clear()
            self._explicit_visible_frames.clear()
            self._visible_object_frames.clear()
        self.clear_visual_scene_objects_snapshot()
        self.clear_visual_state_snapshot()
        self._rebuild_runtime(program, deployment, effective_program=program)

    def load_program(
        self,
        program_path: str,
        deployment_path: Optional[str] = None,
        *,
        force_discard_overrides: bool = False,
        defer_initial_runtime_reset: bool = False,
    ) -> Program:
        """Load a program (and optional deployment). Creates new runner + CycleManager.

        If runtime overrides exist from a previous program, this method rejects
        by default.  Pass ``force_discard_overrides=True`` to explicitly discard
        them.
        """
        with self._runtime_override_lock:
            has_runtime_overrides = bool(self._runtime_overrides)
        if has_runtime_overrides:
            if not force_discard_overrides:
                raise RuntimeError(
                    "Active or staged runtime overrides exist; "
                    "materialize (promote) or clear them before loading a new program, "
                    "or pass force_discard_overrides=True to discard"
                )
            self._clear_runtime_overrides()

        # Stop existing cycle manager if running
        if self.cycle_manager is not None:
            self.cycle_manager.reset()

        # Load deployment first (if provided), but do not publish the new
        # deployment/kernel into session state until every cross-check below
        # has passed. A failed load must leave the previous runtime intact.
        scene_snapshot_provider = self._resolve_scene_snapshot_provider()
        deployment = self.deployment
        deployment_source_path = self.deployment_source_path
        if deployment_path:
            deployment = Deployment(deployment_path)
            deployment.validate_assistant_modes(self.catalog, self.skill_catalog)
            deployment_source_path = deployment_path
        elif deployment is not None:
            deployment.validate_assistant_modes(self.catalog, self.skill_catalog)

        kernel = self._kernel
        kernel_bundle = self.kernel_bundle
        if deployment is not None and self._kernel_factory is not None:
            kernel, kernel_bundle = self._kernel_factory(deployment)

        # Spec/430 §3.2 cross-validation requires the bound robot's drivable
        # joint list. Run it for both /api/session/apply and /api/program/load
        # so typo'd per-joint Robot Control caps fail at load time, not on first use.
        drivable_joints: list[str] = []
        if deployment is not None and kernel is not None:
            try:
                drivable_joints = [str(name) for name in kernel.get_joint_names()]
            except Exception:
                drivable_joints = []
            if drivable_joints:
                deployment.validate_robot_control_joint_limits_against(drivable_joints)

        # Create runner
        runner = OrchestratorRunner(
            catalog_path=self._catalog_path,
            program_path=program_path,
            kernel=kernel,
            deployment_path=deployment_path,
            deployment=deployment,
            skill_catalog=self.skill_catalog,
            event_recorder=self.event_recorder,
            scene_snapshot_provider=scene_snapshot_provider,
            scene_snapshot_url_publisher=self.publish_scene_snapshot,
            pose_source_apply_callback=self.apply_pose_sources_before_planning,
            pose_source_between_nodes_callback=self.apply_pose_sources_between_nodes,
            simulation_control_callback=self.simulation_control_for_behavior_node,
        )
        runner.load()

        self.program = runner.program
        self._effective_program = runner.program
        self.program_source_path = program_path
        self.deployment = deployment
        self.deployment_source_path = deployment_source_path
        self._kernel = kernel
        self.kernel_bundle = kernel_bundle
        self._evaluate_stop_capability()
        joint_names_to_cache = (
            drivable_joints
            if drivable_joints or deployment is not None
            else self.get_drivable_joint_names()
        )
        with self._joint_state_lock:
            self._drivable_joint_names = list(joint_names_to_cache)
        self._update_program_tree_hash()
        self._program_session_generation += 1
        self._clear_published_scene_snapshots()
        with self._scene_viewer_lock:
            self._hidden_frames.clear()
            self._explicit_visible_frames.clear()
            self._visible_object_frames.clear()
        self.clear_pose_source_snapshots()
        self.clear_visual_scene_objects_snapshot()
        self.clear_visual_state_snapshot()
        self.cycle_manager = CycleManager(runner, self._kernel)
        self._bind_cycle_manager_callbacks()
        if not defer_initial_runtime_reset:
            self.reset_backend_authoritative_sim_if_needed(reason="program_load")
            # Explicit load-time scene seed (see _rebuild_runtime): populate the
            # live planning scene with the program's authored objects at load so
            # they are visible at idle before the first Run. Scene-only; replaces
            # the removed implicit frontend authored-scene mirror.
            self.cycle_manager.seed_authored_scene()

        # Notify runtime visual listeners.
        if self._on_program_loaded:
            try:
                self._on_program_loaded(self)
            except Exception:
                logger.exception("on_program_loaded callback failed")

        return self.program

    def update_external_runtime_status(self, provider_id: str, payload: dict[str, Any]) -> None:
        """Record a deployment-side runtime status contribution.

        Bringup supervisors and adapters can use this narrow HTTP-fed surface to
        report external readiness without introducing ROS dependencies into
        Composer.  The runtime status route applies freshness filtering before
        exposing the contribution to the UI.
        """
        provider = provider_id.strip()
        if not provider:
            raise ValueError("provider_id must not be empty")
        record = dict(payload)
        record["providerId"] = provider
        record["receivedAtNs"] = time.monotonic_ns()
        with self._external_runtime_status_lock:
            self._external_runtime_statuses[provider] = record

    def update_pose_source_snapshot(
        self,
        pose_source_id: str,
        payload: dict[str, Any],
    ) -> None:
        """Record the latest snapshot from one pose-source sidecar.

        Composer stores sidecar snapshots separately from the planning scene.
        Mutating policies are handled by the explicit apply layer, including the
        `continuous` policy that the snapshot route invokes after this latest
        value is recorded. This keeps the provider endpoint latest-wins and
        avoids accumulating a pose-update backlog.
        """
        source = pose_source_id.strip()
        if not source:
            raise ValueError("pose_source_id must not be empty")
        record = copy.deepcopy(payload)
        record["pose_source_id"] = source
        record["received_at_ns"] = time.monotonic_ns()
        with self._pose_source_lock:
            self._pose_source_snapshots[source] = record
        self._record_pose_source_snapshot_metric(source, record)
        self._evaluate_pose_source_grasp_integrity(source, record)

    def _pose_source_scene_edit_pose(
        self,
        *,
        object_id: str,
        position: list[float],
        orientation: list[float],
        frame_id: str,
        detail: dict[str, Any],
    ) -> tuple[list[float] | None, list[float] | None, str | None]:
        """Return the pose/frame to send to scene_edit for a source update.

        Detached objects are updated in their configured source frame. Attached
        objects are different: applying a world-frame pose directly would change
        the object's frame to `universe` and silently detach it. For attached
        objects, convert the observed pose through the current attachment chain
        and update the object as an offset in the attachment frame.
        """
        object_state = self.kernel.get_object_state(object_id)
        apply_frame = str(frame_id or "universe")
        if not object_state.attached:
            detail["attached"] = False
            return position, orientation, apply_frame

        attached_frame = str(object_state.frame_id or "")
        detail["attached"] = True
        detail["attached_frame_id"] = attached_frame
        if not attached_frame:
            detail["status"] = "rejected"
            detail["reject_reason"] = "missing_attachment_frame"
            return None, None, None

        if apply_frame == attached_frame:
            detail["attached_update_mode"] = "attachment_frame"
            return position, orientation, attached_frame

        try:
            observed_transform = _pose_to_matrix(position, orientation)
            if apply_frame in {"", "universe"}:
                world_transform = observed_transform
            else:
                source_pose = self.kernel.get_link_pose(apply_frame, "universe")
                if source_pose is None:
                    detail["status"] = "rejected"
                    detail["reject_reason"] = "source_frame_unresolved"
                    detail["frame_id"] = apply_frame
                    return None, None, None
                source_transform = _pose_to_matrix(
                    list(source_pose["position_m"]),
                    list(source_pose["orientation_xyzw"]),
                )
                world_transform = _multiply_matrices(source_transform, observed_transform)

            attached_pose = self.kernel.get_link_pose(attached_frame, "universe")
            if attached_pose is None:
                detail["status"] = "rejected"
                detail["reject_reason"] = "attached_frame_unresolved"
                detail["attached_frame_id"] = attached_frame
                return None, None, None
            attached_transform = _pose_to_matrix(
                list(attached_pose["position_m"]),
                list(attached_pose["orientation_xyzw"]),
            )
            local_position, local_orientation = _matrix_to_pose(
                _multiply_matrices(_invert_matrix(attached_transform), world_transform)
            )
        except (KeyError, TypeError, ValueError) as exc:
            detail["status"] = "rejected"
            detail["reject_reason"] = "attached_pose_transform_failed"
            detail["error"] = str(exc)
            return None, None, None

        detail["attached_update_mode"] = "attachment_offset_from_observation"
        detail["observed_apply_frame_id"] = apply_frame
        return local_position, local_orientation, attached_frame

    def _pose_source_relative_tcp_object_pose(
        self,
        *,
        tcp_pose: dict[str, Any],
        object_pose: dict[str, Any],
    ) -> dict[str, Any]:
        tcp_matrix = _pose_to_matrix(
            list(tcp_pose["position_m"]),
            list(tcp_pose["orientation_xyzw"]),
        )
        object_matrix = _pose_to_matrix(
            list(object_pose["position_m"]),
            list(object_pose["orientation_xyzw"]),
        )
        position, orientation = _matrix_to_pose(
            _multiply_matrices(_invert_matrix(tcp_matrix), object_matrix)
        )
        return {"position_m": position, "orientation_xyzw": orientation}

    def _pose_source_pose_dict(self, pose: dict[str, Any]) -> dict[str, Any] | None:
        position = pose.get("position_xyz") or pose.get("positionXYZ")
        orientation = pose.get("orientation_xyzw") or pose.get("orientationXYZW")
        if (
            not isinstance(position, list)
            or len(position) != 3
            or not isinstance(orientation, list)
            or len(orientation) != 4
        ):
            return None
        try:
            return {
                "position_m": [float(value) for value in position],
                "orientation_xyzw": [float(value) for value in orientation],
            }
        except (TypeError, ValueError):
            return None

    def _set_pose_source_drift_state(
        self,
        key: tuple[str, str],
        state: dict[str, Any],
    ) -> None:
        with self._pose_source_drift_lock:
            self._pose_source_drift_states[key] = copy.deepcopy(state)

    def _publish_pose_source_drift_condition(
        self,
        *,
        source_id: str,
        object_id: str,
        signal_id: str,
        value: bool,
        stale_after_s: float,
        metadata: dict[str, Any],
    ) -> None:
        manager = self.cycle_manager
        if manager is None:
            return
        try:
            manager.publish_signal(
                adapter=f"pose_source:{source_id}",
                signal=signal_id,
                value=value,
                stale_after_ms=max(1, int(float(stale_after_s) * 1000.0)),
                mirror_key=signal_id,
                clear_on_stale=True,
                metadata={
                    "source_id": source_id,
                    "object_id": object_id,
                    **copy.deepcopy(metadata),
                },
            )
        except Exception:
            logger.debug("pose-source drift signal publish failed", exc_info=True)

    def _publish_pose_source_grasp_confirmation(
        self,
        *,
        source_id: str,
        object_id: str,
        confirmation_signal_id: str,
        value: bool,
        stale_after_s: float,
        metadata: dict[str, Any],
    ) -> None:
        manager = self.cycle_manager
        if manager is None:
            return
        signal_id = str(confirmation_signal_id or "").strip()
        if not signal_id:
            return
        try:
            manager.publish_signal(
                adapter=f"pose_source:{source_id}",
                signal=signal_id,
                value=bool(value),
                stale_after_ms=max(1, int(float(stale_after_s) * 1000.0)),
                mirror_key=signal_id,
                clear_on_stale=True,
                metadata={
                    "source_id": source_id,
                    "object_id": object_id,
                    **copy.deepcopy(metadata),
                },
            )
        except Exception:
            logger.debug(
                "pose-source grasp confirmation signal publish failed",
                exc_info=True,
            )

    def _record_pose_source_drift_event(
        self,
        action: str,
        *,
        details: dict[str, Any],
    ) -> None:
        try:
            self.event_recorder.record_pose_source_event(
                f"drift_{action}",
                details=copy.deepcopy(details),
            )
        except Exception:
            logger.debug("pose-source drift event recording failed", exc_info=True)

    def _gripper_ids_for_attached_frame(self, attached_frame_id: str) -> list[str]:
        deployment = self.deployment
        if deployment is None:
            return []
        frame_id = str(attached_frame_id or "").strip()
        if not frame_id:
            return []

        matches: list[str] = []
        for gripper_id, gripper in sorted(deployment.grippers.items()):
            candidate_frames = {
                str(getattr(gripper, "attach_frame", "") or "").strip(),
                str(getattr(gripper, "tcp_frame", "") or "").strip(),
                str(getattr(gripper, "mount_frame", "") or "").strip(),
            }
            candidate_frames.update(
                str(frame or "").strip()
                for frame in getattr(gripper, "auxiliary_frames", [])
            )
            candidate_frames.discard("")
            if frame_id in candidate_frames:
                matches.append(str(gripper_id))
        return matches

    def _mark_attached_object_grasp_lost(
        self,
        *,
        object_id: str,
        attached_frame_id: str,
        evidence_label: str,
        message: str,
    ) -> dict[str, Any]:
        """Propagate measured attached-object loss into gripper status.

        The measured-drift monitor is the primary fault source. This helper only
        updates gripper evidence when the attached frame maps to exactly one
        deployment gripper; ambiguous matches are surfaced in diagnostics rather
        than guessed.
        """

        frame_id = str(attached_frame_id or "").strip()
        if not frame_id:
            return {
                "gripper_grasp_loss": {
                    "status": "not_recorded",
                    "reason": "attached_frame_missing",
                }
            }

        matches = self._gripper_ids_for_attached_frame(frame_id)
        if not matches:
            return {
                "gripper_grasp_loss": {
                    "status": "not_recorded",
                    "reason": "no_matching_gripper",
                    "attached_frame_id": frame_id,
                }
            }
        if len(matches) > 1:
            return {
                "gripper_grasp_loss": {
                    "status": "not_recorded",
                    "reason": "ambiguous_gripper_frame_match",
                    "attached_frame_id": frame_id,
                    "gripper_ids": matches,
                }
            }

        gripper_id = matches[0]
        try:
            result = self.kernel.mark_gripper_grasp_lost(
                gripper_id=gripper_id,
                evidence_label=evidence_label,
                message=message,
                reason_code="grasp_confirmation_lost",
            )
        except Exception as exc:
            logger.debug("gripper grasp-loss status update failed", exc_info=True)
            return {
                "gripper_grasp_loss": {
                    "status": "not_recorded",
                    "reason": type(exc).__name__,
                    "attached_frame_id": frame_id,
                    "gripper_id": gripper_id,
                }
            }
        return {
            "gripper_grasp_loss": {
                "status": "recorded" if result.accepted else "rejected",
                "reason": result.reason_code,
                "error": result.error,
                "attached_frame_id": frame_id,
                "gripper_id": gripper_id,
                "object_id": object_id,
            }
        }

    def pose_source_drift_states(self) -> dict[tuple[str, str], dict[str, Any]]:
        with self._pose_source_drift_lock:
            return copy.deepcopy(self._pose_source_drift_states)

    def _pose_source_baseline_captured(self, key: tuple[str, str]) -> bool:
        with self._pose_source_drift_lock:
            return key in self._pose_source_drift_baselines

    def _set_pose_source_unverifiable(
        self,
        *,
        source_id: str,
        object_id: str,
        key: tuple[str, str],
        source: Any,
        monitor: Any,
        reason: str,
        baseline_captured: bool,
        attached_frame_id: str = "",
        metadata: dict[str, Any] | None = None,
    ) -> None:
        signal_id = getattr(monitor, "signal_id", f"attached_object_drift/{object_id}")
        action = getattr(monitor, "action", "diagnostic_only")
        stale_behavior = getattr(monitor, "stale_behavior", "unverifiable")
        details = {
            "source_id": source_id,
            "object_id": object_id,
            "signal_id": signal_id,
            "reason": reason,
            "stale_behavior": stale_behavior,
            "action": action,
            "baseline_captured": baseline_captured,
            **copy.deepcopy(metadata or {}),
        }
        if attached_frame_id:
            details["attached_frame_id"] = attached_frame_id
        with self._pose_source_drift_lock:
            previous_state = copy.deepcopy(self._pose_source_drift_states.get(key))
        state_changed = (
            not isinstance(previous_state, dict)
            or previous_state.get("status") != "unverifiable"
            or previous_state.get("unverifiable_reason") != reason
            or previous_state.get("baseline_captured") != baseline_captured
        )
        self._set_pose_source_drift_state(
            key,
            {
                "status": "unverifiable",
                "action": action,
                "signal_id": signal_id,
                "baseline_captured": baseline_captured,
                "unverifiable_reason": reason,
                "metadata": details,
            },
        )
        if state_changed:
            self._record_pose_source_drift_event("unverifiable", details=details)

        if monitor is None or not getattr(monitor, "enabled", False):
            return
        self._publish_pose_source_grasp_confirmation(
            source_id=source_id,
            object_id=object_id,
            confirmation_signal_id=getattr(
                monitor,
                "confirmation_signal_id",
                "",
            ),
            value=False,
            stale_after_s=getattr(source, "stale_after_s", 1.0),
            metadata=details,
        )
        should_fault = stale_behavior == "fault"
        self._publish_pose_source_drift_condition(
            source_id=source_id,
            object_id=object_id,
            signal_id=signal_id,
            value=bool(should_fault),
            stale_after_s=getattr(source, "stale_after_s", 1.0),
            metadata=details,
        )
        if not should_fault or action != "interrupt" or self.cycle_manager is None:
            return

        with self._pose_source_drift_lock:
            already_raised = key in self._pose_source_drift_raised
            if not already_raised:
                self._pose_source_drift_raised.add(key)
        if already_raised:
            return
        if attached_frame_id:
            details.update(
                self._mark_attached_object_grasp_lost(
                    object_id=object_id,
                    attached_frame_id=attached_frame_id,
                    evidence_label="attached_drift_monitor",
                    message=(
                        f"Attached object '{object_id}' measured grasp became "
                        f"unverifiable: {reason}"
                    ),
                )
            )
        try:
            self.cycle_manager.raise_runtime_condition_fault(
                condition_id=signal_id,
                message=(
                    f"Attached object '{object_id}' measured grasp became "
                    f"unverifiable: {reason}"
                ),
                details={
                    "reason_code": "attached_object_drift_unverifiable",
                    **details,
                },
            )
        except AttributeError:
            logger.warning(
                "attached object drift unverifiable but cycle fault hook is unavailable",
                extra={
                    "event": "manyforge.pose_source.drift_unverifiable_interrupt_unavailable",
                    "object_id": object_id,
                    "source_id": source_id,
                    "reason": reason,
                },
            )
        except Exception:
            logger.exception("attached object drift unverifiable interrupt failed")

    def _evaluate_pose_source_grasp_integrity(
        self,
        source_id: str,
        snapshot: dict[str, Any],
    ) -> None:
        deployment = self.deployment
        if deployment is None:
            return
        source = deployment.pose_sources.get(source_id)
        if source is None or not source.enabled:
            return

        measured_tcp = snapshot.get("measured_tcp") or snapshot.get("measuredTcp")
        measured_tcp_pose_payload = (
            measured_tcp.get("pose") if isinstance(measured_tcp, dict) else None
        )
        measured_tcp_pose = (
            self._pose_source_pose_dict(measured_tcp_pose_payload)
            if isinstance(measured_tcp_pose_payload, dict)
            else None
        )
        measured_tcp_frame = ""
        measured_tcp_monotonic_s: float | None = None
        if isinstance(measured_tcp, dict):
            if isinstance(measured_tcp_pose_payload, dict):
                measured_tcp_frame = str(
                    measured_tcp.get("frame_id")
                    or measured_tcp.get("frameId")
                    or measured_tcp_pose_payload.get("frame_id")
                    or measured_tcp_pose_payload.get("frameId")
                    or ""
                )
            else:
                measured_tcp_frame = str(
                    measured_tcp.get("frame_id")
                    or measured_tcp.get("frameId")
                    or ""
                )
            try:
                measured_tcp_monotonic_s = float(
                    measured_tcp.get("monotonic_s") or measured_tcp.get("monotonicS")
                )
            except (TypeError, ValueError):
                measured_tcp_monotonic_s = None

        snapshot_objects = {
            str(item.get("object_id") or item.get("objectId") or ""): item
            for item in snapshot.get("objects", [])
            if isinstance(item, dict)
        }
        now_ns = time.monotonic_ns()
        for object_id, binding in sorted(source.objects.items()):
            monitor = binding.attached_drift_monitor
            ghost = binding.measured_state_ghost
            baseline_config = binding.measured_attachment_baseline
            key = (source_id, object_id)
            if monitor is None and baseline_config is None and ghost is None:
                with self._pose_source_drift_lock:
                    self._pose_source_drift_states.pop(key, None)
                    self._pose_source_drift_baselines.pop(key, None)
                    self._pose_source_drift_exceeded_since_ns.pop(key, None)
                    self._pose_source_drift_raised.discard(key)
                continue
            try:
                object_state = self.kernel.get_object_state(object_id)
            except Exception as exc:
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason=f"object_state_unavailable:{type(exc).__name__}",
                    baseline_captured=False,
                )
                continue
            if not getattr(object_state, "attached", False):
                attached_frame_id = str(getattr(object_state, "frame_id", "") or "")
                with self._pose_source_drift_lock:
                    self._pose_source_drift_baselines.pop(key, None)
                    self._pose_source_drift_exceeded_since_ns.pop(key, None)
                    self._pose_source_drift_raised.discard(key)
                self._set_pose_source_drift_state(
                    key,
                    {
                        "status": "disarmed",
                        "action": getattr(monitor, "action", "diagnostic_only"),
                        "signal_id": getattr(
                            monitor,
                            "signal_id",
                            f"attached_object_drift/{object_id}",
                        ),
                        "baseline_captured": False,
                        "unverifiable_reason": "object_not_attached",
                    },
                )
                if monitor is not None and monitor.enabled:
                    self._publish_pose_source_grasp_confirmation(
                        source_id=source_id,
                        object_id=object_id,
                        confirmation_signal_id=monitor.confirmation_signal_id,
                        value=False,
                        stale_after_s=source.stale_after_s,
                        metadata={
                            "source_id": source_id,
                            "object_id": object_id,
                            "attached_frame_id": attached_frame_id,
                            "reason": "object_not_attached",
                            "status": "disarmed",
                            "baseline_captured": False,
                        },
                    )
                continue

            observed = snapshot_objects.get(object_id)
            update = observed.get("update") if isinstance(observed, dict) else None
            pose_payload = update.get("pose") if isinstance(update, dict) else None
            object_pose = (
                self._pose_source_pose_dict(pose_payload)
                if isinstance(pose_payload, dict)
                else None
            )
            if not isinstance(observed, dict) or observed.get("status") != "fresh":
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason="object_sample_not_fresh",
                    baseline_captured=self._pose_source_baseline_captured(key),
                    attached_frame_id=str(getattr(object_state, "frame_id", "") or ""),
                )
                continue
            if object_pose is None:
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason="object_pose_malformed",
                    baseline_captured=self._pose_source_baseline_captured(key),
                    attached_frame_id=str(getattr(object_state, "frame_id", "") or ""),
                )
                continue
            if measured_tcp_pose is None or measured_tcp_monotonic_s is None:
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason="measured_tcp_missing",
                    baseline_captured=self._pose_source_baseline_captured(key),
                    attached_frame_id=str(getattr(object_state, "frame_id", "") or ""),
                )
                continue
            if isinstance(pose_payload, dict):
                object_frame = str(
                    pose_payload.get("frame_id")
                    or pose_payload.get("frameId")
                    or binding.frame_id
                    or source.frame_id
                    or ""
                )
            else:
                object_frame = str(binding.frame_id or source.frame_id or "")
            if measured_tcp_frame and object_frame and measured_tcp_frame != object_frame:
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason="frame_mismatch",
                    baseline_captured=self._pose_source_baseline_captured(key),
                    attached_frame_id=str(getattr(object_state, "frame_id", "") or ""),
                    metadata={
                        "tcp_frame_id": measured_tcp_frame,
                        "object_frame_id": object_frame,
                    },
                )
                continue
            try:
                object_monotonic_s = float(
                    update.get("monotonic_s") or update.get("monotonicS")
                )
            except (TypeError, ValueError, AttributeError):
                object_monotonic_s = None
            tolerance_s = (
                monitor.sample_time_tolerance_s
                if monitor is not None
                else 0.05
            )
            sample_time_delta_s = (
                abs(float(measured_tcp_monotonic_s) - float(object_monotonic_s))
                if object_monotonic_s is not None
                else None
            )
            if sample_time_delta_s is None or sample_time_delta_s > tolerance_s:
                self._set_pose_source_unverifiable(
                    source_id=source_id,
                    object_id=object_id,
                    key=key,
                    source=source,
                    monitor=monitor,
                    reason="sample_time_mismatch",
                    baseline_captured=self._pose_source_baseline_captured(key),
                    attached_frame_id=str(getattr(object_state, "frame_id", "") or ""),
                    metadata={"sample_time_delta_s": sample_time_delta_s},
                )
                continue

            current_relative = self._pose_source_relative_tcp_object_pose(
                tcp_pose=measured_tcp_pose,
                object_pose=object_pose,
            )
            with self._pose_source_drift_lock:
                baseline = self._pose_source_drift_baselines.get(key)
                if baseline is None:
                    baseline = {
                        "pose": copy.deepcopy(current_relative),
                        "captured_at_ns": now_ns,
                        "tcp_monotonic_s": measured_tcp_monotonic_s,
                        "object_monotonic_s": object_monotonic_s,
                        "attached_frame_id": str(getattr(object_state, "frame_id", "") or ""),
                    }
                    self._pose_source_drift_baselines[key] = baseline
                    self._pose_source_drift_exceeded_since_ns.pop(key, None)
                    self._pose_source_drift_raised.discard(key)
                    self._record_pose_source_drift_event(
                        "baseline_captured",
                        details={
                            "source_id": source_id,
                            "object_id": object_id,
                            "attached_frame_id": str(
                                getattr(object_state, "frame_id", "") or ""
                            ),
                            "tcp_monotonic_s": measured_tcp_monotonic_s,
                            "object_monotonic_s": object_monotonic_s,
                            "sample_time_delta_s": sample_time_delta_s,
                        },
                    )
            if monitor is None or not monitor.enabled:
                self._set_pose_source_drift_state(
                    key,
                    {
                        "status": "baseline_captured",
                        "action": getattr(monitor, "action", "diagnostic_only"),
                        "signal_id": getattr(
                            monitor,
                            "signal_id",
                            f"attached_object_drift/{object_id}",
                        ),
                        "baseline_captured": True,
                        "sample_time_delta_s": sample_time_delta_s,
                        "metadata": {
                            "attached_frame_id": str(getattr(object_state, "frame_id", "") or ""),
                        },
                    },
                )
                continue

            translation_m, rotation_rad = _relative_pose_delta(
                baseline["pose"],
                current_relative,
            )
            translation_exceeded = (
                monitor.translation_threshold_m is not None
                and translation_m > monitor.translation_threshold_m
            )
            rotation_exceeded = (
                monitor.rotation_threshold_rad is not None
                and rotation_rad > monitor.rotation_threshold_rad
            )
            exceeded = translation_exceeded or rotation_exceeded
            with self._pose_source_drift_lock:
                if exceeded:
                    exceeded_since_ns = self._pose_source_drift_exceeded_since_ns.setdefault(
                        key,
                        now_ns,
                    )
                else:
                    self._pose_source_drift_exceeded_since_ns.pop(key, None)
                    self._pose_source_drift_raised.discard(key)
                    exceeded_since_ns = None
                debounced = (
                    exceeded_since_ns is not None
                    and (now_ns - exceeded_since_ns)
                    >= int(float(monitor.debounce_s) * 1_000_000_000)
                )
                already_raised = key in self._pose_source_drift_raised
                if debounced:
                    self._pose_source_drift_raised.add(key)

            status = "exceeded" if debounced else ("near_limit" if exceeded else "ok")
            condition_payload = {
                "source_id": source_id,
                "object_id": object_id,
                "attached_frame_id": str(getattr(object_state, "frame_id", "") or ""),
                "signal_id": monitor.signal_id,
                "translation_m": translation_m,
                "rotation_rad": rotation_rad,
                "translation_threshold_m": monitor.translation_threshold_m,
                "rotation_threshold_rad": monitor.rotation_threshold_rad,
                "sample_time_delta_s": sample_time_delta_s,
                "debounce_s": monitor.debounce_s,
                "baseline": copy.deepcopy(baseline),
                "current_measured_offset": copy.deepcopy(current_relative),
            }
            if debounced and not already_raised and monitor.action == "interrupt":
                condition_payload.update(
                    self._mark_attached_object_grasp_lost(
                        object_id=object_id,
                        attached_frame_id=str(
                            getattr(object_state, "frame_id", "") or ""
                        ),
                        evidence_label="attached_drift_monitor",
                        message=(
                            f"Attached object '{object_id}' drift exceeded "
                            "measured TCP/object threshold"
                        ),
                    )
                )
            drift_state = {
                "status": status,
                "action": monitor.action,
                "signal_id": monitor.signal_id,
                "baseline_captured": True,
                "translation_m": translation_m,
                "rotation_rad": rotation_rad,
                "translation_threshold_m": monitor.translation_threshold_m,
                "rotation_threshold_rad": monitor.rotation_threshold_rad,
                "debounce_s": monitor.debounce_s,
                "sample_time_delta_s": sample_time_delta_s,
                "exceeded_since_ns": exceeded_since_ns,
                "condition_raised": debounced,
                "metadata": condition_payload,
            }
            self._set_pose_source_drift_state(key, drift_state)
            self._publish_pose_source_grasp_confirmation(
                source_id=source_id,
                object_id=object_id,
                confirmation_signal_id=monitor.confirmation_signal_id,
                value=status == "ok",
                stale_after_s=source.stale_after_s,
                metadata={
                    **condition_payload,
                    "status": status,
                    "confirmation_policy": "attached_drift_monitor",
                },
            )
            self._publish_pose_source_drift_condition(
                source_id=source_id,
                object_id=object_id,
                signal_id=monitor.signal_id,
                value=bool(debounced),
                stale_after_s=source.stale_after_s,
                metadata=condition_payload,
            )
            if debounced and not already_raised:
                self._record_pose_source_drift_event(
                    "exceeded",
                    details=condition_payload,
                )
            if (
                debounced
                and not already_raised
                and monitor.action == "interrupt"
                and self.cycle_manager is not None
            ):
                message = (
                    f"Attached object '{object_id}' drift exceeded measured "
                    "TCP/object threshold"
                )
                try:
                    self.cycle_manager.raise_runtime_condition_fault(
                        condition_id=monitor.signal_id,
                        message=message,
                        details={
                            "reason_code": "attached_object_drift",
                            **condition_payload,
                        },
                    )
                except AttributeError:
                    logger.warning(
                        "attached object drift exceeded but cycle fault hook is unavailable",
                        extra={
                            "event": "manyforge.pose_source.drift_interrupt_unavailable",
                            "object_id": object_id,
                            "source_id": source_id,
                        },
                    )
                except Exception:
                    logger.exception("attached object drift interrupt failed")

    def pose_source_snapshots(self) -> dict[str, dict[str, Any]]:
        """Return a defensive copy of latest pose-source snapshots."""
        with self._pose_source_lock:
            return copy.deepcopy(self._pose_source_snapshots)

    def clear_pose_source_snapshots(self) -> None:
        """Drop observed source state when the runtime scene changes."""
        self.invalidate_pose_source_applies(reason="clear_pose_source_snapshots")
        with self._pose_source_lock:
            self._pose_source_snapshots.clear()
        with self._pose_source_drift_lock:
            self._pose_source_drift_baselines.clear()
            self._pose_source_drift_states.clear()
            self._pose_source_drift_exceeded_since_ns.clear()
            self._pose_source_drift_raised.clear()

    def invalidate_pose_source_applies(self, *, reason: str = "") -> dict[str, Any]:
        """Invalidate queued/in-flight continuous pose-source scene applies.

        Reset and program load rebuild the backend scene to a known baseline.
        A queued continuous apply captured before that boundary must not stamp
        the rebuilt scene with stale source authority, so workers carry a
        generation and re-check it after they acquire ``scene_edit_lock``.
        """
        with self._pose_source_apply_lock:
            self._pose_source_apply_generation += 1
            generation = self._pose_source_apply_generation
            dropped = sum(
                len(request.get("object_ids") or [request.get("policy")])
                for request in self._pose_source_apply_pending.values()
            )
            self._pose_source_apply_pending.clear()
            self._pose_source_apply_signatures.clear()
            self._increment_pose_source_apply_metric_locked(
                "stale_generation_dropped",
                amount=dropped,
            )
            self._pose_source_apply_last_result = {
                "success": True,
                "policy": "continuous",
                "applied_count": 0,
                "skipped_count": dropped,
                "rejected_count": 0,
                "details": (
                    [
                        {
                            "status": "skipped",
                            "skip_reason": "pose_source_apply_generation_invalidated",
                            "reason": reason,
                        }
                    ]
                    if dropped
                    else []
                ),
                "invalidated_generation": generation,
                "reason": reason,
            }
        return {"generation": generation, "dropped_count": dropped, "reason": reason}

    def _pose_source_apply_generation_value(self) -> int:
        with self._pose_source_apply_lock:
            return self._pose_source_apply_generation

    def _increment_pose_source_apply_metric_locked(
        self,
        metric: str,
        *,
        amount: int = 1,
        source_id: str | None = None,
        object_id: str | None = None,
    ) -> None:
        if amount <= 0:
            return
        self._pose_source_apply_metrics[metric] = (
            int(self._pose_source_apply_metrics.get(metric, 0)) + amount
        )
        if source_id and object_id:
            key = f"{source_id}:{object_id}"
            counters = self._pose_source_apply_object_metrics.setdefault(key, {})
            counters[metric] = int(counters.get(metric, 0)) + amount

    def _record_pose_source_apply_metric(
        self,
        metric: str,
        *,
        amount: int = 1,
        source_id: str | None = None,
        object_id: str | None = None,
    ) -> None:
        with self._pose_source_apply_lock:
            self._increment_pose_source_apply_metric_locked(
                metric,
                amount=amount,
                source_id=source_id,
                object_id=object_id,
            )

    def _record_pose_source_snapshot_metric(
        self,
        source_id: str,
        payload: dict[str, Any],
    ) -> None:
        with self._pose_source_apply_lock:
            self._increment_pose_source_apply_metric_locked("snapshots_received")
            for item in payload.get("objects", []) or []:
                if not isinstance(item, dict):
                    continue
                object_id = str(item.get("object_id") or "")
                if object_id:
                    self._increment_pose_source_apply_metric_locked(
                        "snapshots_received",
                        source_id=source_id,
                        object_id=object_id,
                    )

    @staticmethod
    def _pose_source_scene_request_signatures(
        request: SceneEditRequest,
    ) -> tuple[tuple[Any, ...], tuple[Any, ...]]:
        geometry_signature = (
            str(request.object_id or ""),
            str(request.frame_id or ""),
            tuple(float(value) for value in (request.pose_position_m or [])),
            tuple(float(value) for value in (request.pose_orientation_xyzw or [])),
            str(request.source_id or ""),
            request.source_freshness_budget_ns,
            str(request.scene_admission_role or ""),
            request.scene_admission_lease_max_age_ns,
            request.scene_admission_target_drift_tolerance_m,
            request.scene_admission_target_angular_tolerance_rad,
            request.scene_admission_obstacle_drift_tolerance_m,
            request.scene_admission_obstacle_angular_tolerance_rad,
            bool(request.scene_admission_allow_runtime_dynamic_obstacle),
        )
        full_signature = geometry_signature + (request.source_timestamp_ns,)
        return geometry_signature, full_signature

    def _pose_source_apply_signature_decision(
        self,
        *,
        key: tuple[str, str, str],
        geometry_signature: tuple[Any, ...],
        full_signature: tuple[Any, ...],
        runtime_freshness_required: bool,
    ) -> str:
        with self._pose_source_apply_lock:
            previous = self._pose_source_apply_signatures.get(key)
            if previous is not None and previous.get("full") == full_signature:
                return "duplicate"
            if (
                previous is not None
                and previous.get("geometry") == geometry_signature
                and not runtime_freshness_required
            ):
                self._pose_source_apply_signatures[key] = {
                    "geometry": geometry_signature,
                    "full": full_signature,
                }
                return "unchanged_without_runtime_freshness"
        return "apply"

    def _pose_source_mark_signature_applied(
        self,
        *,
        key: tuple[str, str, str],
        geometry_signature: tuple[Any, ...],
        full_signature: tuple[Any, ...],
    ) -> None:
        with self._pose_source_apply_lock:
            self._pose_source_apply_signatures[key] = {
                "geometry": geometry_signature,
                "full": full_signature,
            }

    def pose_source_apply_status(self) -> dict[str, Any]:
        with self._pose_source_apply_lock:
            return {
                "generation": self._pose_source_apply_generation,
                "running": self._pose_source_apply_running,
                "pending_count": len(self._pose_source_apply_pending),
                "last_result": copy.deepcopy(self._pose_source_apply_last_result),
                "counters": dict(self._pose_source_apply_metrics),
                "objects": {
                    key: dict(value)
                    for key, value in self._pose_source_apply_object_metrics.items()
                },
            }

    def live_scene_mutation_block_reason(self) -> str | None:
        """Return why live scene mutation is unsafe right now, if blocked."""
        manager = self.cycle_manager
        if manager is None:
            return None
        checker = getattr(manager, "live_scene_mutation_block_reason", None)
        if callable(checker):
            try:
                reason = checker()
            except Exception as exc:  # noqa: BLE001 - unreadable guard fails closed
                logger.warning(
                    "live scene mutation guard failed closed: %s",
                    exc,
                    exc_info=True,
                )
                return "scene_mutation_guard_unavailable"
            if reason is None:
                return None
            if isinstance(reason, str):
                return reason if reason else None

        cycle_state = getattr(manager, "state", None)
        cycle_value = str(getattr(cycle_state, "value", "") or "").lower()
        cycle_name = str(getattr(cycle_state, "name", "") or cycle_value).upper()
        if cycle_value in {"running", "stopping", "direct_control"}:
            return f"execution_active:{cycle_name}"
        return "scene_mutation_guard_unavailable"

    @staticmethod
    def _action_span_event_details(action_span: Any | None) -> dict[str, str]:
        if action_span is None:
            return {}
        details: dict[str, str] = {}
        for attr in (
            "action_source",
            "action_span_id",
            "node_instance_id",
            "node_attempt_id",
            "parent_node_attempt_id",
        ):
            value = getattr(action_span, attr, None)
            if value:
                details[attr] = str(value)
        program_run_id = getattr(action_span, "program_run_id", None)
        if program_run_id:
            details["action_span_program_run_id"] = str(program_run_id)
        return details

    def apply_pose_source_updates(
        self,
        *,
        policy: str,
        object_ids: list[str] | None = None,
        require_idle: bool = False,
        reason: str = "",
        action_span: Any | None = None,
        expected_generation: int | None = None,
    ) -> dict[str, Any]:
        """Apply fresh observed poses to the backend planning scene.

        This is the only Composer-owned bridge from pose-source observations to
        kernel scene edits. It is deliberately policy-gated and latest-wins:
        sidecars can provide observations, but the deployment selects which
        object policies may mutate the planning scene.
        """
        if policy not in {
            "when_idle",
            "continuous",
            "continuous_unattached",
            "before_planning",
            "between_nodes",
        }:
            raise ValueError(f"unsupported pose-source apply policy: {policy}")
        if self.kernel is None:
            return {
                "success": False,
                "policy": policy,
                "error": "kernel unavailable",
                "applied_count": 0,
                "skipped_count": 0,
                "rejected_count": 1,
                "details": [],
            }
        cycle_state = self.cycle_manager.state if self.cycle_manager is not None else None
        if require_idle and cycle_state is not None and cycle_state.value != "idle":
            return {
                "success": False,
                "policy": policy,
                "error": f"pose-source apply requires idle cycle state, got {cycle_state.value}",
                "cycle_state": cycle_state.value,
                "applied_count": 0,
                "skipped_count": 0,
                "rejected_count": 1,
                "details": [],
            }
        block_reason = self.live_scene_mutation_block_reason()
        if block_reason == "resumable_stopped_context":
            return {
                "success": True,
                "policy": policy,
                "applied_count": 0,
                "skipped_count": 1,
                "rejected_count": 0,
                "details": [
                    {
                        "status": "skipped",
                        "skip_reason": "resumable_stopped_context",
                        "reason": reason,
                    }
                ],
                "blocked_reason": block_reason,
            }
        if block_reason == "scene_mutation_guard_unavailable":
            return {
                "success": False,
                "policy": policy,
                "error": "live scene mutation guard unavailable",
                "applied_count": 0,
                "skipped_count": 0,
                "rejected_count": 1,
                "details": [
                    {
                        "status": "rejected",
                        "reject_reason": "scene_mutation_guard_unavailable",
                        "reason": reason,
                    }
                ],
                "blocked_reason": block_reason,
            }
        deployment = self.deployment
        if deployment is None or not getattr(deployment, "pose_sources", {}):
            return {
                "success": True,
                "policy": policy,
                "applied_count": 0,
                "skipped_count": 0,
                "rejected_count": 0,
                "details": [],
            }

        requested_ids = {str(object_id) for object_id in object_ids or [] if str(object_id)}
        require_all_bound = policy == "before_planning" and not requested_ids
        snapshots = self.pose_source_snapshots()
        details: list[dict[str, Any]] = []
        applied_count = 0
        skipped_count = 0
        rejected_count = 0
        required_ids: set[str] = set()
        visual_publish_needed = False

        with self.scene_edit_lock:
            if expected_generation is not None:
                current_generation = self._pose_source_apply_generation_value()
                if expected_generation != current_generation:
                    dropped_count = max(1, len(requested_ids))
                    self._record_pose_source_apply_metric(
                        "stale_generation_dropped",
                        amount=dropped_count,
                    )
                    return {
                        "success": True,
                        "policy": policy,
                        "applied_count": 0,
                        "skipped_count": dropped_count,
                        "rejected_count": 0,
                        "details": [
                            {
                                "status": "skipped",
                                "skip_reason": "pose_source_apply_generation_invalidated",
                                "reason": reason,
                                "expected_generation": expected_generation,
                                "current_generation": current_generation,
                            }
                        ],
                        "invalidated_generation": current_generation,
                    }
            for source_id, source in sorted(deployment.pose_sources.items()):
                if not source.enabled:
                    continue
                snapshot = snapshots.get(source_id, {})
                snapshot_objects = {
                    str(item.get("object_id") or ""): item
                    for item in snapshot.get("objects", [])
                    if isinstance(item, dict)
                }
                for object_id, binding in sorted(source.objects.items()):
                    if binding.apply_policy != policy:
                        continue
                    if requested_ids and object_id not in requested_ids:
                        continue
                    if require_all_bound:
                        required_ids.add(object_id)
                    observed = snapshot_objects.get(object_id)
                    detail = {
                        "source_id": source_id,
                        "object_id": object_id,
                        "policy": policy,
                        "reason": reason,
                    }
                    scene_admission = getattr(binding, "scene_admission", None)
                    if scene_admission is None and policy != "observe_only":
                        scene_admission = PoseSourceSceneAdmissionConfig(
                            role="snapshot_obstacle"
                        )
                    if scene_admission is not None:
                        detail["scene_admission_role"] = scene_admission.role
                        if scene_admission.lease_max_age_s is not None:
                            detail["scene_admission_lease_max_age_s"] = (
                                scene_admission.lease_max_age_s
                            )
                        if scene_admission.target_drift_tolerance_m is not None:
                            detail["scene_admission_target_drift_tolerance_m"] = (
                                scene_admission.target_drift_tolerance_m
                            )
                        if scene_admission.target_angular_tolerance_rad is not None:
                            detail[
                                "scene_admission_target_angular_tolerance_rad"
                            ] = scene_admission.target_angular_tolerance_rad
                        if scene_admission.obstacle_drift_tolerance_m is not None:
                            detail["scene_admission_obstacle_drift_tolerance_m"] = (
                                scene_admission.obstacle_drift_tolerance_m
                            )
                        if scene_admission.obstacle_angular_tolerance_rad is not None:
                            detail[
                                "scene_admission_obstacle_angular_tolerance_rad"
                            ] = scene_admission.obstacle_angular_tolerance_rad
                        detail["scene_admission_allow_runtime_dynamic_obstacle"] = (
                            scene_admission.allow_runtime_dynamic_obstacle
                        )
                    if policy == "before_planning":
                        object_state = self.kernel.get_object_state(object_id)
                        if object_state.attached:
                            detail["status"] = "skipped"
                            detail["skip_reason"] = "attached_object"
                            detail["attached"] = True
                            detail["attached_frame_id"] = str(object_state.frame_id or "")
                            skipped_count += 1
                            details.append(detail)
                            continue
                    if not observed:
                        detail["status"] = "skipped"
                        detail["skip_reason"] = "missing_observation"
                        skipped_count += 1
                        details.append(detail)
                        continue
                    if policy == "continuous_unattached":
                        object_state = self.kernel.get_object_state(object_id)
                        if object_state.attached:
                            detail["status"] = "skipped"
                            detail["skip_reason"] = "attached_object"
                            detail["attached"] = True
                            detail["attached_frame_id"] = str(object_state.frame_id or "")
                            skipped_count += 1
                            details.append(detail)
                            continue
                    if observed.get("status") != "fresh":
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "not_fresh"
                        detail["observed_status"] = observed.get("status")
                        rejected_count += 1
                        details.append(detail)
                        continue
                    age_s_raw = observed.get("age_s")
                    if age_s_raw is None:
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "missing_age"
                        rejected_count += 1
                        details.append(detail)
                        continue
                    try:
                        age_s = float(age_s_raw)
                    except (TypeError, ValueError):
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "malformed_age"
                        detail["age_s"] = str(age_s_raw)
                        rejected_count += 1
                        details.append(detail)
                        continue
                    if age_s < 0.0:
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "negative_age"
                        detail["age_s"] = age_s
                        rejected_count += 1
                        details.append(detail)
                        continue
                    if age_s > float(source.stale_after_s):
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "stale"
                        detail["age_s"] = age_s
                        detail["stale_after_s"] = float(source.stale_after_s)
                        rejected_count += 1
                        details.append(detail)
                        continue
                    received_at_ns = int(snapshot.get("received_at_ns", 0) or 0)
                    source_freshness_budget_ns = int(float(source.stale_after_s) * 1_000_000_000)
                    if received_at_ns <= 0 or source_freshness_budget_ns <= 0:
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "invalid_freshness_budget"
                        detail["received_at_ns"] = received_at_ns
                        detail["source_freshness_budget_ns"] = source_freshness_budget_ns
                        rejected_count += 1
                        details.append(detail)
                        continue
                    source_timestamp_ns = received_at_ns - int(age_s * 1_000_000_000)
                    if source_timestamp_ns < 0:
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "invalid_source_timestamp"
                        detail["received_at_ns"] = received_at_ns
                        detail["age_s"] = age_s
                        rejected_count += 1
                        details.append(detail)
                        continue
                    update = observed.get("update")
                    pose = update.get("pose") if isinstance(update, dict) else None
                    if not isinstance(pose, dict):
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "missing_pose"
                        rejected_count += 1
                        details.append(detail)
                        continue
                    position = pose.get("position_xyz") or pose.get("positionXYZ")
                    orientation = pose.get("orientation_xyzw") or pose.get("orientationXYZW")
                    observed_frame_id = pose.get("frame_id") or pose.get("frameId")
                    # The deployment binding owns ManyForge planning-frame semantics.
                    # External providers often report an entity-local/source-native
                    # frame in their payload; using that frame for scene_edit would
                    # make an otherwise fresh observation fail to resolve in the
                    # planning scene. Keep the provider frame as trace metadata and
                    # apply in the configured object/source frame.
                    frame_id = binding.frame_id or source.frame_id or observed_frame_id
                    if (
                        not isinstance(position, list)
                        or len(position) != 3
                        or not isinstance(orientation, list)
                        or len(orientation) != 4
                    ):
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "malformed_pose"
                        rejected_count += 1
                        details.append(detail)
                        continue
                    edit_position, edit_orientation, edit_frame_id = (
                        self._pose_source_scene_edit_pose(
                            object_id=object_id,
                            position=[float(value) for value in position],
                            orientation=[float(value) for value in orientation],
                            frame_id=str(frame_id or "universe"),
                            detail=detail,
                        )
                    )
                    if edit_position is None or edit_orientation is None or edit_frame_id is None:
                        rejected_count += 1
                        details.append(detail)
                        continue
                    # Safe-point applies validate freshness before mutating the
                    # backend scene. After that mutation, the backend owns the
                    # scene state for planning/execution; carrying the source
                    # timestamp forward would make long moves fail as "stale"
                    # even though no continuous source is expected. Continuous
                    # policies keep runtime freshness metadata because the
                    # source remains authoritative while execution is active.
                    runtime_freshness_required = policy in {
                        "continuous",
                        "continuous_unattached",
                    }
                    scene_request = SceneEditRequest(
                        operation="update_pose",
                        object_id=object_id,
                        pose_position_m=edit_position,
                        pose_orientation_xyzw=edit_orientation,
                        frame_id=edit_frame_id,
                        source_id=(
                            source_id
                            if scene_admission is not None
                            or runtime_freshness_required
                            else None
                        ),
                        source_timestamp_ns=(
                            source_timestamp_ns if runtime_freshness_required else None
                        ),
                        source_freshness_budget_ns=(
                            source_freshness_budget_ns
                            if runtime_freshness_required
                            else None
                        ),
                        scene_admission_role=(
                            scene_admission.role
                            if scene_admission is not None
                            else None
                        ),
                        scene_admission_lease_max_age_ns=(
                            int(scene_admission.lease_max_age_s * 1_000_000_000)
                            if scene_admission is not None
                            and scene_admission.lease_max_age_s is not None
                            else None
                        ),
                        scene_admission_target_drift_tolerance_m=(
                            scene_admission.target_drift_tolerance_m
                            if scene_admission is not None
                            else None
                        ),
                        scene_admission_target_angular_tolerance_rad=(
                            scene_admission.target_angular_tolerance_rad
                            if scene_admission is not None
                            else None
                        ),
                        scene_admission_obstacle_drift_tolerance_m=(
                            scene_admission.obstacle_drift_tolerance_m
                            if scene_admission is not None
                            else None
                        ),
                        scene_admission_obstacle_angular_tolerance_rad=(
                            scene_admission.obstacle_angular_tolerance_rad
                            if scene_admission is not None
                            else None
                        ),
                        scene_admission_allow_runtime_dynamic_obstacle=(
                            bool(scene_admission.allow_runtime_dynamic_obstacle)
                            if scene_admission is not None
                            else False
                        ),
                        action_span=action_span,
                    )
                    signature_key = (policy, source_id, object_id)
                    geometry_signature, full_signature = (
                        self._pose_source_scene_request_signatures(scene_request)
                    )
                    signature_decision = self._pose_source_apply_signature_decision(
                        key=signature_key,
                        geometry_signature=geometry_signature,
                        full_signature=full_signature,
                        runtime_freshness_required=runtime_freshness_required,
                    )
                    if signature_decision != "apply":
                        detail["status"] = "skipped"
                        detail["skip_reason"] = (
                            "pose_source_duplicate_no_op"
                            if signature_decision == "duplicate"
                            else "pose_source_unchanged_without_runtime_freshness"
                        )
                        detail["frame_id"] = edit_frame_id
                        if observed_frame_id:
                            detail["observed_frame_id"] = str(observed_frame_id)
                        skipped_count += 1
                        details.append(detail)
                        self._record_pose_source_apply_metric(
                            "no_op_skipped",
                            source_id=source_id,
                            object_id=object_id,
                        )
                        continue
                    result = self.kernel.scene_edit(scene_request)
                    if not result.success:
                        detail["status"] = "rejected"
                        detail["reject_reason"] = "scene_edit_failed"
                        detail["error"] = result.error
                        detail["frame_id"] = edit_frame_id
                        if observed_frame_id:
                            detail["observed_frame_id"] = str(observed_frame_id)
                        rejected_count += 1
                        details.append(detail)
                        continue
                    detail["status"] = "applied"
                    detail["frame_id"] = edit_frame_id
                    update_status = getattr(result, "update_status", None)
                    if update_status:
                        detail["update_status"] = str(update_status)
                    if observed_frame_id:
                        detail["observed_frame_id"] = str(observed_frame_id)
                    if update_status not in {"freshness_only", "no_op"}:
                        visual_update = self.apply_visual_scene_edit_success(
                            scene_request
                        )
                        visual_publish_needed = True
                        if visual_update.get("needs_refresh"):
                            detail["visual_refresh_requested"] = True
                            self.request_visual_scene_objects_refresh(
                                reason=f"pose_source_{policy}"
                            )
                    else:
                        detail["visual_update_skipped"] = str(update_status)
                    self._pose_source_mark_signature_applied(
                        key=signature_key,
                        geometry_signature=geometry_signature,
                        full_signature=full_signature,
                    )
                    if update_status == "freshness_only":
                        self._record_pose_source_apply_metric(
                            "freshness_only_applied",
                            source_id=source_id,
                            object_id=object_id,
                        )
                    elif update_status == "no_op":
                        self._record_pose_source_apply_metric(
                            "no_op_skipped",
                            source_id=source_id,
                            object_id=object_id,
                        )
                    else:
                        self._record_pose_source_apply_metric(
                            "geometry_applied",
                            source_id=source_id,
                            object_id=object_id,
                        )
                    applied_count += 1
                    details.append(detail)

        if visual_publish_needed:
            self.request_visual_state_publish(reason=f"pose_source_{policy}")

        if not require_all_bound:
            required_ids = set(requested_ids)
        required_missing = [
            object_id
            for object_id in required_ids
            if not any(
                detail["object_id"] == object_id
                and (
                        detail["status"] == "applied"
                        or (
                            policy in {"continuous_unattached", "before_planning"}
                            and detail["status"] == "skipped"
                            and detail.get("skip_reason") == "attached_object"
                        )
                        or (
                            policy in {"continuous", "continuous_unattached"}
                            and detail["status"] == "skipped"
                            and detail.get("skip_reason")
                            in {
                                "pose_source_duplicate_no_op",
                                "pose_source_unchanged_without_runtime_freshness",
                            }
                        )
                    )
                for detail in details
            )
            and any(
                binding.object_id == object_id and binding.apply_policy == policy
                for source in deployment.pose_sources.values()
                for binding in source.objects.values()
            )
        ]
        success = rejected_count == 0 and not required_missing
        error = None
        if required_missing:
            error = f"no fresh pose-source update for: {', '.join(sorted(required_missing))}"
        elif rejected_count:
            error = "one or more pose-source updates were rejected"
        if rejected_count:
            self._record_pose_source_apply_metric("errors", amount=rejected_count)

        event_details = {
            "policy": policy,
            "reason": reason,
            "object_ids": sorted(requested_ids),
            "required_object_ids": sorted(required_ids),
            "require_all_bound": require_all_bound,
            "applied_count": applied_count,
            "skipped_count": skipped_count,
            "rejected_count": rejected_count,
            "required_missing": sorted(required_missing),
            "success": success,
            "details": copy.deepcopy(details),
        }
        event_details.update(self._action_span_event_details(action_span))
        routine_continuous_heartbeat = (
            policy in {"continuous", "continuous_unattached"}
            and success
            and rejected_count == 0
            and bool(details)
            and all(
                (
                    detail.get("status") == "skipped"
                    and detail.get("skip_reason")
                    in {
                        "pose_source_duplicate_no_op",
                        "pose_source_unchanged_without_runtime_freshness",
                    }
                )
                or (
                    detail.get("status") == "applied"
                    and detail.get("update_status") in {"freshness_only", "no_op"}
                )
                for detail in details
            )
        )
        if not routine_continuous_heartbeat:
            try:
                self.event_recorder.record_pose_source_event(
                    "applied" if success else "apply_failed",
                    details=event_details,
                )
            except Exception:
                logger.debug("pose-source event recording failed", exc_info=True)

        return {
            "success": success,
            "policy": policy,
            "error": error,
            "applied_count": applied_count,
            "skipped_count": skipped_count,
            "rejected_count": rejected_count,
            "details": details,
        }

    def enqueue_pose_source_apply(
        self,
        *,
        policy: str,
        object_ids: list[str] | None = None,
        require_idle: bool = False,
        reason: str = "",
    ) -> dict[str, Any]:
        """Queue a latest-wins continuous pose-source scene apply.

        Continuous pose-source snapshots can arrive while trajectory validation
        owns the planner scene. Applying inside the HTTP request path makes
        provider requests pile up behind the same native scene lock and can
        starve viewer polling. This worker keeps only the newest apply request
        and runs the existing audited apply path on one background thread.
        """
        if policy not in {"continuous", "continuous_unattached"}:
            raise ValueError(
                "enqueue_pose_source_apply is only for continuous pose-source policies"
            )
        request = {
            "policy": policy,
            "object_ids": list(object_ids or []),
            "require_idle": bool(require_idle),
            "reason": reason,
        }
        with self._pose_source_apply_lock:
            request["expected_generation"] = self._pose_source_apply_generation
            coalesced = policy in self._pose_source_apply_pending
            if coalesced:
                existing = self._pose_source_apply_pending[policy]
                existing_ids = {
                    str(object_id)
                    for object_id in existing.get("object_ids", [])
                    if object_id
                }
                new_ids = {
                    str(object_id)
                    for object_id in request.get("object_ids", [])
                    if object_id
                }
                existing["object_ids"] = sorted(existing_ids | new_ids)
                existing["require_idle"] = bool(
                    existing.get("require_idle") or request["require_idle"]
                )
                existing["reason"] = request["reason"] or existing.get("reason", "")
                existing["expected_generation"] = self._pose_source_apply_generation
                self._increment_pose_source_apply_metric_locked(
                    "apply_coalesced",
                    amount=max(1, len(new_ids)),
                )
            else:
                self._pose_source_apply_pending[policy] = request
            started_worker = False
            if not self._pose_source_apply_running:
                self._pose_source_apply_running = True
                threading.Thread(
                    target=self._pose_source_apply_worker,
                    name="manyforge_pose_source_apply",
                    daemon=True,
                ).start()
                started_worker = True
            last_result = copy.deepcopy(self._pose_source_apply_last_result)
        return {
            "success": True,
            "policy": policy,
            "queued": True,
            "started_worker": started_worker,
            "coalesced_pending": coalesced,
            "object_ids": list(object_ids or []),
            "last_result": last_result,
        }

    def _pose_source_apply_worker(self) -> None:
        while True:
            with self._pose_source_apply_lock:
                if not self._pose_source_apply_pending:
                    self._pose_source_apply_running = False
                    return
                policy = sorted(self._pose_source_apply_pending)[0]
                request = self._pose_source_apply_pending.pop(policy)
            started = time.monotonic()
            try:
                result = self.apply_pose_source_updates(**request)
            except Exception as exc:  # pragma: no cover - defensive worker guard
                result = {
                    "success": False,
                    "policy": request.get("policy"),
                    "error": str(exc),
                    "applied_count": 0,
                    "skipped_count": 0,
                    "rejected_count": 1,
                    "details": [],
                }
            duration_ms = (time.monotonic() - started) * 1000.0
            result["duration_ms"] = round(duration_ms, 1)
            result["queued_worker"] = True
            with self._pose_source_apply_lock:
                self._pose_source_apply_last_result = copy.deepcopy(result)
            if duration_ms >= 100.0:
                _log_throttle.warning(
                    logger,
                    "pose_source.apply_slow",
                    "pose-source apply worker slow: policy=%s objects=%s "
                    "duration_ms=%.1f success=%s",
                    request.get("policy"),
                    request.get("object_ids"),
                    duration_ms,
                    result.get("success", False),
                    extra={"event": "manyforge.pose_source.apply_slow"},
                )
            if not result.get("success", False):
                # Continuous-policy feeds retry at up to max_update_hz; a
                # persistently failing source must not flood the log.
                _log_throttle.warning(
                    logger,
                    "pose_source.apply_failed",
                    "pose-source apply worker failed: policy=%s objects=%s error=%s",
                    request.get("policy"),
                    request.get("object_ids"),
                    result.get("error"),
                    extra={"event": "manyforge.pose_source.apply_failed"},
                )

    def apply_pose_sources_before_planning(
        self,
        *,
        object_ids: list[str] | None = None,
        reason: str = "",
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        """Apply `before_planning` source updates at the BT latch point."""
        del object_ids
        return self.apply_pose_source_updates(
            policy="before_planning",
            object_ids=[],
            require_idle=False,
            reason=reason or "before_planning",
            action_span=action_span,
        )

    def apply_pose_sources_between_nodes(
        self,
        *,
        object_ids: list[str] | None = None,
        reason: str = "",
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        """Apply `between_nodes` source updates at a BT safe point."""
        return self.apply_pose_source_updates(
            policy="between_nodes",
            object_ids=object_ids,
            require_idle=False,
            reason=reason or "between_nodes",
            action_span=action_span,
        )

    def reset_simulation_for_behavior_node(
        self,
        *,
        reason: str = "",
        source: str = "behavior_node",
        allow_running: bool = True,
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        """BT callback for the ResetSimulation node."""
        return self.simulation_control_for_behavior_node(
            operation="reset",
            reason=reason or "reset_simulation_node",
            source=source,
            allow_running=allow_running,
            action_span=action_span,
        )

    def simulation_control_for_behavior_node(
        self,
        *,
        operation: str,
        reason: str = "",
        source: str = "behavior_node",
        allow_running: bool = True,
        **kwargs: Any,
    ) -> dict[str, Any]:
        """BT callback for simulator-control nodes."""
        action_span = kwargs.get("action_span")
        if operation == "reset":
            return self.reset_simulation(
                reason=reason or "reset_simulation_node",
                source=source,
                allow_running=allow_running,
                action_span=action_span,
            )
        if operation == "set_state":
            return self.set_simulation_state(
                state=kwargs.get("state"),
                reason=reason or "set_simulation_state_node",
                source=source,
                allow_running=allow_running,
                action_span=action_span,
            )
        if operation == "step":
            return self.step_simulation(
                steps=kwargs.get("steps", 1),
                reason=reason or "step_simulation_node",
                source=source,
                allow_running=allow_running,
                action_span=action_span,
            )
        if operation == "set_entity_state":
            return self.set_simulation_entity_state(
                entity=kwargs.get("entity", ""),
                pose=kwargs.get("pose", {}),
                reason=reason or "set_entity_state_node",
                source=source,
                allow_running=allow_running,
                action_span=action_span,
            )
        raise RuntimeError(f"Unsupported simulation control operation: {operation}")

    def set_simulation_state(
        self,
        *,
        state: Any,
        reason: str = "",
        source: str = "api",
        allow_running: bool = False,
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        target_state = self._normalize_simulation_state_target(state)
        required_capability = (
            "simulator.control.play"
            if target_state == "playing"
            else "simulator.control.pause"
        )
        return self._run_simulation_control_operation(
            operation="set_state",
            required_capability=required_capability,
            reason=reason,
            source=source,
            allow_running=allow_running,
            payload={"state": target_state},
            action_span=action_span,
        )

    def step_simulation(
        self,
        *,
        steps: Any = 1,
        reason: str = "",
        source: str = "api",
        allow_running: bool = False,
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        try:
            step_count = int(steps)
        except (TypeError, ValueError) as exc:
            raise RuntimeError("simulation step count must be an integer") from exc
        if step_count < 1:
            raise RuntimeError("simulation step count must be >= 1")
        return self._run_simulation_control_operation(
            operation="step",
            required_capability="simulator.control.step",
            reason=reason,
            source=source,
            allow_running=allow_running,
            payload={"steps": step_count},
            action_span=action_span,
        )

    def set_simulation_entity_state(
        self,
        *,
        entity: Any,
        pose: Any,
        reason: str = "",
        source: str = "api",
        allow_running: bool = False,
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        entity_name = str(entity or "").strip()
        if not entity_name:
            raise RuntimeError("simulation entity is required")
        if not isinstance(pose, dict):
            raise RuntimeError("simulation entity pose must be a mapping")
        return self._run_simulation_control_operation(
            operation="set_entity_state",
            required_capability="simulator.entity_state.set",
            reason=reason,
            source=source,
            allow_running=allow_running,
            payload={"entity": entity_name, "pose": dict(pose)},
            action_span=action_span,
        )

    def reset_simulation(
        self,
        *,
        reason: str = "",
        source: str = "api",
        allow_running: bool = False,
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        """Reset simulator state through the deployment-declared control surface."""
        control_id = uuid.uuid4().hex[:12]
        control, execution_config = self._simulation_reset_contract()
        scope = control.reset.default_scope
        close_command_gate = control.reset.close_command_gate
        resume_play_after_reset = control.reset.resume_play_after_reset
        command_gate_topic = self._simulation_command_gate_topic(execution_config)
        wait_ready_s = control.reset.wait_ready_s
        cycle_state = (
            self.cycle_manager.state.value if self.cycle_manager is not None else "none"
        )
        started = time.monotonic()
        base_details = {
            "operation": "reset",
            "control_id": control_id,
            "source": source,
            "reason": reason,
            "scope": scope,
            "required_capability": "simulator.control.reset",
            "cycle_state": cycle_state,
            "allow_running": bool(allow_running),
            "resume_play_after_reset": bool(resume_play_after_reset),
        }
        base_details.update(self._action_span_event_details(action_span))
        self.event_recorder.record_simulation_control_event(
            "requested",
            details=base_details,
            authority_source=source,
            trigger_node_instance=reason if source == "behavior_node" else None,
            execution_mode="program" if source == "behavior_node" else None,
        )

        with self._simulation_control_lock:
            try:
                self._validate_simulation_reset_cycle_state(
                    allow_running=allow_running,
                )
                if close_command_gate and not command_gate_topic:
                    raise RuntimeError(
                        "simulation reset requires close_command_gate=true, but the "
                        "execution adapter does not declare command_gate_topic"
                    )
                self.clear_pose_source_snapshots()
                result = self._run_ros_simulation_reset(
                    control_id=control_id,
                    scope=scope,
                    service_prefix=control.service_prefix,
                    command_gate_topic=command_gate_topic,
                    close_command_gate=close_command_gate,
                    resume_play_after_reset=resume_play_after_reset,
                    play_capability_declared=control.capabilities.play,
                    service_timeout_s=wait_ready_s,
                    wait_ready_s=wait_ready_s,
                    execution_config=execution_config,
                    source=source,
                    reason=reason,
                )
                if not result.get("success", False):
                    raise RuntimeError(str(result.get("error") or "simulation reset failed"))
                self._refresh_runtime_after_simulation_reset()
                duration_s = time.monotonic() - started
                completed_details = {
                    **base_details,
                    "duration_s": duration_s,
                    "service_name": result.get("service_name"),
                    "close_command_gate": close_command_gate,
                    "command_gate_topic": command_gate_topic,
                    "command_gate_reopened": result.get("command_gate_reopened"),
                    "resume_play_after_reset": result.get(
                        "resume_play_after_reset", False
                    ),
                    "resume_play_result": result.get("resume_play_result"),
                    "readiness": result.get("readiness"),
                }
                self.event_recorder.record_simulation_control_event(
                    "completed",
                    details=completed_details,
                    authority_source=source,
                    trigger_node_instance=reason if source == "behavior_node" else None,
                    execution_mode="program" if source == "behavior_node" else None,
                )
                return {
                    **result,
                    "success": True,
                    "operation": "reset",
                    "control_id": control_id,
                    "scope": scope,
                    "duration_s": duration_s,
                    "resume_play_after_reset": result.get(
                        "resume_play_after_reset", False
                    ),
                    "resume_play_result": result.get("resume_play_result"),
                }
            except Exception as exc:
                duration_s = time.monotonic() - started
                self.event_recorder.record_simulation_control_event(
                    "failed",
                    details={
                        **base_details,
                        "duration_s": duration_s,
                        "error": str(exc),
                        "close_command_gate": close_command_gate,
                        "command_gate_topic": command_gate_topic,
                    },
                    authority_source=source,
                    trigger_node_instance=reason if source == "behavior_node" else None,
                    execution_mode="program" if source == "behavior_node" else None,
                )
                if isinstance(exc, RuntimeError):
                    raise
                raise RuntimeError(str(exc)) from exc

    def _run_simulation_control_operation(
        self,
        *,
        operation: str,
        required_capability: str,
        reason: str,
        source: str,
        allow_running: bool,
        payload: dict[str, Any],
        action_span: Any | None = None,
    ) -> dict[str, Any]:
        control_id = uuid.uuid4().hex[:12]
        control, _execution_config = self._simulation_control_contract(
            required_capability=required_capability,
        )
        service_timeout_s = control.reset.wait_ready_s
        cycle_state = (
            self.cycle_manager.state.value if self.cycle_manager is not None else "none"
        )
        started = time.monotonic()
        base_details = {
            "operation": operation,
            "control_id": control_id,
            "source": source,
            "reason": reason,
            "required_capability": required_capability,
            "cycle_state": cycle_state,
            "allow_running": bool(allow_running),
            **self._simulation_control_audit_payload(payload),
        }
        base_details.update(self._action_span_event_details(action_span))
        self.event_recorder.record_simulation_control_event(
            "requested",
            details=base_details,
            authority_source=source,
            trigger_node_instance=reason if source == "behavior_node" else None,
            execution_mode="program" if source == "behavior_node" else None,
        )

        with self._simulation_control_lock:
            try:
                self._validate_simulation_control_cycle_state(
                    operation=operation,
                    allow_running=allow_running,
                )
                result = self._run_ros_simulation_control_operation(
                    operation=operation,
                    control_id=control_id,
                    service_prefix=control.service_prefix,
                    service_timeout_s=service_timeout_s,
                    source=source,
                    reason=reason,
                    payload=payload,
                )
                if not result.get("success", False):
                    raise RuntimeError(str(result.get("error") or "simulation control failed"))
                duration_s = time.monotonic() - started
                self.event_recorder.record_simulation_control_event(
                    "completed",
                    details={
                        **base_details,
                        "duration_s": duration_s,
                        "service_name": result.get("service_name"),
                    },
                    authority_source=source,
                    trigger_node_instance=reason if source == "behavior_node" else None,
                    execution_mode="program" if source == "behavior_node" else None,
                )
                return {
                    **result,
                    "success": True,
                    "operation": operation,
                    "control_id": control_id,
                    "duration_s": duration_s,
                }
            except Exception as exc:
                duration_s = time.monotonic() - started
                self.event_recorder.record_simulation_control_event(
                    "failed",
                    details={
                        **base_details,
                        "duration_s": duration_s,
                        "error": str(exc),
                    },
                    authority_source=source,
                    trigger_node_instance=reason if source == "behavior_node" else None,
                    execution_mode="program" if source == "behavior_node" else None,
                )
                if isinstance(exc, RuntimeError):
                    raise
                raise RuntimeError(str(exc)) from exc

    def _simulation_reset_contract(self):
        control, execution_config = self._simulation_control_contract(
            required_capability="simulator.control.reset",
        )
        if control.reset.default_scope != "state":
            raise RuntimeError(
                "Only simulation_control.reset.default_scope=state is supported"
            )
        return control, execution_config

    def _simulation_control_contract(self, *, required_capability: str):
        deployment = self.deployment
        if deployment is None:
            raise RuntimeError("No deployment loaded")
        control = deployment.simulation_control
        if control is None:
            raise RuntimeError("simulation_control is not configured for this deployment")
        if control.kind != "ros2_simulation_interfaces":
            raise RuntimeError(
                f"Unsupported simulation_control.kind: {control.kind}"
            )
        if required_capability not in control.capability_keys():
            raise RuntimeError(
                f"simulation_control does not declare {required_capability}"
            )
        execution = deployment.adapters.get("execution")
        execution_config = dict(execution.config or {}) if execution is not None else {}
        return control, execution_config

    def _simulation_command_gate_topic(self, execution_config: dict[str, Any]) -> str:
        topic = (
            execution_config.get("command_gate_topic")
            or execution_config.get("command_gate_enable_topic")
            or ""
        )
        return str(topic).strip()

    def _validate_simulation_reset_cycle_state(self, *, allow_running: bool) -> None:
        self._validate_simulation_control_cycle_state(
            operation="reset",
            allow_running=allow_running,
        )

    def _validate_simulation_control_cycle_state(
        self,
        *,
        operation: str,
        allow_running: bool,
    ) -> None:
        manager = self.cycle_manager
        if manager is None:
            return
        block_reason = self.live_scene_mutation_block_reason()
        if block_reason == "resumable_stopped_context":
            raise RuntimeError(
                "Cannot reset simulation while a stopped cycle still has a "
                "resumable behavior-tree context; reset the cycle or resume it "
                "before resetting the simulator"
            )
        if block_reason == "scene_mutation_guard_unavailable":
            raise RuntimeError(
                f"Cannot {operation} simulation: live scene mutation guard is "
                "unavailable"
            )
        cycle_state = manager.state.value
        if cycle_state == "running" and not allow_running:
            raise RuntimeError(
                f"Cannot {operation} simulation during an active program cycle; "
                "author an explicit simulator-control node or stop/reset the cycle first"
            )
        if cycle_state == "direct_control":
            if operation != "reset":
                raise RuntimeError(
                    f"Cannot {operation} simulation during direct-control intervention; "
                    "stop direct control first"
                )
            manager.abort_intervention()
            deadline = time.monotonic() + 2.0
            while manager.state.value == "direct_control" and time.monotonic() < deadline:
                time.sleep(0.01)
            if manager.state.value == "direct_control":
                raise RuntimeError(
                    "Cannot reset simulation: direct-control intervention did not abort"
                )

    def _run_ros_simulation_control_operation(
        self,
        *,
        operation: str,
        control_id: str,
        service_prefix: str,
        service_timeout_s: float,
        source: str,
        reason: str,
        payload: dict[str, Any],
    ) -> dict[str, Any]:
        from manyforge_ros.simulation_control import SimulationControlClient

        import rclpy

        initialized_here = False
        if not rclpy.ok():
            rclpy.init(args=None)
            initialized_here = True
        node = rclpy.create_node("manyforge_composer_simulation_control")
        try:
            client = SimulationControlClient(
                node,
                service_prefix=service_prefix,
                service_timeout_s=service_timeout_s,
            )
            self.event_recorder.record_simulation_control_event(
                "started",
                details={
                    "operation": operation,
                    "control_id": control_id,
                    "source": source,
                    "reason": reason,
                    "service_prefix": service_prefix,
                    "service_timeout_s": service_timeout_s,
                    **self._simulation_control_audit_payload(payload),
                },
                authority_source=source,
                trigger_node_instance=reason if source == "behavior_node" else None,
                execution_mode="program" if source == "behavior_node" else None,
            )
            if operation == "set_state":
                return client.set_simulation_state(
                    state=str(payload.get("state") or "")
                ).to_dict()
            if operation == "step":
                return client.step(steps=int(payload.get("steps", 1))).to_dict()
            if operation == "set_entity_state":
                return client.set_entity_state(
                    entity=str(payload.get("entity") or ""),
                    pose=dict(payload.get("pose") or {}),
                ).to_dict()
            raise RuntimeError(f"Unsupported simulation control operation: {operation}")
        finally:
            node.destroy_node()
            if initialized_here:
                rclpy.shutdown()

    @staticmethod
    def _normalize_simulation_state_target(state: Any) -> str:
        value = str(state or "").strip().lower()
        aliases = {
            "play": "playing",
            "playing": "playing",
            "pause": "paused",
            "paused": "paused",
        }
        target = aliases.get(value)
        if target is None:
            raise RuntimeError(
                "simulation state must be one of: playing, paused"
            )
        return target

    @staticmethod
    def _simulation_control_audit_payload(payload: dict[str, Any]) -> dict[str, Any]:
        audit: dict[str, Any] = {}
        if "state" in payload:
            audit["state"] = payload.get("state")
        if "steps" in payload:
            audit["steps"] = payload.get("steps")
        if "entity" in payload:
            audit["entity"] = payload.get("entity")
        pose = payload.get("pose")
        if isinstance(pose, dict):
            audit["frame_id"] = pose.get("frame_id")
            audit["position_m"] = pose.get("position_m")
        return audit

    def _run_ros_simulation_reset(
        self,
        *,
        control_id: str,
        scope: str,
        service_prefix: str,
        command_gate_topic: str,
        close_command_gate: bool,
        resume_play_after_reset: bool,
        play_capability_declared: bool,
        service_timeout_s: float,
        wait_ready_s: float,
        execution_config: dict[str, Any],
        source: str,
        reason: str,
    ) -> dict[str, Any]:
        from manyforge_ros.simulation_control import SimulationControlClient

        import rclpy

        initialized_here = False
        if not rclpy.ok():
            rclpy.init(args=None)
            initialized_here = True
        node = rclpy.create_node("manyforge_composer_simulation_reset")
        command_gate_closed = False
        command_gate_reopened = False
        try:
            client = SimulationControlClient(
                node,
                service_prefix=service_prefix,
                service_timeout_s=service_timeout_s,
            )
            if close_command_gate:
                # Single gate authority: the execution backend owns the gate
                # topic (sole transient-local publisher). Publishing from a
                # second node here made the latched value reader-dependent
                # and could strand the gate closed across process restarts.
                if not self._kernel.set_command_gate(False):
                    raise RuntimeError(
                        "simulation reset requires close_command_gate=true, "
                        "but the execution backend exposes no command-gate "
                        "surface"
                    )
                command_gate_closed = True
            self.event_recorder.record_simulation_control_event(
                "started",
                details={
                    "operation": "reset",
                    "control_id": control_id,
                    "source": source,
                    "reason": reason,
                    "scope": scope,
                    "service_prefix": service_prefix,
                    "close_command_gate": close_command_gate,
                    "command_gate_topic": command_gate_topic,
                    "resume_play_after_reset": resume_play_after_reset,
                    "service_timeout_s": service_timeout_s,
                    "wait_ready_s": wait_ready_s,
                },
                authority_source=source,
                trigger_node_instance=reason if source == "behavior_node" else None,
                execution_mode="program" if source == "behavior_node" else None,
            )
            reset_result = client.reset(scope=scope).to_dict()
            if not reset_result.get("success", False):
                return {
                    **reset_result,
                    "success": False,
                    "command_gate_closed": command_gate_closed,
                    "command_gate_reopened": False,
                    "resume_play_after_reset": False,
                    "resume_play_result": None,
                }
            readiness = self._wait_for_simulation_reset_readiness(
                wait_ready_s=wait_ready_s,
                execution_config=execution_config,
            )
            if not readiness.get("ready", False):
                return {
                    **reset_result,
                    "success": False,
                    "error": "simulation reset readiness check failed",
                    "readiness": readiness,
                    "command_gate_closed": command_gate_closed,
                    "command_gate_reopened": False,
                    "resume_play_after_reset": False,
                    "resume_play_result": None,
                }
            resume_play_result: dict[str, Any] | None = None
            should_resume_play = (
                resume_play_after_reset
                and self._is_commanded_sim_execution_config(execution_config)
            )
            if should_resume_play:
                if not play_capability_declared:
                    return {
                        **reset_result,
                        "success": False,
                        "error": (
                            "simulation_control.reset.resume_play_after_reset=true "
                            "requires simulation_control.capabilities.play=true "
                            "for commanded-sim deployments"
                        ),
                        "readiness": readiness,
                        "command_gate_closed": command_gate_closed,
                        "command_gate_reopened": False,
                        "resume_play_after_reset": True,
                        "resume_play_result": None,
                    }
                resume_play_result = client.set_simulation_state(
                    state="playing"
                ).to_dict()
                if not resume_play_result.get("success", False):
                    return {
                        **reset_result,
                        "success": False,
                        "error": "simulation reset play-state resume failed",
                        "readiness": readiness,
                        "command_gate_closed": command_gate_closed,
                        "command_gate_reopened": False,
                        "resume_play_after_reset": True,
                        "resume_play_result": resume_play_result,
                    }
            if close_command_gate:
                command_gate_reopened = self._kernel.set_command_gate(True)
            return {
                **reset_result,
                "success": True,
                "readiness": readiness,
                "command_gate_closed": command_gate_closed,
                "command_gate_reopened": command_gate_reopened,
                "resume_play_after_reset": should_resume_play,
                "resume_play_result": resume_play_result,
            }
        finally:
            node.destroy_node()
            if initialized_here:
                rclpy.shutdown()

    @staticmethod
    def _is_commanded_sim_execution_config(execution_config: dict[str, Any]) -> bool:
        authority_profile = (
            execution_config.get("execution_state_authority_profile")
            or execution_config.get("execution_state_source_mode")
            or ""
        )
        return authority_profile == "commanded_sim"

    def _wait_for_simulation_reset_readiness(
        self,
        *,
        wait_ready_s: float,
        execution_config: dict[str, Any],
    ) -> dict[str, Any]:
        pose_source_result: dict[str, Any] | None = None
        if self.deployment is not None and self.deployment.pose_sources:
            pose_source_result = self.apply_pose_sources_between_nodes(
                reason="simulation_reset",
            )
            if not pose_source_result.get("success", False):
                return {
                    "ready": False,
                    "status": "pose_source_failed",
                    "poseSourceApply": pose_source_result,
                }

        if not self._is_commanded_sim_execution_config(execution_config):
            return {
                "ready": True,
                "status": "not_required",
                "poseSourceApply": pose_source_result,
            }

        deadline = time.monotonic() + wait_ready_s
        last_status: dict[str, Any] | None = None
        while time.monotonic() < deadline:
            statuses = self.external_runtime_statuses(max_age_s=6.0)
            last_status = statuses.get("isaac_demo_supervisor")
            if (
                isinstance(last_status, dict)
                and last_status.get("canExecute") is True
                and str(last_status.get("readiness", "")) == "ready"
            ):
                return {
                    "ready": True,
                    "status": "ready",
                    "provider": "isaac_demo_supervisor",
                    "poseSourceApply": pose_source_result,
                    "providerStatus": dict(last_status),
                }
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
        return {
            "ready": False,
            "status": "timeout",
            "provider": "isaac_demo_supervisor",
            "poseSourceApply": pose_source_result,
            "providerStatus": dict(last_status or {}),
        }

    def _refresh_runtime_after_simulation_reset(self) -> None:
        self.clear_pose_source_snapshots()
        self.clear_visual_state_snapshot()
        self.clear_visual_scene_objects_snapshot()
        refresh_result = self.refresh_visual_scene_objects_sync(
            reason="simulation_reset"
        )
        if not refresh_result.get("success", False):
            logger.warning(
                "simulation reset visual scene refresh failed: %s",
                refresh_result.get("error"),
            )
        self.request_visual_state_publish(reason="simulation_reset")

    def external_runtime_statuses(self, *, max_age_s: float = 5.0) -> dict[str, dict[str, Any]]:
        """Return fresh external runtime status contributions keyed by provider."""
        now_ns = time.monotonic_ns()
        max_age_ns = int(max_age_s * 1e9)
        fresh: dict[str, dict[str, Any]] = {}
        stale: list[str] = []
        with self._external_runtime_status_lock:
            for provider, record in self._external_runtime_statuses.items():
                received_at_ns = int(record.get("receivedAtNs", 0) or 0)
                if received_at_ns and now_ns - received_at_ns <= max_age_ns:
                    fresh[provider] = dict(record)
                else:
                    stale.append(provider)
            for provider in stale:
                self._external_runtime_statuses.pop(provider, None)
        return fresh

    def reset_backend_authoritative_sim_if_needed(self, *, reason: str) -> None:
        """Push simulator-authoritative profiles back to their configured initial state.

        Real controller-backed deployments keep the physical/controller state as
        truth and therefore must not move on software reset.  The Isaac
        commanded-sim preview is explicitly different: ManyForge owns the
        simulator command truth, so program load/reset should command the
        configured initial joint vector through the execution backend.
        """
        deployment = self.deployment
        if deployment is None:
            return
        execution = deployment.adapters.get("execution")
        if execution is None:
            return
        config = dict(execution.config or {})
        authority_profile = (
            config.get("execution_state_authority_profile")
            or config.get("execution_state_source_mode")
        )
        if authority_profile != "commanded_sim":
            return
        if config.get("reset_to_initial_on_load", True) is False and reason == "program_load":
            return
        result = self._kernel.reset()
        if not result.accepted:
            raise RuntimeError(
                "commanded_sim reset to initial robot state failed: "
                f"{result.error or 'unknown error'}"
            )
