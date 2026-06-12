"""CycleManager — runtime lifecycle controller for continuous robot programs."""

from __future__ import annotations

import logging
import json
import sys
import threading
import time
import traceback
import uuid
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Callable, Optional

import py_trees

logger = logging.getLogger(__name__)

from collections import deque

from manyforge_behavior.blackboard_bindings import key_ref_id
from manyforge_behavior.blackboard_namespace import write_namespace_bundle
from manyforge_behavior.signals import RuntimeSignalRegistry, SignalProvider, SignalUpdate

from manyforge_behavior.kernel_client import (
    ExecutionSnapshot,
    ExecutionState,
    ExecutionStatus,
    KernelClient,
    ObjectState,
    SceneEditRequest,
    SceneObjectSnapshot,
)
from manyforge_behavior.runner import OrchestratorRunner


class CycleState(Enum):
    """Lifecycle states for the CycleManager."""
    IDLE = "idle"
    RUNNING = "running"
    STOPPED = "stopped"
    DIRECT_CONTROL = "direct_control"


class GrantType:
    """Authority grant types for intervention sessions.

    These are checked grants, not advisory labels.  The CycleManager enforces
    the rules described in OP-4.
    """
    HUMAN_APPROVED = "human_approved"
    POLICY_GRANTED = "policy_granted"
    # Reserved — does NOT authorise IDLE -> DIRECT_CONTROL in the baseline
    # industrial product.  Only usable for STOPPED -> DIRECT_CONTROL when a
    # deployment-level policy explicitly allows it.
    DEPLOYMENT_GRANTED = "deployment_granted"

    ALL = {HUMAN_APPROVED, POLICY_GRANTED, DEPLOYMENT_GRANTED}
    ACTIVE = {HUMAN_APPROVED, POLICY_GRANTED}  # baseline product


class RecoveryTrigger(Enum):
    """Closed enum of recovery trigger reasons.

    Canonical names are spec'd at `spec/460` §6.2.2:

    - ``planning_failed``
    - ``validation_failed``
    - ``execute_failed``
    - ``execute_timeout``
    - ``scene_mismatch``
    - ``precondition_failed``
    - ``postcondition_failed``

    Two legacy values (``timeout``, ``node_failure``) are kept as
    deprecated aliases for migration. They MUST NOT be used by new
    code and will be removed once all in-tree call sites and traces
    have been migrated. See ``open-points.md`` for the removal target.
    """
    PLANNING_FAILED = "planning_failed"
    VALIDATION_FAILED = "validation_failed"
    EXECUTE_FAILED = "execute_failed"
    EXECUTE_TIMEOUT = "execute_timeout"
    SCENE_MISMATCH = "scene_mismatch"
    PRECONDITION_FAILED = "precondition_failed"
    POSTCONDITION_FAILED = "postcondition_failed"

    # --- Deprecated legacy aliases ---
    # Kept as distinct enum members rather than aliasing canonical
    # values so existing serialised traces (which carry the legacy
    # string) continue to round-trip. Code SHOULD prefer the canonical
    # values above.
    TIMEOUT = "timeout"
    NODE_FAILURE = "node_failure"


RECOVERY_TRIGGER_CANONICAL_VALUES = frozenset({
    "planning_failed",
    "validation_failed",
    "execute_failed",
    "execute_timeout",
    "scene_mismatch",
    "precondition_failed",
    "postcondition_failed",
})

# Backend fault taxonomy (audit Phase 6A): the table lives in the
# dependency-light `fault_taxonomy` module so external-surface consumers
# (6H diagnostics bridge, 6I medkit adapter) can import it without pulling
# py_trees. Re-exported here for backward compatibility.
from manyforge_behavior.fault_taxonomy import (  # noqa: F401  (re-export)
    BackendFaultClass,
    ROS2_CONTROL_BACKEND_FAULT_CLASSES,
)

_ROS2_CONTROL_BACKEND_REASON_CODES = frozenset(ROS2_CONTROL_BACKEND_FAULT_CLASSES)

_GENERIC_BACKEND_ERROR_CODES = frozenset({
    None,
    "execution_aborted",
    "execution_rejected",
    "invalid_request",
    "stop_failed",
})


def canonical_recovery_trigger_values() -> set[str]:
    """Return live-authoring recovery trigger values.

    Deprecated aliases remain parseable for persisted traces, but live
    model/API allowlists must expose only the canonical spec/460 taxonomy.
    """

    return set(RECOVERY_TRIGGER_CANONICAL_VALUES)


def preferred_structured_error_code(
    error_code: Optional[str],
    error_details: Optional[dict[str, Any]],
) -> Optional[str]:
    """Prefer exact backend reason tokens over generic transport codes.

    ROS2-control backend faults keep the top-level ErrorDTO code compatible
    with the core enum (`execution_aborted`, `execution_rejected`,
    `stop_failed`, ...), then carry the operator-facing cause in
    `details.reason_code`. Promote only that known backend taxonomy so
    unrelated details such as runtime-collision monitor internals do not
    replace their stronger top-level codes.
    """

    if isinstance(error_details, dict):
        reason_code = error_details.get("reason_code")
        if (
            isinstance(reason_code, str)
            and reason_code in _ROS2_CONTROL_BACKEND_REASON_CODES
            and error_code in _GENERIC_BACKEND_ERROR_CODES
        ):
            return reason_code
    return error_code


@dataclass
class InterventionSession:
    """Tracks a direct-control intervention session."""
    session_id: str
    parent_run_id: Optional[str]
    entry_state: str
    lease_timeout_ms: int
    lease_expires_at_ns: int
    resume_target: str
    session_kind: str = "manual_intervention"
    authority_source: str = "human"
    principal_id: str = ""
    grant_type: str = "human_approved"
    trigger_reason: str = "manual_request"
    trigger_node_instance: Optional[str] = None
    approval_mode: str = "human_approved"
    artifact_origin: str = "saved_program"
    status: str = "running"  # running, completed, failed, aborted, expired
    actual_outcome: Optional[str] = None
    error: Optional[str] = None
    started_at_ns: int = field(default_factory=time.monotonic_ns)
    finished_at_ns: Optional[int] = None
    events: list[dict[str, Any]] = field(default_factory=list)
    promotions: list[dict[str, Any]] = field(default_factory=list)
    _event_index: int = field(default=0, init=False, repr=False)

    def record_event(self, event_type: str, **details: Any) -> None:
        self._event_index += 1
        self.events.append(
            {
                "event_index": self._event_index,
                "timestamp_ns": time.monotonic_ns(),
                "type": event_type,
                "details": details,
            }
        )

    def renew_lease(self, lease_timeout_ms: Optional[int] = None) -> None:
        if lease_timeout_ms is not None:
            self.lease_timeout_ms = lease_timeout_ms
        self.lease_expires_at_ns = time.monotonic_ns() + (
            self.lease_timeout_ms * 1_000_000
        )
        self.record_event(
            "lease_renewed",
            lease_timeout_ms=self.lease_timeout_ms,
            lease_expires_at_ns=self.lease_expires_at_ns,
        )


@dataclass
class DelegatedRecoveryRequest:
    """Structured request created when a node policy delegates recovery authority."""

    request_id: str
    parent_run_id: Optional[str]
    requested_from_state: str
    trigger_node_instance: str
    trigger_node_id: str
    trigger_reason: RecoveryTrigger
    delegate_policy: str
    allowed_authority_sources: tuple[str, ...]
    allowed_actions: tuple[str, ...]
    allowed_resume_targets: tuple[str, ...] = ("resume", "stop", "reset")
    default_resume_target: str = "resume"
    runtime_override_policy: str = "forbidden"
    persistent_edit_policy: str = "forbidden"
    requested_at_ns: int = field(default_factory=time.monotonic_ns)


@dataclass
class _InterventionContext:
    tree: Any
    node_statuses: dict[str, str]
    stop_requested: bool
    velocity_scale: float


def _collect_statuses(node, result):
    """Recursively collect node statuses from a py_trees node and its children."""
    result[node.name] = node.status.value if node.status else "INVALID"
    if hasattr(node, 'children'):
        for child in node.children:
            _collect_statuses(child, result)


def _iter_tree_nodes(node):
    """Yield program tree nodes in pre-order."""
    yield node
    for child in getattr(node, "children", []) or []:
        yield from _iter_tree_nodes(child)


def _find_failure_feedback(node) -> Optional[str]:
    """Return the deepest failure feedback message available in the tree."""
    if hasattr(node, "children"):
        for child in reversed(node.children):
            child_feedback = _find_failure_feedback(child)
            if child_feedback:
                return child_feedback
    if node.status == py_trees.common.Status.FAILURE:
        feedback = getattr(node, "feedback_message", None)
        if feedback:
            return f"{node.name}: {feedback}"
        return node.name
    return None


class CycleManager:
    """Runtime lifecycle controller for continuous robot programs.

    Four-state machine (spec/400 §6.6.2):

        IDLE ──(run)──► RUNNING ──(stop)──► STOPPED ──(run)──► RUNNING
        STOPPED ──(intervention.enter)──► DIRECT_CONTROL ──(end)──► STOPPED
        Any state ──(reset)──► IDLE

    See `spec/400` §6.6.2 for the canonical states and `spec/460` §6.2
    for the intervention/direct-control transitions.

    Run-side auto-ack (spec/400 §6.6.2): if the kernel is in
    `INTERVENTION_REQUIRED` and the leftover state is provably
    execution-local (scene/program/deployment edits applied, next
    motion can pass kernel validation), the cycle
    manager MAY call `kernel.acknowledge_intervention(...)` on Run.
    Otherwise Run fails closed with a structured reason.

    The CycleManager owns:
    - The OrchestratorRunner (tree builder)
    - The KernelClient (execution engine)
    - The tick thread (runs the tree at ~100Hz)
    - Blackboard keys: stop_requested, velocity_scale
    """
    
    def __init__(self, runner: OrchestratorRunner, kernel: KernelClient):
        self._runner = runner
        self._kernel = kernel
        self._state = CycleState.IDLE
        self._tree = None
        self._tick_thread = None
        self._tick_cleanup_thread: Optional[threading.Thread] = None
        self._tick_stop_event = threading.Event()  # signals tick thread to exit
        self._tick_exit_event = threading.Event()
        self._tick_pause_event = threading.Event()  # cleared = paused, set = running
        self._velocity_scale = 1.0
        self._lock = threading.Lock()
        self._on_run_from_idle: Optional[Callable[[], None]] = None
        self._error: Optional[str] = None
        # Structured cause for the HMI: when the cycle's `_error` is set from
        # a kernel-side fault (collision, intervention, etc.), the kernel's
        # ErrorDTO carries a stable `error_code` plus a `details` map (e.g.
        # `collision_result_first_failure_pair`, `reason_code`,
        # `collision_result_waypoint_index`). Preserving them on the cycle
        # status lets the operator see what actually happened instead of a
        # generic banner. Both are best-effort: motion-soft-fail paths that
        # only produce a string set only `_error` and leave the structured
        # fields None.
        self._error_code: Optional[str] = None
        self._error_details: Optional[dict[str, Any]] = None
        self._node_statuses: dict[str, str] = {}
        self._intervention: Optional[InterventionSession] = None
        self._intervention_context: Optional[_InterventionContext] = None
        self._pending_delegated_recovery: Optional[DelegatedRecoveryRequest] = None
        self._on_delegated_recovery_requested: Optional[
            Callable[[DelegatedRecoveryRequest], None]
        ] = None
        self._on_recovery_exhausted: Optional[
            Callable[[str, str, str], None]
        ] = None
        self._parent_run_id_provider: Optional[Callable[[], Optional[str]]] = None
        self._kernel_events: deque[dict[str, Any]] = deque(maxlen=4096)
        self._kernel_event_index: int = 0
        self._signal_registry = RuntimeSignalRegistry()
        self._signal_managed_keys: set[str] = set()

        # Set initial pause event to set (not paused)
        self._tick_pause_event.set()
        self._tick_exit_event.set()

    def _tree_is_terminal(self) -> bool:
        """True when the current tree has already reached a terminal status."""
        if self._tree is None:
            return False
        return self._tree.root.status in (
            py_trees.common.Status.SUCCESS,
            py_trees.common.Status.FAILURE,
        )

    def _clear_runtime_scene(self) -> None:
        """Return the planning scene to its authored baseline.

        Reset is an industrial "back to known state" operation: the operator
        clicks Reset and expects the next Run to start from the program's
        authored scene, not from whatever runtime state the previous cycle
        accumulated. This method enforces that contract by:

        1. Detaching every currently-attached object. After reset the robot
           is not physically holding anything, so any attachment state from
           the prior execution must be cleared. This applies to authored
           objects too: if `attach_cheese_box` succeeded before the cycle
           was stopped, the cheese box is still attached in the backend —
           leaving it attached would (a) misrepresent physical reality after
           reset and (b) cause the subsequent `apply_scene()` to fail with
           "Object already exists" when it tries to re-add the authored
           cheese box.
        2. Removing every now-unattached object. This wipes both the just-
           detached objects and anything runtime-added (`AddCollisionObject`
           BT node, assistant runtime override, REST scene edit). The kernel
           snapshot used by the next plan is now empty of objects.
        3. `apply_scene()` (called by the caller right after this method)
           re-adds the authored set with the canonical pose and shape.

        Every scene edit's result is checked. A failure to detach or remove
        is a reset-failed condition and propagates as a RuntimeError so the
        operator sees that reset did not return to a known state.
        """
        program = self._runner.program
        if program is None:
            return
        try:
            current_objects = self._kernel.get_scene_objects()
        except (AttributeError, NotImplementedError):
            # Legacy kernel clients that predate `get_scene_objects()` cannot
            # enumerate runtime-added objects. Fall back to the historical
            # behavior — purge authored objects so `apply_scene()` re-adds
            # them cleanly — and accept that runtime-added objects will
            # persist on those clients. Production backends
            # (RoboPlanExecutionBackend, Ros2ControlExecutionBackend) all
            # implement get_scene_objects, so this fallback only affects
            # legacy test mocks.
            for resource in program.scene:
                result = self._kernel.scene_edit(
                    SceneEditRequest(
                        operation="remove", object_id=resource.object_id
                    )
                )
                if not result.success:
                    raise RuntimeError(
                        f"reset failed to remove authored scene object "
                        f"'{resource.object_id}': {result.error}"
                    )
            return
        # Phase 1: detach everything currently attached. Reset means "robot
        # is no longer holding anything" — both authored and runtime-added
        # attachments are cleared.
        for snapshot in current_objects:
            if not snapshot.attached:
                continue
            result = self._kernel.scene_edit(
                SceneEditRequest(
                    operation="detach", object_id=snapshot.object_id
                )
            )
            if not result.success:
                raise RuntimeError(
                    f"reset failed to detach scene object "
                    f"'{snapshot.object_id}': {result.error}"
                )
        # Phase 2: remove every object so apply_scene() can re-add the
        # authored set from a clean slate. We iterate the snapshot taken
        # before phase 1; since detach does not change the set of objects
        # in the scene (only their attach state), the ID list is still
        # valid.
        for snapshot in current_objects:
            result = self._kernel.scene_edit(
                SceneEditRequest(
                    operation="remove", object_id=snapshot.object_id
                )
            )
            if not result.success:
                raise RuntimeError(
                    f"reset failed to remove scene object "
                    f"'{snapshot.object_id}': {result.error}"
                )

    def _prepare_fresh_cycle(self) -> None:
        """Rebuild tree and reapply the program scene without touching robot state."""
        if self._runner.program is None:
            self._runner.load()

        self._clear_motion_paused_blackboard()
        self._set_resume_after_error_flag(False)
        self._clear_runtime_scene()
        self._tree = self._runner.build_tree()
        self._runner.apply_scene()
        self._write_bb_keys()
        with self._lock:
            self._node_statuses = {}
            self._pending_delegated_recovery = None

    @property
    def state(self) -> CycleState:
        """Current lifecycle state."""
        return self._state
    
    @property
    def velocity_scale(self) -> float:
        """Current velocity scale (0.0 to 1.0)."""
        with self._lock:
            return self._velocity_scale

    @property
    def error(self) -> Optional[str]:
        """Last tick-loop exception or surfaced terminal node failure, or None."""
        return self._error

    @property
    def error_code(self) -> Optional[str]:
        """Stable machine-readable code matching the kernel ErrorDTO, when
        the latest error came from a kernel/backend fault. Motion-soft-fail
        errors that only produced a human message leave this None."""
        return self._error_code

    @property
    def error_details(self) -> Optional[dict[str, Any]]:
        """Structured diagnostics map matching the kernel ErrorDTO. Carries
        collision-pair / classification / waypoint-index / reason-code style
        details when the cause is a kernel fault. None when the cause did
        not produce a structured payload."""
        return self._error_details

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        """Get the latest observer-safe execution snapshot from the kernel."""
        return self._kernel.get_execution_snapshot()

    def get_status(self) -> ExecutionStatus:
        """Get the current kernel execution status."""
        return self._kernel.get_status()

    def get_link_pose(self, target_frame: str, base_frame: str) -> Optional[dict]:
        """Get the current backend-authoritative pose of a named robot frame."""
        return self._kernel.get_link_pose(target_frame, base_frame)

    def get_object_pose(self, object_id: str, base_frame: str) -> Optional[dict]:
        """Get the current backend-authoritative object pose."""
        return self._kernel.get_object_pose(object_id, base_frame)

    def get_object_state(self, object_id: str) -> ObjectState:
        """Get the current backend-authoritative object state."""
        return self._kernel.get_object_state(object_id)

    def get_scene_objects(self) -> list[SceneObjectSnapshot]:
        """Get backend-authoritative scene object snapshots."""
        return self._kernel.get_scene_objects()

    def get_gripper_status(self, gripper_id: str):
        """Get the current status of an actuated gripper command channel."""
        return self._kernel.get_gripper_status(gripper_id)

    def get_gripper_configs(self) -> dict[str, dict]:
        """Get deployment gripper configs as JSON-safe dictionaries."""
        deployment = getattr(self._runner, "deployment", None)
        if deployment is None:
            return {}
        return {
                gripper_id: {
                    "command_mode": config.command_mode,
                    "group_id": config.group_id,
                    "joint_name": config.joint_name,
                    "motion_time_s": config.motion_time_s,
                    "srdf_presets": dict(config.srdf_presets),
                    "description": config.description,
                    "touch_links": list(config.touch_links),
                "mount_frame": config.mount_frame,
                "attach_frame": config.attach_frame,
                "tcp_frame": config.tcp_frame,
                "auxiliary_frames": list(config.auxiliary_frames),
            }
            for gripper_id, config in deployment.grippers.items()
        }

    def get_scene_trace(self, limit: Optional[int] = None) -> list[dict]:
        """Get recent backend scene trace events."""
        return self._kernel.get_scene_trace(limit=limit)

    def clear_scene_trace(self) -> None:
        """Clear retained backend scene trace events."""
        self._kernel.clear_scene_trace()
    
    def get_node_statuses(self) -> dict[str, str]:
        """Return the latest atomically-collected node status snapshot.

        Returns:
            Dict mapping node instance name to status string.
            Status values: "SUCCESS", "FAILURE", "RUNNING", "INVALID"
        """
        with self._lock:
            return dict(self._node_statuses)

    def _drain_kernel_events(self) -> None:
        """Pull native kernel events from the kernel and buffer them."""
        events = self._kernel.drain_kernel_events()
        if not events:
            return []
        with self._lock:
            for event in events:
                self._kernel_event_index += 1
                event["event_index"] = self._kernel_event_index
                self._kernel_events.append(event)
        return events

    def drain_kernel_events_now(self) -> list[dict[str, Any]]:
        """Pull pending native kernel events into the indexed event buffer.

        Normal behavior-tree execution drains native events from the tick loop.
        Direct API surfaces (status routes, websocket status broadcasts) bypass
        that loop, so they call this explicit drain before reading the buffered
        event stream.
        """
        return self._drain_kernel_events()

    def get_kernel_events_since(self, event_index: int) -> list[dict[str, Any]]:
        """Return kernel events with event_index greater than the given value."""
        with self._lock:
            return [
                dict(event)
                for event in self._kernel_events
                if event.get("event_index", 0) > event_index
            ]

    @staticmethod
    def _blackboard_value_to_json_safe(value):
        """Best-effort JSON-safe conversion for blackboard values."""
        try:
            json.dumps(value)
            return value
        except TypeError:
            pass

        if hasattr(value, "model_dump"):
            try:
                dumped = value.model_dump()
                json.dumps(dumped)
                return dumped
            except Exception:
                return None

        if isinstance(value, dict):
            converted = {}
            for key, item in value.items():
                if not isinstance(key, str):
                    return None
                converted_item = CycleManager._blackboard_value_to_json_safe(item)
                if converted_item is None and item is not None:
                    return None
                converted[key] = converted_item
            return converted

        if isinstance(value, (list, tuple)):
            converted = []
            for item in value:
                converted_item = CycleManager._blackboard_value_to_json_safe(item)
                if converted_item is None and item is not None:
                    return None
                converted.append(converted_item)
            return converted

        return None

    def get_blackboard_snapshot(self) -> dict[str, dict]:
        """Return a JSON-safe snapshot of the shared py_trees blackboard."""
        snapshot: dict[str, dict] = {}
        for key in sorted(py_trees.blackboard.Blackboard.keys()):
            try:
                value = py_trees.blackboard.Blackboard.get(key)
            except KeyError:
                continue
            value_json = self._blackboard_value_to_json_safe(value)
            entry = {
                "value_json": value_json,
                "type": type(value).__name__,
            }
            if value_json is None:
                value_repr = repr(value)
                truncated = len(value_repr) > 256
                if truncated:
                    value_repr = value_repr[:253] + "..."
                entry["value_repr"] = value_repr
                if truncated:
                    entry["truncated"] = True
            snapshot[key.lstrip("/")] = entry
        return snapshot

    def register_signal_provider(self, provider: SignalProvider) -> None:
        """Register an external signal provider keyed by adapter id."""
        self._signal_registry.register_provider(provider)

    def unregister_signal_provider(self, adapter_id: str) -> None:
        """Remove an external signal provider and clear its mirrored keys."""
        self._signal_registry.unregister_provider(adapter_id)
        self._sync_signal_blackboard(time.monotonic_ns())

    def publish_signal(
        self,
        *,
        adapter: str,
        signal: str,
        value: Any,
        stale_after_ms: int | None = None,
        mirror_key: str | None = None,
        clear_on_stale: bool = True,
        metadata: dict[str, Any] | None = None,
    ) -> None:
        """Publish one external signal sample directly into the runtime registry."""
        stale_after_ns = None
        if stale_after_ms is not None:
            stale_after_ns = max(stale_after_ms, 0) * 1_000_000
        self._signal_registry.publish(
            SignalUpdate(
                adapter=adapter,
                signal=signal,
                value=value,
                stale_after_ns=stale_after_ns,
                mirror_key=mirror_key,
                clear_on_stale=clear_on_stale,
                metadata=metadata or {},
            )
        )
        self._sync_signal_blackboard(time.monotonic_ns())

    def get_signal_snapshot(self) -> dict[str, Any]:
        """Return the current runtime signal/provider snapshot."""
        return self._signal_registry.snapshot(now_ns=time.monotonic_ns())

    def _current_stop_requested(self) -> bool:
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_stop_read")
            bb.register_key(key="stop_requested", access=py_trees.common.Access.READ)
            return bool(bb.stop_requested)
        except (KeyError, AttributeError):
            return False

    def _set_stop_requested_flag(self, stop_requested: bool, *, client_name: str) -> None:
        bb = py_trees.blackboard.Client(name=client_name)
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.stop_requested = stop_requested

    def _read_motion_paused_error(self) -> Optional[str]:
        """Read the motion-soft-fail signal a motion node may have set on the blackboard.

        Returns the human-readable cause string when a motion node has
        captured a runtime failure and is awaiting operator decision;
        returns None otherwise. The motion node sets two keys together:
        ``motion_paused_due_to_error`` (bool) and ``motion_error_message``
        (str). We require both for a positive signal so a stale message
        from a previous run can't trigger an unwanted quiesce.
        """
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_motion_error_read")
            bb.register_key(
                key="motion_paused_due_to_error",
                access=py_trees.common.Access.READ,
            )
            bb.register_key(
                key="motion_error_message",
                access=py_trees.common.Access.READ,
            )
            paused = bool(bb.get("motion_paused_due_to_error"))
        except (KeyError, AttributeError):
            return None
        if not paused:
            return None
        try:
            message = bb.get("motion_error_message")
        except (KeyError, AttributeError):
            message = None
        if isinstance(message, str) and message.strip():
            return message.strip()
        return "Motion failed without a cause string"

    def _enter_motion_paused_state(self, error_message: str) -> None:
        """Quiesce the runtime in response to a motion soft-fail.

        Mirrors stop() with two differences: the error string is
        recorded on ``self._error`` so the cycle status surfaces the
        cause, and the tick thread is suspended (via _tick_pause_event)
        rather than terminated, so a subsequent run() can resume the
        same tree with the same node holding its position.

        Also captures the kernel's structured error_code / error_details
        from the latest execution snapshot when available, so the HMI can
        render the actual cause (collision pair, classification, waypoint
        index, fault reason code) instead of only the blackboard string.
        """
        self._quiesce_runtime(
            client_name="cycle_manager_motion_paused",
            kernel_action="pause_if_executing",
        )
        structured_code, structured_details = self._capture_kernel_error_diagnostics()
        with self._lock:
            self._error = error_message
            self._error_code = structured_code
            self._error_details = structured_details
            self._state = CycleState.STOPPED

    def _capture_kernel_error_diagnostics(
        self,
    ) -> tuple[Optional[str], Optional[dict[str, Any]]]:
        """Best-effort read of the kernel's structured error diagnostics.

        Two complementary sources are consulted:

        1. **ExecutionSnapshot error_code/error_details.** The kernel
           populates these when a kernel-side ErrorDTO is the cause (plan
           rejection, validation failure, dispatch refusal). Older kernel
           clients without the structured fields return (None, None).

        2. **Recent kernel events.** For runtime fault interrupts (a
           dynamic body appearing on the trajectory during execution),
           the kernel transitions to `Interrupted` but DOES NOT populate
           snapshot.error — the rich diagnostics live on the event stream
           (`collision_detected`, `interrupt_latched`). Without harvesting
           events the operator only sees a fallback "Motion execution
           ended in interrupted" string. We scan the most recent buffered
           events for the latest interrupt-causing record and synthesize
           the structured fields from its details payload.

        Source #1 wins when both are present (kernel-side errors carry the
        canonical ErrorDTO). Source #2 backfills the runtime-interrupt
        case. Returns (None, None) when neither source has anything useful.
        """
        try:
            snapshot = self._kernel.get_execution_snapshot()
        except Exception:  # noqa: BLE001 — diagnostics best-effort
            snapshot = None
        snapshot_code = (
            getattr(snapshot, "error_code", None) or None if snapshot else None
        )
        snapshot_details = (
            getattr(snapshot, "error_details", None) if snapshot else None
        )
        if not snapshot_details:
            snapshot_details = None
        if snapshot_code or snapshot_details:
            return (
                preferred_structured_error_code(snapshot_code, snapshot_details),
                snapshot_details,
            )
        # Snapshot has nothing — try the event stream. Walk recent events
        # newest-first looking for a record that carries diagnostics worth
        # surfacing. `collision_detected` is the strongest signal (full
        # collision-result map); `interrupt_latched` is next (policy +
        # latch identity). Other event types are not currently mapped.
        return self._harvest_kernel_event_diagnostics()

    def _harvest_kernel_event_diagnostics(
        self,
    ) -> tuple[Optional[str], Optional[dict[str, Any]]]:
        """Scan recent buffered kernel events for a structured cause when
        the snapshot's error field is empty.

        Event priority (not chronology):
          1. `collision_detected` — carries the colliding pair, the
             classification, the failing waypoint, the dynamic-body id,
             and the snapshot revision. This is the strongest operator-
             facing diagnostic for a runtime collision, so it wins over
             any other recent event regardless of order. The runtime
             monitor publishes collision_detected BEFORE interrupt_latched
             during a fault sequence, so a naive newest-first walk would
             pick the latch event and discard the richer collision data.
          2. `interrupt_latched` — fallback when no collision_detected is
             present in the window (e.g. drift-fault interrupts, operator-
             initiated preempts, etc.). Carries the policy id + latch
             identity but no causal diagnostics.

        Other event types are not currently mapped. The lookup window is
        bounded to the last 64 events to keep the cost predictable for
        long-running cycles. Within each priority tier we still prefer
        the most recent event (newest-first), so a fresh collision wins
        over a stale one from earlier in the cycle.
        """
        try:
            self._drain_kernel_events()
        except Exception:  # noqa: BLE001 — drain is best-effort here
            pass
        events: list[dict[str, Any]]
        with self._lock:
            events = list(self._kernel_events)[-64:]

        latest_collision: Optional[dict[str, Any]] = None
        latest_latch: Optional[dict[str, Any]] = None
        for event in reversed(events):
            event_type = event.get("type") or event.get("event_type") or ""
            details = event.get("details")
            if not isinstance(details, dict) or not details:
                continue
            if event_type == "collision_detected" and latest_collision is None:
                latest_collision = dict(details)
                # We have the highest-priority signal; no need to keep
                # scanning for older latches.
                break
            if event_type == "interrupt_latched" and latest_latch is None:
                latest_latch = dict(details)
                # Don't break — keep scanning in case an older
                # collision_detected is still in the window.

        if latest_collision is not None:
            # Runtime collision monitor is operationally distinct from a
            # pre-execute validation rejection: the trajectory PASSED
            # validation, then a dynamic body appeared during execution
            # and the runtime monitor latched. Use a separate code so
            # the HMI can route to the right operator-facing copy.
            return "runtime_collision", latest_collision
        if latest_latch is not None:
            return "motion_interrupt_latched", latest_latch
        return None, None

    def _set_resume_after_error_flag(self, value: bool) -> None:
        """Toggle the blackboard signal that tells the motion node to retry.

        Set to True before transitioning STOPPED→RUNNING when a motion
        soft-fail is in flight. The motion node consumes the flag on its
        next tick, clears its internal soft-fail state, and re-issues
        the same kernel goal.
        """
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_resume_after_error")
            bb.register_key(
                key="resume_after_error_requested",
                access=py_trees.common.Access.WRITE,
            )
            bb.set("resume_after_error_requested", value)
        except (KeyError, AttributeError):
            pass

    def _clear_motion_paused_blackboard(self) -> None:
        """Clear the soft-fail signal so the cycle resumes cleanly on next tick."""
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_motion_paused_clear")
            bb.register_key(
                key="motion_paused_due_to_error",
                access=py_trees.common.Access.WRITE,
            )
            bb.register_key(
                key="motion_error_message",
                access=py_trees.common.Access.WRITE,
            )
            bb.register_key(
                key="motion_paused_node_name",
                access=py_trees.common.Access.WRITE,
            )
            bb.set("motion_paused_due_to_error", False)
            bb.set("motion_error_message", None)
            bb.set("motion_paused_node_name", None)
        except (KeyError, AttributeError):
            pass

    def _quiesce_runtime(
        self,
        *,
        client_name: str,
        kernel_action: str,
    ) -> None:
        """Enter a quiesced runtime mode shared by stop and reset paths."""
        self._set_stop_requested_flag(True, client_name=client_name)

        kernel_status = self._kernel.get_status()
        if kernel_action == "pause_if_executing":
            if kernel_status.state == ExecutionState.EXECUTING:
                self._kernel.pause()
        elif kernel_action == "stop_if_active":
            stoppable = {
                ExecutionState.EXECUTING,
                ExecutionState.PLANNING,
                ExecutionState.VALIDATING,
                ExecutionState.INTERRUPTED,
                ExecutionState.PAUSED,
                ExecutionState.STOPPING,
            }
            if kernel_status.state in stoppable:
                self._kernel.stop()
        else:
            raise ValueError(f"unsupported kernel_action: {kernel_action}")

        self._tick_pause_event.clear()

    def _restore_blackboard_runtime_state(
        self,
        *,
        stop_requested: bool,
        velocity_scale: float,
    ) -> None:
        bb = py_trees.blackboard.Client(name="cycle_manager_restore")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.stop_requested = stop_requested
        bb.velocity_scale = velocity_scale

    def _iter_runtime_tree_nodes(self, node):
        yield node
        for child in getattr(node, "children", []) or []:
            yield from self._iter_runtime_tree_nodes(child)
        decorated = getattr(node, "decorated", None)
        if decorated is not None:
            yield from self._iter_runtime_tree_nodes(decorated)

    def _start_tick_thread(self, target) -> None:
        self._tick_stop_event.clear()
        self._tick_exit_event.clear()
        self._tick_pause_event.set()
        self._tick_thread = threading.Thread(target=target, daemon=True)
        self._tick_thread.start()

    def set_parent_run_id_provider(
        self,
        provider: Optional[Callable[[], Optional[str]]],
    ) -> None:
        """Install a callback used to correlate delegated recovery requests to runs."""
        self._parent_run_id_provider = provider

    def set_on_delegated_recovery_requested(
        self,
        callback: Optional[Callable[[DelegatedRecoveryRequest], None]],
    ) -> None:
        """Install a callback invoked when a delegated recovery request is raised."""
        self._on_delegated_recovery_requested = callback

    def set_on_recovery_exhausted(
        self,
        callback: Optional[Callable[[str, str, str], None]],
    ) -> None:
        """Install a callback invoked when program_only recovery is exhausted.

        Callback receives (node_instance, node_id, trigger_reason_value).
        """
        self._on_recovery_exhausted = callback

    def _can_auto_acknowledge_leftover_state(self) -> tuple[bool, str]:
        """Check whether kernel-side leftover state is safe to clear on Run.

        Per `spec/400` §6.6.2, Run-side auto-ack of `INTERVENTION_REQUIRED`
        is permitted only when the leftover state is execution-local and
        the next command can pass ManyForge-owned checks. External plant
        safety (PLC, scanners, robot safety controllers) is intentionally
        out of scope; this method only validates ManyForge-owned state.

        Returns (ok, reason). When ok is False, reason is a stable
        identifier suitable for surfacing to the operator.
        """
        try:
            kernel_status = self._kernel.get_status()
        except Exception as exc:  # noqa: BLE001 — propagate as structured reason
            return False, f"kernel_status_unreadable:{type(exc).__name__}"

        # Refuse if the kernel is in a transitional/stopping state that a
        # supervisory Run cannot safely override; the operator must
        # end/stop it first.
        active_session_states = (
            ExecutionState.INTERRUPTED,
            ExecutionState.STOPPING,
        )
        if kernel_status.state in active_session_states:
            return False, "active_or_stopping"

        # Refuse if the cycle-manager-tracked intervention session is still
        # marked running: that means a direct-control lease still owns
        # blackboard scope. The operator must end the intervention session
        # via the explicit end/promote path before Run can auto-ack.
        if self._intervention is not None and self._intervention.status == "running":
            return False, "intervention_session_running"

        return True, "ok"

    def run(self) -> None:
        """
        Start or resume program execution.
        
        If IDLE with no prepared tree: runner.load(), runner.build_tree(),
                 runner.apply_scene(), write BB keys, start tick thread.
                 State → RUNNING.
        If IDLE with a prepared non-terminal tree (for example after Reset):
                 write BB keys, start tick thread. State → RUNNING.
        If IDLE with a terminal tree (SUCCESS/FAILURE): no-op. A fresh cycle
                 must be prepared explicitly via Reset before another run.
        If STOPPED: clear stop_requested on BB, resume tick thread. State → RUNNING.
                    If kernel is PAUSED, call kernel.resume().
        If RUNNING: no-op.
        """
        prepare_fresh = False
        start_from_idle = False
        on_run_from_idle: Optional[Callable[[], None]] = None

        with self._lock:
            if self._state == CycleState.RUNNING:
                return  # Already running, no-op

            self._error = None
            self._error_code = None
            self._error_details = None

            if self._state == CycleState.IDLE:
                # Guard: refuse to start if a zombie tick thread is still alive
                if self._tick_thread is not None and self._tick_thread.is_alive():
                    raise RuntimeError("Cannot start new cycle: previous tick thread is still alive")
                if self._tree is None:
                    prepare_fresh = True
                elif self._tree_is_terminal():
                    return
                start_from_idle = True
                on_run_from_idle = self._on_run_from_idle

            elif self._state == CycleState.STOPPED:
                # Resuming from stopped state
                self._pending_delegated_recovery = None
                bb = py_trees.blackboard.Client(name="cycle_manager_resume")
                bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
                bb.stop_requested = False

                motion_paused_error = self._read_motion_paused_error()
                kernel_status = self._kernel.get_status()

                # If we're resuming from a motion soft-fail (the cycle
                # was put into STOPPED by _enter_motion_paused_state),
                # Run is an explicit operator retry. A runtime collision
                # fault leaves the kernel in INTERRUPTED with a latched
                # ManyForge-owned interrupt; clearing the scene object alone
                # does not clear that latch. Acknowledge only the interrupted
                # execution boundary before re-ticking the held motion node;
                # do not call kernel.reset(), because ROS/Isaac reset may
                # command the backend back to its initial/home pose. If the
                # obstacle is still present, the fresh execute/monitor pass
                # will latch a new collision immediately.
                if (
                    motion_paused_error is not None
                    and kernel_status.state == ExecutionState.INTERRUPTED
                ):
                    ack_interrupted = self._kernel.acknowledge_interrupted_for_retry(
                        principal_id="cycle_manager_run",
                        reason="run_retry_after_motion_fault",
                    )
                    if not ack_interrupted.accepted:
                        raise RuntimeError(
                            "kernel.acknowledge_interrupted_for_retry rejected: "
                            f"{ack_interrupted.error}"
                        )
                    kernel_status = self._kernel.get_status()

                # Clear the soft-fail blackboard keys and set the resume
                # signal so the holding motion node retries its goal on the
                # next tick rather than re-emitting the same error.
                if motion_paused_error is not None:
                    self._clear_motion_paused_blackboard()
                    self._set_resume_after_error_flag(True)

                # Run-side auto-ack of `INTERVENTION_REQUIRED`.
                # The kernel rejects `execute()` while intervention is
                # required, so a stopped cycle whose underlying kernel
                # ended in intervention cannot resume without an explicit
                # ack. spec/400 §6.6.2 permits an automatic ack only when
                # the leftover state is execution-local and the next
                # command can pass ManyForge-owned checks; otherwise Run
                # MUST fail closed.
                if kernel_status.state == ExecutionState.INTERVENTION_REQUIRED:
                    ok, reason = self._can_auto_acknowledge_leftover_state()
                    if not ok:
                        raise RuntimeError(
                            "Cannot auto-acknowledge kernel "
                            f"INTERVENTION_REQUIRED on Run: {reason}. "
                            "Explicit operator recovery required."
                        )
                    ack = self._kernel.acknowledge_intervention(
                        principal_id="cycle_manager_run",
                        reason="run_auto_ack_local_leftover",
                    )
                    if not ack.accepted:
                        raise RuntimeError(
                            "kernel.acknowledge_intervention rejected: "
                            f"{ack.error}"
                        )
                    kernel_status = self._kernel.get_status()

                if kernel_status.state == ExecutionState.PAUSED:
                    self._kernel.resume()

                self._tick_pause_event.set()
                if self._tick_thread is None or not self._tick_thread.is_alive():
                    if self._tree is None:
                        raise RuntimeError("Cannot resume stopped cycle without a tree")
                    self._start_tick_thread(self._tick_loop)
                self._state = CycleState.RUNNING

        if start_from_idle:
            if prepare_fresh:
                self._prepare_fresh_cycle()
            else:
                self._write_bb_keys()

            if on_run_from_idle:
                on_run_from_idle()

            with self._lock:
                self._start_tick_thread(self._tick_loop)
                self._state = CycleState.RUNNING

    def stop(self) -> None:
        """
        Pause/hold program execution at the cycle layer.

        If RUNNING: set stop_requested=True on BB, suspend tick thread.
                    If kernel state is EXECUTING, call kernel.pause().
                    A later Run from STOPPED resumes by replanning from
                    fresh measured state to the original motion goal.
                    State → STOPPED.
        If STOPPED or IDLE: no-op.
        """
        with self._lock:
            if self._state != CycleState.RUNNING:
                return  # Not running, no-op

        self._quiesce_runtime(
            client_name="cycle_manager_stop",
            kernel_action="pause_if_executing",
        )

        with self._lock:
            self._state = CycleState.STOPPED

    def _await_zombie_thread_exit(self, thread_to_join) -> None:
        thread_to_join.join()
        with self._lock:
            if self._tick_cleanup_thread is threading.current_thread():
                self._tick_cleanup_thread = None
            if self._tick_thread is thread_to_join:
                self._tick_thread = None
    
    def reset(self) -> None:
        """
        Reset program to initial state.
        
        If RUNNING: stop() first.
        Rebuilds the software/runtime scene state for the currently-loaded
        program without teleporting the robot.
        """
        with self._lock:
            need_stop = (self._state == CycleState.RUNNING)

        if need_stop:
            self.stop()

        thread_to_join = None
        with self._lock:
            # Stop tick thread if running
            if self._tick_thread is not None and self._tick_thread.is_alive():
                self._tick_stop_event.set()
                thread_to_join = self._tick_thread

        if thread_to_join is not None:
            if not self._tick_exit_event.wait(timeout=0.025):
                blocked_stack = None
                if thread_to_join.ident is not None:
                    frame = sys._current_frames().get(thread_to_join.ident)
                    if frame is not None:
                        blocked_stack = "".join(traceback.format_stack(frame))
                if blocked_stack:
                    logger.error(
                        "Tick thread did not terminate during reset grace window — zombie thread detected\n"
                        "Blocked tick thread stack follows:\n%s",
                        blocked_stack,
                    )
                else:
                    logger.error(
                        "Tick thread did not terminate during reset grace window — zombie thread detected"
                    )
                with self._lock:
                    self._error = "tick thread did not terminate"
                    self._error_code = "tick_thread_zombie"
                    self._error_details = None
                    self._state = CycleState.IDLE
                    self._pending_delegated_recovery = None
                    cleanup_alive = (
                        self._tick_cleanup_thread is not None
                        and self._tick_cleanup_thread.is_alive()
                    )
                    if not cleanup_alive:
                        self._tick_cleanup_thread = threading.Thread(
                            target=self._await_zombie_thread_exit,
                            args=(thread_to_join,),
                            name="manyforge_tick_cleanup",
                            daemon=True,
                        )
                        self._tick_cleanup_thread.start()
                return
            with self._lock:
                if self._tick_thread is thread_to_join:
                    self._tick_thread = None

        kernel_status = self._kernel.get_status()
        if kernel_status.state != ExecutionState.IDLE:
            self._kernel.reset()

        self._prepare_fresh_cycle()

        with self._lock:
            self._tick_stop_event.clear()
            self._tick_pause_event.set()
            self._error = None
            self._error_code = None
            self._error_details = None
            self._state = CycleState.IDLE
            self._pending_delegated_recovery = None

    def set_velocity_scale(
        self, scale: float, principal_id: str | None = None
    ) -> None:
        """Set velocity_scale on blackboard. Clamped to [0.0, 1.0].

        Rejects NaN and infinity — these would propagate to the robot.

        ADR-0018: also forward the new scale to the kernel/backend runtime
        register. Discrete trajectories read that register while they are in
        flight, so the operator slider retimes the current motion without
        stopping or re-dispatching behavior-tree nodes.
        """
        import math

        if not isinstance(scale, (int, float)) or math.isnan(scale) or math.isinf(scale):
            raise ValueError(f"velocity_scale must be a finite number, got {scale!r}")
        scale = max(0.0, min(1.0, float(scale)))
        with self._lock:
            previous_scale = self._velocity_scale
            self._velocity_scale = scale

        bb = py_trees.blackboard.Client(name="cycle_manager_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.velocity_scale = scale

        try:
            forward = getattr(self._kernel, "set_runtime_velocity_scale", None)
            if forward is not None:
                forward(scale, principal_id=principal_id)
        except Exception:
            logger.debug(
                "kernel.set_runtime_velocity_scale failed; HMI runtime "
                "velocity slider will retain the backend's previous scale",
                exc_info=True,
            )
        if previous_scale != scale:
            details = {
                "event": "runtime_velocity_scale_changed",
                "previous_scale": previous_scale,
                "new_scale": scale,
                "backend_status": "forwarded",
            }
            if principal_id:
                details["principal_id"] = principal_id
            logger.info(
                "runtime_velocity_scale_changed",
                extra=details,
            )

    def _refresh_signal_registry(self) -> None:
        """Poll external signal providers and mirror fresh values to blackboard."""
        now_ns = time.monotonic_ns()
        self._signal_registry.poll_providers(now_ns=now_ns)
        self._sync_signal_blackboard(now_ns)

    def _sync_signal_blackboard(self, now_ns: int) -> None:
        snapshot = self._signal_registry.snapshot(now_ns=now_ns)
        current_keys: set[str] = set()
        for signal_data in snapshot["signals"].values():
            mirror_key = signal_data.get("mirrorKey") or signal_data.get("signal")
            normalized = self._normalize_blackboard_key(str(mirror_key))
            current_keys.add(normalized)
            if signal_data.get("freshness") == "fresh":
                py_trees.blackboard.Blackboard.set(normalized, signal_data.get("value"))
            elif signal_data.get("clearOnStale", True):
                py_trees.blackboard.Blackboard.unset(normalized)

        for managed_key in self._signal_managed_keys - current_keys:
            py_trees.blackboard.Blackboard.unset(managed_key)
        self._signal_managed_keys = current_keys
    
    def _tick_loop(self) -> None:
        """Background thread: tick the tree at ~100Hz (10ms period).
        
        Each iteration:
        1. Check _tick_stop_event — if set, exit thread
        2. Wait on _tick_pause_event — blocks when paused
        3. Tick the tree
        4. If tree root status is SUCCESS or FAILURE, stop
        5. Sleep remainder of 10ms period
        """
        period_s = 0.010  # 10ms = 100Hz
        
        while not self._tick_stop_event.is_set():
            # Wait for pause event (blocks when paused)
            if not self._tick_pause_event.wait(timeout=0.001):
                continue  # Paused, loop again

            self._refresh_signal_registry()

            # Tick the tree
            start_time = time.monotonic()
            try:
                self._tree.tick()
            except Exception:
                error_msg = traceback.format_exc()
                logger.error("Tick loop exception: %s", error_msg)
                self._tick_stop_event.set()
                with self._lock:
                    self._error = error_msg
                    self._error_code = "tick_loop_exception"
                    self._error_details = None
                    self._state = CycleState.IDLE
                    self._tick_thread = None
                break
            
            # Drain native kernel events
            kernel_events = self._drain_kernel_events()

            # Collect node statuses atomically (F-6: thread-safe observer snapshot)
            statuses: dict[str, str] = {}
            _collect_statuses(self._tree.root, statuses)
            with self._lock:
                self._node_statuses = statuses

            # Motion soft-fail: a motion node has captured a runtime
            # failure (collision, planning timeout, execution rejection,
            # etc.) and is holding Status.RUNNING with the cause stored
            # on the blackboard. Quiesce the runtime so the operator can
            # see the error and decide between Run-again (retry the same
            # motion) and Reset (clear the cycle). Without this branch
            # the motion node would hold RUNNING forever and the
            # operator would have no signal that anything went wrong.
            motion_error = self._read_motion_paused_error()
            if motion_error is not None:
                self._enter_motion_paused_state(motion_error)
                continue

            # Check if tree completed
            status = self._tree.root.status
            if status in (py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE):
                self._tick_stop_event.set()
                if status == py_trees.common.Status.FAILURE:
                    failure_feedback = _find_failure_feedback(self._tree.root)
                    delegated_candidate = self._find_delegated_recovery_candidate(statuses)
                    if delegated_candidate is not None:
                        delegate_policy = delegated_candidate.modification_policy.get(
                            "intervention_delegate", "forbidden"
                        )
                        trigger = self._derive_delegated_trigger_reason()
                        if delegate_policy == "program_only":
                            # program_only recovery is handled by in-tree recovery
                            # decorators. If the tree still failed, recovery is
                            # exhausted — stop without external delegation request.
                            logger.warning(
                                "program_only recovery exhausted for node '%s' "
                                "(trigger=%s) — stopping",
                                delegated_candidate.name,
                                trigger.value,
                            )
                            bb = py_trees.blackboard.Client(name="cycle_manager_recovery_exhausted")
                            bb.register_key(
                                key="stop_requested",
                                access=py_trees.common.Access.WRITE,
                            )
                            bb.stop_requested = True
                            tick_code, tick_details = (
                                self._capture_kernel_error_diagnostics()
                            )
                            with self._lock:
                                self._error = failure_feedback
                                self._error_code = (
                                    tick_code or "recovery_exhausted"
                                )
                                self._error_details = tick_details
                                self._state = CycleState.STOPPED
                                self._tick_thread = None
                            self._notify_recovery_exhausted(
                                delegated_candidate, trigger
                            )
                            break
                        request = self._build_delegated_recovery_request(
                            delegated_candidate,
                            trigger_reason=trigger,
                        )
                        bb = py_trees.blackboard.Client(name="cycle_manager_delegate_stop")
                        bb.register_key(
                            key="stop_requested",
                            access=py_trees.common.Access.WRITE,
                        )
                        bb.stop_requested = True
                        delegated_code, delegated_details = (
                            self._capture_kernel_error_diagnostics()
                        )
                        with self._lock:
                            self._error = failure_feedback
                            self._error_code = (
                                delegated_code or "delegated_recovery_requested"
                            )
                            self._error_details = delegated_details
                            self._pending_delegated_recovery = request
                            self._state = CycleState.STOPPED
                            self._tick_thread = None
                        self._notify_delegated_recovery_requested(request)
                        break
                terminal_code, terminal_details = (
                    (None, None)
                    if status != py_trees.common.Status.FAILURE
                    else self._capture_kernel_error_diagnostics()
                )
                with self._lock:
                    if status == py_trees.common.Status.FAILURE:
                        self._error = failure_feedback
                        self._error_code = (
                            terminal_code or "tree_failed"
                        )
                        self._error_details = terminal_details
                    else:
                        self._error = None
                        self._error_code = None
                        self._error_details = None
                    self._state = CycleState.IDLE
                    self._tick_thread = None
                break
            
            # Sleep remainder of period
            elapsed = time.monotonic() - start_time
            sleep_time = period_s - elapsed
            if sleep_time > 0:
                # Audit 6D (finding 2.9): interruptible slice — a stop
                # request wakes the loop immediately instead of waiting out
                # the remainder of the tick period.
                self._tick_stop_event.wait(timeout=sleep_time)

        with self._lock:
            if self._tick_thread is threading.current_thread():
                self._tick_thread = None
        self._tick_exit_event.set()
    
    @property
    def intervention(self) -> Optional[InterventionSession]:
        """Current or most recent intervention session, or None."""
        return self._intervention

    @property
    def pending_delegated_recovery(self) -> Optional[DelegatedRecoveryRequest]:
        """Current delegated recovery request waiting to be serviced, if any."""
        return self._pending_delegated_recovery

    @staticmethod
    def _normalize_blackboard_key(key: str) -> str:
        return key.lstrip("/").replace(".", "/")

    @classmethod
    def _scoped_intervention_key(cls, session_id: str, key: str) -> str:
        normalized = cls._normalize_blackboard_key(key)
        if normalized.startswith("_intervention/"):
            return normalized
        return f"_intervention/{session_id}/{normalized}"

    def _rewrite_intervention_tree(
        self,
        tree_dict: dict[str, Any],
        session_id: str,
    ) -> dict[str, Any]:
        catalog = self._runner.catalog
        if catalog is None:
            raise RuntimeError("Runner catalog must be loaded before intervention rewriting")
        program = self._runner.program
        declared_keys = {} if program is None else program.blackboard_keys

        local_keys: set[str] = set()

        def blackboard_param_names(node_id: str) -> tuple[tuple[str, ...], tuple[str, ...]]:
            try:
                entry = catalog.get(node_id)
            except KeyError:
                return (), ()
            return tuple(entry.bb_writer_params), tuple(entry.bb_reader_params)

        def extract_key_ref(value: Any) -> str | None:
            if isinstance(value, str):
                return value
            if (
                isinstance(value, dict)
                and isinstance(value.get("key_ref"), str)
            ):
                return value["key_ref"]
            return None

        def resolve_runtime_key(value: Any) -> str | None:
            key_ref = extract_key_ref(value)
            if key_ref is None:
                return None
            declared = declared_keys.get(key_ref)
            if declared is not None:
                return declared.key
            return key_ref

        def collect_keys(node: dict[str, Any]) -> None:
            params = node.get("params", {})
            writer_params, _ = blackboard_param_names(node.get("id", ""))
            for param_name in writer_params:
                value = params.get(param_name)
                runtime_key = resolve_runtime_key(value)
                if runtime_key is not None:
                    local_keys.add(self._normalize_blackboard_key(runtime_key))
            for child in node.get("children", []) or []:
                collect_keys(child)

        def rewrite(node: dict[str, Any]) -> dict[str, Any]:
            rewritten = deepcopy(node)
            params = dict(rewritten.get("params", {}))
            node_id = rewritten.get("id", "")
            writer_params, reader_params = blackboard_param_names(node_id)
            for param_name in writer_params:
                value = params.get(param_name)
                runtime_key = resolve_runtime_key(value)
                if runtime_key is not None:
                    params[param_name] = self._scoped_intervention_key(session_id, runtime_key)
            for param_name in reader_params:
                value = params.get(param_name)
                runtime_key = resolve_runtime_key(value)
                if runtime_key is not None:
                    normalized = self._normalize_blackboard_key(runtime_key)
                    if normalized in local_keys:
                        params[param_name] = self._scoped_intervention_key(session_id, runtime_key)
                    elif key_ref_id(value) is not None:
                        params[param_name] = runtime_key
            rewritten["params"] = params
            rewritten["children"] = [rewrite(child) for child in rewritten.get("children", []) or []]
            return rewritten

        collect_keys(tree_dict)
        return rewrite(tree_dict)

    def renew_intervention_lease(
        self,
        lease_timeout_ms: Optional[int] = None,
    ) -> InterventionSession:
        with self._lock:
            if self._intervention is None or self._intervention.status != "running":
                raise RuntimeError("No running intervention session")
            self._intervention.renew_lease(lease_timeout_ms=lease_timeout_ms)
            return self._intervention

    def promote_intervention_values(
        self,
        promote_keys: dict[str, str],
        *,
        session_id: Optional[str] = None,
    ) -> InterventionSession:
        """Promote scoped intervention blackboard values back to parent keys."""
        if not isinstance(promote_keys, dict) or not promote_keys:
            raise RuntimeError("promote_keys must be a non-empty mapping")

        with self._lock:
            session = self._intervention

        if session is None:
            raise RuntimeError("No intervention session")
        if session_id is not None and session.session_id != session_id:
            raise RuntimeError("session_id does not match the current or most recent intervention")

        resolved_values: list[tuple[str, str, Any]] = []
        missing_sources: list[str] = []
        for source_key, target_key in promote_keys.items():
            if not isinstance(source_key, str) or not source_key.strip():
                raise RuntimeError("promote_keys source keys must be non-empty strings")
            if not isinstance(target_key, str) or not target_key.strip():
                raise RuntimeError("promote_keys target values must be non-empty strings")
            scoped_source = self._scoped_intervention_key(session.session_id, source_key)
            try:
                value = py_trees.blackboard.Blackboard.get(scoped_source)
            except KeyError:
                missing_sources.append(source_key)
                continue
            resolved_values.append(
                (
                    source_key,
                    self._normalize_blackboard_key(target_key),
                    deepcopy(value),
                )
            )

        if missing_sources:
            raise RuntimeError(
                "Cannot promote missing intervention blackboard keys: "
                + ", ".join(sorted(missing_sources))
            )

        timestamp_ns = time.monotonic_ns()
        promoted_entries: list[dict[str, Any]] = []
        for source_key, target_key, value in resolved_values:
            py_trees.blackboard.Blackboard.set(target_key, value)
            promoted_entries.append(
                {
                    "source_key": self._scoped_intervention_key(session.session_id, source_key),
                    "target_key": target_key,
                    "timestamp_ns": timestamp_ns,
                }
            )

        session.promotions.extend(promoted_entries)
        session.record_event(
            "blackboard_promoted",
            promoted=[
                {
                    "source_key": entry["source_key"],
                    "target_key": entry["target_key"],
                }
                for entry in promoted_entries
            ],
        )
        return session

    def _current_parent_run_id(self) -> Optional[str]:
        if self._parent_run_id_provider is None:
            return None
        try:
            return self._parent_run_id_provider()
        except Exception:
            logger.exception("parent_run_id provider failed")
            return None

    def _notify_delegated_recovery_requested(
        self,
        request: DelegatedRecoveryRequest,
    ) -> None:
        if self._on_delegated_recovery_requested is None:
            return
        try:
            self._on_delegated_recovery_requested(request)
        except Exception:
            logger.exception("delegated recovery callback failed")

    def _notify_recovery_exhausted(
        self,
        node,
        trigger: RecoveryTrigger,
    ) -> None:
        """Notify that program_only recovery was exhausted for a node."""
        if self._on_recovery_exhausted is None:
            return
        try:
            self._on_recovery_exhausted(node.name, node.id, trigger.value)
        except Exception:
            logger.exception("recovery exhausted callback failed")

    def _find_delegated_recovery_candidate(
        self,
        statuses: dict[str, str],
    ):
        program = self._runner.program
        if program is None or program.tree is None:
            return None

        def visit(node):
            for child in node.children:
                candidate = visit(child)
                if candidate is not None:
                    return candidate
            if statuses.get(node.name) != py_trees.common.Status.FAILURE.value:
                return None
            if node.modification_policy.get("intervention_delegate", "forbidden") == "forbidden":
                return None
            return node

        return visit(program.tree)

    def _derive_delegated_trigger_reason(self) -> RecoveryTrigger:
        # Maps kernel event types to canonical recovery triggers
        # (spec/460 §6.2.2). The legacy "timeout" / NODE_FAILURE
        # surfaces are deprecated and intentionally not used here.
        _EVENT_TO_TRIGGER = {
            "planning_failed": RecoveryTrigger.PLANNING_FAILED,
            "validation_failed": RecoveryTrigger.VALIDATION_FAILED,
            "execute_failed": RecoveryTrigger.EXECUTE_FAILED,
            "timeout": RecoveryTrigger.EXECUTE_TIMEOUT,
            "scene_mismatch": RecoveryTrigger.SCENE_MISMATCH,
        }
        with self._lock:
            for event in reversed(self._kernel_events):
                event_type = event.get("type")
                trigger = _EVENT_TO_TRIGGER.get(event_type)
                if trigger is not None:
                    return trigger
                if event_type == "error_raised":
                    return RecoveryTrigger.EXECUTE_FAILED
                if (
                    event_type == "state_transition"
                    and event.get("to_state") == "failed"
                ):
                    return RecoveryTrigger.EXECUTE_FAILED
        # No identifying event in the window; fall back to a generic
        # execute-failure rather than the deprecated NODE_FAILURE alias.
        return RecoveryTrigger.EXECUTE_FAILED

    def _build_delegated_recovery_request(
        self,
        node,
        *,
        trigger_reason: RecoveryTrigger,
    ) -> DelegatedRecoveryRequest:
        delegate_policy = node.modification_policy.get("intervention_delegate", "forbidden")
        authority_map = {
            "human_only": ("human",),
            "llm_allowed": ("human", "llm"),
            "program_only": ("program_node",),
        }
        allowed_actions = ["intervention"]
        runtime_override_policy = node.modification_policy.get("runtime_override", "forbidden")
        persistent_edit_policy = node.modification_policy.get("persistent_edit", "forbidden")
        if runtime_override_policy != "forbidden":
            allowed_actions.append("runtime_override")
        if persistent_edit_policy != "forbidden":
            allowed_actions.append("draft_patch")
        return DelegatedRecoveryRequest(
            request_id=uuid.uuid4().hex[:12],
            parent_run_id=self._current_parent_run_id(),
            requested_from_state=CycleState.RUNNING.value,
            trigger_node_instance=node.name,
            trigger_node_id=node.id,
            trigger_reason=trigger_reason,
            delegate_policy=delegate_policy,
            allowed_authority_sources=authority_map.get(delegate_policy, tuple()),
            allowed_actions=tuple(allowed_actions),
            runtime_override_policy=runtime_override_policy,
            persistent_edit_policy=persistent_edit_policy,
        )

    def _finish_intervention(
        self,
        session: InterventionSession,
        *,
        status: str,
        actual_outcome: Optional[str] = None,
        error: Optional[str] = None,
    ) -> None:
        session.status = status
        session.error = error
        session.finished_at_ns = time.monotonic_ns()
        session.actual_outcome = actual_outcome
        session.record_event(
            "session_finished",
            status=status,
            actual_outcome=actual_outcome,
            error=error,
        )

    def _restore_after_intervention(
        self,
        session: InterventionSession,
        *,
        requested_outcome: Optional[str] = None,
        forced_state: Optional[CycleState] = None,
    ) -> None:
        context = self._intervention_context
        base_state = (
            forced_state
            if forced_state is not None
            else (
                CycleState.IDLE
                if session.entry_state == CycleState.IDLE.value
                else CycleState.STOPPED
            )
        )
        with self._lock:
            if context is not None:
                self._tree = context.tree
                self._node_statuses = dict(context.node_statuses)
                self._restore_blackboard_runtime_state(
                    stop_requested=context.stop_requested,
                    velocity_scale=context.velocity_scale,
                )
            self._state = base_state
            self._tick_thread = None
            self._tick_stop_event.clear()
            self._tick_pause_event.set()
            self._intervention_context = None

        if forced_state is not None:
            if session.actual_outcome is None:
                session.actual_outcome = forced_state.value
            return

        if requested_outcome == "resume" and session.entry_state == CycleState.STOPPED.value:
            self.run()
            session.actual_outcome = "resumed"
            session.record_event("resume_completed", state=self._state.value)
            return

        if requested_outcome == "reset":
            self.reset()
            session.actual_outcome = "reset"
            session.record_event("reset_completed", state=self._state.value)
            return

        if session.actual_outcome is None:
            session.actual_outcome = (
                "stay_idle"
                if session.entry_state == CycleState.IDLE.value
                else "stopped"
            )

    def execute_intervention(
        self,
        tree_dict: dict[str, Any],
        *,
        parent_run_id: Optional[str] = None,
        lease_timeout_ms: int = 60_000,
        resume_target: Optional[str] = None,
        session_kind: Optional[str] = None,
        authority_source: Optional[str] = None,
        principal_id: Optional[str] = None,
        grant_type: Optional[str] = None,
        trigger_reason: Optional[str] = None,
        trigger_node_instance: Optional[str] = None,
        approval_mode: Optional[str] = None,
        artifact_origin: str = "saved_program",
    ) -> InterventionSession:
        """Execute a temporary subtree as a direct-control intervention."""
        from manyforge_behavior.program import tree_node_from_dict

        if self._runner.catalog is None or self._runner.program is None:
            self._runner.load()

        allowed_session_kinds = {"manual_intervention", "delegated_recovery"}
        allowed_authority_sources = {"human", "llm", "program_node", "policy"}
        allowed_approval_modes = {"human_approved", "policy_granted", "not_required"}
        allowed_artifact_origins = {"saved_program", "runtime_override"}

        if lease_timeout_ms <= 0:
            raise RuntimeError("lease_timeout_ms must be > 0")

        # -- principal_id is required (may be a placeholder token in dev) --
        if not principal_id or not principal_id.strip():
            raise RuntimeError(
                "principal_id is required: identify the human operator or agent runtime"
            )
        principal_id = principal_id.strip()

        session_kind = session_kind or "manual_intervention"
        if session_kind not in allowed_session_kinds:
            raise RuntimeError(
                "session_kind must be one of: manual_intervention, delegated_recovery"
            )
        if authority_source not in allowed_authority_sources:
            raise RuntimeError(
                "authority_source is required and must be one of: human, llm, program_node, policy"
            )

        # -- grant_type enforcement (OP-4 scoped grants) --
        if grant_type is None:
            # Derive grant_type from authority_source for backward compat
            if authority_source == "human":
                grant_type = GrantType.HUMAN_APPROVED
            elif authority_source in {"llm", "program_node", "policy"}:
                grant_type = GrantType.POLICY_GRANTED
            else:
                grant_type = GrantType.HUMAN_APPROVED
        if grant_type not in GrantType.ACTIVE:
            raise RuntimeError(
                f"grant_type must be one of: {', '.join(sorted(GrantType.ACTIVE))}"
            )
        if grant_type == GrantType.HUMAN_APPROVED and authority_source != "human":
            raise RuntimeError(
                "grant_type=human_approved requires authority_source=human"
            )
        if grant_type == GrantType.POLICY_GRANTED and authority_source == "human":
            raise RuntimeError(
                "grant_type=policy_granted is not valid for authority_source=human"
            )

        # -- approval_mode (kept for backward compat, forced to match grant_type) --
        if approval_mode is None:
            approval_mode = grant_type  # human_approved or policy_granted
        elif approval_mode != grant_type:
            raise RuntimeError(
                f"approval_mode={approval_mode} contradicts grant_type={grant_type}; "
                "omit approval_mode to let it be derived from grant_type, "
                "or set it to match grant_type"
            )
        if approval_mode not in allowed_approval_modes:
            raise RuntimeError(
                "approval_mode is required and must be one of: human_approved, policy_granted, not_required"
            )
        if artifact_origin not in allowed_artifact_origins:
            raise RuntimeError(
                "artifact_origin must be one of: saved_program, runtime_override"
            )
        if session_kind == "manual_intervention" and authority_source not in {"human", "llm"}:
            raise RuntimeError(
                "manual_intervention authority_source must be human or llm"
            )
        if authority_source == "human" and approval_mode != "human_approved":
            raise RuntimeError("human authority_source requires approval_mode=human_approved")
        if authority_source == "policy" and approval_mode != "policy_granted":
            raise RuntimeError("policy authority_source requires approval_mode=policy_granted")
        if authority_source in {"llm", "program_node"} and approval_mode == "human_approved":
            raise RuntimeError(
                "approval_mode=human_approved is only valid for human authority_source"
            )
        if trigger_node_instance is not None and not trigger_reason:
            raise RuntimeError(
                "trigger_reason is required when trigger_node_instance is provided"
            )
        if authority_source == "program_node" and not trigger_node_instance:
            raise RuntimeError(
                "trigger_node_instance is required for authority_source=program_node"
            )
        if trigger_reason is None:
            trigger_reason = "manual_request"

        pending_request: Optional[DelegatedRecoveryRequest] = None
        thread_to_join = None
        with self._lock:
            if self._state not in (CycleState.IDLE, CycleState.STOPPED):
                raise RuntimeError(
                    f"Cannot start intervention from state {self._state.value}"
                )

            entry_state = self._state.value
            if (
                entry_state == CycleState.IDLE.value
                and (session_kind != "manual_intervention"
                     or authority_source != "human"
                     or approval_mode != "human_approved")
            ):
                raise RuntimeError(
                    "Entering DIRECT_CONTROL from IDLE requires explicit human-approved manual intervention"
                )
            pending_request = self._pending_delegated_recovery
            if session_kind == "delegated_recovery":
                if entry_state != CycleState.STOPPED.value:
                    raise RuntimeError(
                        "delegated_recovery may only start from STOPPED"
                    )
                if pending_request is None:
                    raise RuntimeError(
                        "No delegated recovery request is pending"
                    )
                if authority_source not in pending_request.allowed_authority_sources:
                    allowed = ", ".join(pending_request.allowed_authority_sources) or "none"
                    raise RuntimeError(
                        "delegated_recovery authority_source must be one of: "
                        f"{allowed}"
                    )
                if trigger_node_instance is not None and (
                    trigger_node_instance != pending_request.trigger_node_instance
                ):
                    raise RuntimeError(
                        "trigger_node_instance does not match the pending delegated recovery request"
                    )
                if trigger_reason is not None and (
                    trigger_reason != "manual_request"
                    and trigger_reason != pending_request.trigger_reason.value
                ):
                    raise RuntimeError(
                        "trigger_reason does not match the pending delegated recovery request"
                    )
                if resume_target is not None and (
                    resume_target not in pending_request.allowed_resume_targets
                ):
                    raise RuntimeError(
                        "resume_target is not allowed for the pending delegated recovery request"
                    )
            kernel_state = self._kernel.get_status().state
            if entry_state == CycleState.STOPPED.value and kernel_state == ExecutionState.PAUSED:
                raise RuntimeError(
                    "Cannot start intervention from STOPPED while the kernel holds a paused motion; reset the cycle first"
                )
            saved_context = _InterventionContext(
                tree=self._tree,
                node_statuses=dict(self._node_statuses),
                stop_requested=self._current_stop_requested(),
                velocity_scale=self._velocity_scale,
            )
            if self._tick_thread is not None and self._tick_thread.is_alive():
                self._tick_stop_event.set()
                thread_to_join = self._tick_thread

        if thread_to_join is not None:
            thread_to_join.join(timeout=2.0)
            if thread_to_join.is_alive():
                raise RuntimeError(
                    "Cannot start intervention: previous tick thread is still alive"
                )

        with self._lock:
            if self._tick_thread is thread_to_join:
                self._tick_thread = None
            default_resume_target = (
                "stay_idle"
                if entry_state == CycleState.IDLE.value
                else (
                    pending_request.default_resume_target
                    if session_kind == "delegated_recovery" and pending_request is not None
                    else "stop"
                )
            )
            session = InterventionSession(
                session_id=uuid.uuid4().hex[:12],
                session_kind=session_kind,
                authority_source=authority_source,
                principal_id=principal_id,
                grant_type=grant_type,
                parent_run_id=(
                    (
                        pending_request.parent_run_id
                        if session_kind == "delegated_recovery" and pending_request is not None
                        else parent_run_id
                    )
                    if entry_state == CycleState.STOPPED.value
                    else None
                ),
                entry_state=entry_state,
                lease_timeout_ms=lease_timeout_ms,
                lease_expires_at_ns=time.monotonic_ns() + (lease_timeout_ms * 1_000_000),
                resume_target=resume_target or default_resume_target,
                trigger_reason=(
                    pending_request.trigger_reason.value
                    if session_kind == "delegated_recovery" and pending_request is not None
                    else trigger_reason
                ),
                trigger_node_instance=(
                    pending_request.trigger_node_instance
                    if session_kind == "delegated_recovery" and pending_request is not None
                    else trigger_node_instance
                ),
                approval_mode=approval_mode,
                artifact_origin=artifact_origin,
            )
            session.record_event(
                "session_started",
                session_kind=session.session_kind,
                authority_source=session.authority_source,
                principal_id=session.principal_id,
                grant_type=session.grant_type,
                entry_state=entry_state,
                parent_run_id=session.parent_run_id,
                lease_timeout_ms=lease_timeout_ms,
                resume_target=session.resume_target,
                trigger_reason=session.trigger_reason,
                trigger_node_instance=session.trigger_node_instance,
                approval_mode=session.approval_mode,
                artifact_origin=session.artifact_origin,
            )
            self._intervention = session
            self._intervention_context = saved_context
            self._error = None
            if session_kind == "delegated_recovery":
                self._pending_delegated_recovery = None

        rewritten_tree = self._rewrite_intervention_tree(tree_dict, session.session_id)

        # Set intervention context on the runner so nodes like QueryModelNode
        # attribute audit events to this session, not to the base program run.
        self._runner._intervention_context = {
            "session_id": session.session_id,
            "principal_id": session.principal_id,
            "run_id": f"intervention-{session.session_id}",
            "parent_run_id": session.parent_run_id,
        }
        try:
            tree_node = tree_node_from_dict(rewritten_tree)
            root = self._runner._build_node(tree_node)
            tree = py_trees.trees.BehaviourTree(root=root)
            tree.setup()
        except Exception as e:
            self._finish_intervention(
                session,
                status="failed",
                actual_outcome="build_failed",
                error=str(e),
            )
            self._restore_after_intervention(session)
            raise RuntimeError(f"Failed to build intervention tree: {e}") from e
        finally:
            self._runner._intervention_context = None

        self._write_bb_keys()

        with self._lock:
            self._tree = tree
            self._start_tick_thread(lambda: self._intervention_tick_loop(session))
            self._state = CycleState.DIRECT_CONTROL
        session.record_event("tree_started")

        return session

    def _intervention_tick_loop(self, session: InterventionSession) -> None:
        """Tick loop for intervention subtrees. Same cadence as _tick_loop."""
        period_s = 0.010

        while not self._tick_stop_event.is_set():
            if time.monotonic_ns() >= session.lease_expires_at_ns:
                logger.warning("Intervention session %s lease expired", session.session_id)
                self._tick_stop_event.set()
                self._finish_intervention(
                    session,
                    status="expired",
                    actual_outcome="stopped",
                    error="Intervention lease expired",
                )
                self._restore_after_intervention(
                    session,
                    forced_state=CycleState.STOPPED,
                )
                return

            if not self._tick_pause_event.wait(timeout=0.001):
                continue

            start_time = time.monotonic()
            try:
                self._tree.tick()
            except Exception:
                error_msg = traceback.format_exc()
                logger.error("Intervention tick exception: %s", error_msg)
                self._tick_stop_event.set()
                with self._lock:
                    self._error = error_msg
                self._finish_intervention(
                    session,
                    status="failed",
                    error=error_msg,
                )
                self._restore_after_intervention(session)
                return

            self._drain_kernel_events()

            statuses: dict[str, str] = {}
            _collect_statuses(self._tree.root, statuses)
            with self._lock:
                self._node_statuses = statuses

            status = self._tree.root.status
            if status in (py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE):
                self._tick_stop_event.set()
                if status == py_trees.common.Status.SUCCESS:
                    self._finish_intervention(
                        session,
                        status="completed",
                    )
                    self._restore_after_intervention(
                        session,
                        requested_outcome=session.resume_target,
                    )
                else:
                    self._finish_intervention(
                        session,
                        status="failed",
                        error="Intervention subtree returned FAILURE",
                    )
                    self._restore_after_intervention(session)
                return

            elapsed = time.monotonic() - start_time
            sleep_time = period_s - elapsed
            if sleep_time > 0:
                # Audit 6D (finding 2.9): interruptible slice — a stop
                # request wakes the loop immediately instead of waiting out
                # the remainder of the tick period.
                self._tick_stop_event.wait(timeout=sleep_time)

        if session.status == "running":
            self._finish_intervention(
                session,
                status="aborted",
            )
            self._restore_after_intervention(session)

    def abort_intervention(self) -> None:
        """Abort a running intervention and restore the saved parent context."""
        with self._lock:
            if self._state != CycleState.DIRECT_CONTROL:
                return
            self._tick_stop_event.set()
            thread_to_join = self._tick_thread

        if thread_to_join is not None:
            thread_to_join.join(timeout=2.0)

        with self._lock:
            if thread_to_join is not None and thread_to_join.is_alive():
                logger.error("Intervention tick thread did not terminate — zombie detected")
                self._error = "intervention tick thread did not terminate"

    def _write_bb_keys(self) -> None:
        """Write stop_requested=False and velocity_scale to blackboard."""
        bb = py_trees.blackboard.Client(name="cycle_manager_init")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.stop_requested = False
        bb.velocity_scale = self._velocity_scale
