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
    INTERRUPT_HANDLER = "interrupt_handler"
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
    """Closed enum of recovery trigger reasons (OP-6).

    Replaces ad-hoc string trigger reasons with a typed, bounded taxonomy.
    """
    PLANNING_FAILED = "planning_failed"
    VALIDATION_FAILED = "validation_failed"
    EXECUTE_FAILED = "execute_failed"
    TIMEOUT = "timeout"
    SCENE_MISMATCH = "scene_mismatch"
    NODE_FAILURE = "node_failure"
    PRECONDITION_FAILED = "precondition_failed"


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


@dataclass
class PendingInterrupt:
    """Latched interrupt request awaiting explicit operator/runtime handling."""

    interrupt_id: str
    source_id: str
    source_adapter: str
    signal: str
    kind: str
    priority: int
    maskable: bool
    policy_id: Optional[str]
    handler_id: Optional[str]
    preemption_mode: str
    after_handler: str
    active_node_name: Optional[str]
    active_node_id: Optional[str]
    active_move_id: Optional[str]
    latched_at_ns: int = field(default_factory=time.monotonic_ns)
    details: dict[str, Any] = field(default_factory=dict)


@dataclass
class ActiveInterruptHandler:
    """Runtime execution state for a compiled interrupt handler subtree."""

    interrupt_id: str
    policy_id: Optional[str]
    handler_id: str
    tree: py_trees.trees.BehaviourTree
    resource_claims: tuple[str, ...]
    timeout_s: Optional[float] = None
    started_at_ns: int = field(default_factory=time.monotonic_ns)
    deadline_at_ns: Optional[int] = None
    tick_count: int = 0
    last_status: str = py_trees.common.Status.INVALID.value
    completed_at_ns: Optional[int] = None
    last_error: Optional[str] = None


_INTERRUPT_CONTEXT_PREFIX = "_interrupt/current"
_SUPPORTED_AUTO_INTERRUPT_RESOURCE_CLAIMS = {
    "blackboard",
    "gripper",
    "model",
    "motion",
    "planner",
    "scene",
    "signals",
    "timers",
}


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
    
    State transitions:
        IDLE ──(run)──► RUNNING ──(stop)──► STOPPED ──(run)──► RUNNING
        Any state ──(reset)──► IDLE
    
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
        self._node_statuses: dict[str, str] = {}
        self._intervention: Optional[InterventionSession] = None
        self._intervention_context: Optional[_InterventionContext] = None
        self._pending_delegated_recovery: Optional[DelegatedRecoveryRequest] = None
        self._pending_interrupt: Optional[PendingInterrupt] = None
        self._active_interrupt_handler: Optional[ActiveInterruptHandler] = None
        self._deferred_interrupts: deque[PendingInterrupt] = deque()
        self._on_delegated_recovery_requested: Optional[
            Callable[[DelegatedRecoveryRequest], None]
        ] = None
        self._on_recovery_exhausted: Optional[
            Callable[[str, str, str], None]
        ] = None
        self._on_interrupt_event: Optional[Callable[..., None]] = None
        self._parent_run_id_provider: Optional[Callable[[], Optional[str]]] = None
        self._kernel_events: deque[dict[str, Any]] = deque(maxlen=4096)
        self._kernel_event_index: int = 0
        self._interrupt_source_levels: dict[str, bool] = {}
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
        """Best-effort removal of program-scoped scene objects from the backend."""
        program = self._runner.program
        if program is None:
            return
        for resource in program.scene:
            self._kernel.scene_edit(
                SceneEditRequest(operation="remove", object_id=resource.object_id)
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
            self._pending_interrupt = None
            self._active_interrupt_handler = None
            self._deferred_interrupts.clear()
            self._interrupt_source_levels = {}
        self._clear_interrupt_context()
    
    @property
    def state(self) -> CycleState:
        """Current lifecycle state."""
        return self._state
    
    @property
    def velocity_scale(self) -> float:
        """Current velocity scale (0.0 to 1.0)."""
        return self._velocity_scale

    @property
    def error(self) -> Optional[str]:
        """Last tick-loop exception or surfaced terminal node failure, or None."""
        return self._error

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
        Direct surfaces such as the streaming-target WebSocket bypass that loop,
        so API routes and websocket status broadcasts call this explicit drain
        before reading the buffered event stream.
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
        """
        self._quiesce_runtime(
            client_name="cycle_manager_motion_paused",
            kernel_action="pause_if_executing",
        )
        with self._lock:
            self._error = error_message
            self._state = CycleState.STOPPED

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
        """Enter a quiesced runtime mode shared by stop and interrupt preemption."""
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

    def _publish_interrupt_context(self, pending: PendingInterrupt) -> None:
        """Publish the current interrupt context as a bundle on the blackboard."""
        bb = py_trees.blackboard.Client(name="cycle_manager_interrupt_context")
        bb.register_key(key=_INTERRUPT_CONTEXT_PREFIX, access=py_trees.common.Access.WRITE)
        write_namespace_bundle(
            bb,
            _INTERRUPT_CONTEXT_PREFIX,
            value={
                "interrupt_id": pending.interrupt_id,
                "source_id": pending.source_id,
                "source_adapter": pending.source_adapter,
                "signal": pending.signal,
                "kind": pending.kind,
                "priority": pending.priority,
                "maskable": pending.maskable,
                "policy_id": pending.policy_id,
                "handler_id": pending.handler_id,
                "preemption_mode": pending.preemption_mode,
                "after_handler": pending.after_handler,
                "active_node_name": pending.active_node_name,
                "active_node_id": pending.active_node_id,
                "active_move_id": pending.active_move_id,
                "latched_at_ns": pending.latched_at_ns,
                "details": dict(pending.details),
            },
            status=str(pending.details.get("handler_state", "latched")),
            interrupt_id=pending.interrupt_id,
            source_id=pending.source_id,
            policy_id=pending.policy_id,
            handler_id=pending.handler_id,
            preemption_mode=pending.preemption_mode,
            disposition=pending.after_handler,
        )

    def _clear_interrupt_context(self) -> None:
        """Remove the active interrupt context bundle from the blackboard."""
        py_trees.blackboard.Blackboard.unset(_INTERRUPT_CONTEXT_PREFIX)

    def _cleanup_interrupt_handler_tree(
        self,
        active: Optional[ActiveInterruptHandler],
    ) -> None:
        """Best-effort invalidation of an active handler tree on forced exit."""
        if active is None:
            return
        try:
            if active.tree.root.status == py_trees.common.Status.RUNNING:
                active.tree.root.stop(py_trees.common.Status.INVALID)
        except Exception:
            logger.exception("Failed to stop active interrupt handler tree cleanly")

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

    def set_on_interrupt_event(
        self,
        callback: Optional[Callable[..., None]],
    ) -> None:
        """Install a callback invoked when runtime interrupt lifecycle events occur."""
        self._on_interrupt_event = callback

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
        If INTERRUPT_HANDLER: reject resume while a handler is still executing.
        If RUNNING: no-op.
        """
        prepare_fresh = False
        start_from_idle = False
        on_run_from_idle: Optional[Callable[[], None]] = None
        clear_resumable_interrupt = False
        resumed_from_stopped = False

        with self._lock:
            if self._state == CycleState.RUNNING:
                return  # Already running, no-op

            self._error = None

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
                pending_interrupt = self._pending_interrupt
                if pending_interrupt is not None:
                    handler_state = str(
                        pending_interrupt.details.get("handler_state", "latched")
                    )
                    if (
                        pending_interrupt.after_handler != "resumable_hold"
                        or handler_state != "completed"
                    ):
                        raise RuntimeError(
                            "Cannot resume cycle while interrupt "
                            f"'{pending_interrupt.policy_id or pending_interrupt.source_id}' "
                            "requires reset or explicit handler execution"
                        )
                    clear_resumable_interrupt = True
                # Resuming from stopped state
                self._pending_delegated_recovery = None
                bb = py_trees.blackboard.Client(name="cycle_manager_resume")
                bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
                bb.stop_requested = False

                # If we're resuming from a motion soft-fail (the cycle
                # was put into STOPPED by _enter_motion_paused_state),
                # clear the soft-fail blackboard keys and set the resume
                # signal so the holding motion node retries its goal on
                # the next tick rather than re-emitting the same error.
                if self._read_motion_paused_error() is not None:
                    self._clear_motion_paused_blackboard()
                    self._set_resume_after_error_flag(True)

                kernel_status = self._kernel.get_status()
                if kernel_status.state == ExecutionState.PAUSED:
                    self._kernel.resume()

                self._tick_pause_event.set()
                if self._tick_thread is None or not self._tick_thread.is_alive():
                    if self._tree is None:
                        raise RuntimeError("Cannot resume stopped cycle without a tree")
                    self._start_tick_thread(self._tick_loop)
                self._state = CycleState.RUNNING
                resumed_from_stopped = True
            elif self._state == CycleState.INTERRUPT_HANDLER:
                raise RuntimeError("Cannot resume cycle while an interrupt handler is executing")

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
        elif resumed_from_stopped and clear_resumable_interrupt:
            self._clear_pending_interrupt(
                action="cleared",
                reason="operator_resume",
            )
    
    def stop(self) -> None:
        """
        Stop program execution.
        
        If RUNNING: set stop_requested=True on BB, suspend tick thread.
                    If kernel state is EXECUTING, call kernel.pause().
                    State → STOPPED.
        If INTERRUPT_HANDLER, STOPPED or IDLE: no-op.
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
                    self._state = CycleState.IDLE
                    self._pending_delegated_recovery = None
                    self._pending_interrupt = None
                    self._active_interrupt_handler = None
                    self._deferred_interrupts.clear()
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
                self._clear_interrupt_context()
                return
            with self._lock:
                if self._tick_thread is thread_to_join:
                    self._tick_thread = None

        kernel_status = self._kernel.get_status()
        if kernel_status.state != ExecutionState.IDLE:
            self._kernel.reset()

        self._prepare_fresh_cycle()

        clear_pending_interrupt = False
        active_interrupt_handler = None
        with self._lock:
            self._tick_stop_event.clear()
            self._tick_pause_event.set()
            self._error = None
            self._state = CycleState.IDLE
            self._pending_delegated_recovery = None
            clear_pending_interrupt = self._pending_interrupt is not None
            active_interrupt_handler = self._active_interrupt_handler
            self._active_interrupt_handler = None
            self._deferred_interrupts.clear()
            self._interrupt_source_levels = {}
        self._cleanup_interrupt_handler_tree(active_interrupt_handler)
        if clear_pending_interrupt:
            self._clear_pending_interrupt(action="cleared", reason="cycle_reset")
        else:
            self._clear_interrupt_context()
    
    def set_velocity_scale(self, scale: float) -> None:
        """Set velocity_scale on blackboard. Clamped to [0.0, 1.0].

        Rejects NaN and infinity — these would propagate to the robot.
        """
        import math
        if not isinstance(scale, (int, float)) or math.isnan(scale) or math.isinf(scale):
            raise ValueError(f"velocity_scale must be a finite number, got {scale!r}")
        scale = max(0.0, min(1.0, float(scale)))
        self._velocity_scale = scale
        
        bb = py_trees.blackboard.Client(name="cycle_manager_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.velocity_scale = scale

    def _try_activate_interrupt_handler(
        self,
        pending: PendingInterrupt,
    ) -> bool:
        """Compile and activate a named interrupt handler when safe to do so."""
        program = self._runner.program
        if program is None or pending.handler_id is None:
            return False
        handler = program.interrupt_handlers.get(pending.handler_id)
        if handler is None:
            pending.details["handler_state"] = "blocked"
            pending.details["handler_blocked_reason"] = "missing_handler_definition"
            self._publish_interrupt_context(pending)
            self._notify_interrupt_event(
                action="handler_auto_execution_blocked",
                interrupt_id=pending.interrupt_id,
                source=pending.source_id,
                policy_id=pending.policy_id,
                preemption_mode=pending.preemption_mode,
                disposition=pending.after_handler,
                active_node_id=pending.active_node_id,
                active_move_id=pending.active_move_id,
                details=dict(pending.details),
            )
            return False

        resource_claims = tuple(sorted(handler.resource_claims))
        pending.details["handler_resource_claims"] = list(resource_claims)
        if handler.timeout_s is not None:
            pending.details["handler_timeout_s"] = handler.timeout_s
        unsupported_claims = sorted(
            set(resource_claims) - _SUPPORTED_AUTO_INTERRUPT_RESOURCE_CLAIMS
        )
        if unsupported_claims:
            pending.details["handler_state"] = "blocked"
            pending.details["handler_blocked_reason"] = (
                "unsupported_auto_execution_resource_claims"
            )
            pending.details["unsupported_resource_claims"] = unsupported_claims
            self._publish_interrupt_context(pending)
            self._notify_interrupt_event(
                action="handler_auto_execution_blocked",
                interrupt_id=pending.interrupt_id,
                source=pending.source_id,
                policy_id=pending.policy_id,
                preemption_mode=pending.preemption_mode,
                disposition=pending.after_handler,
                active_node_id=pending.active_node_id,
                active_move_id=pending.active_move_id,
                details=dict(pending.details),
            )
            return False

        preparation_error = self._prepare_interrupt_handler_runtime(
            pending=pending,
            resource_claims=resource_claims,
        )
        if preparation_error is not None:
            pending.details["handler_state"] = "blocked"
            pending.details["handler_blocked_reason"] = preparation_error
            self._set_stop_requested_flag(
                True,
                client_name="cycle_manager_interrupt_handler_restore_stop_flag",
            )
            self._publish_interrupt_context(pending)
            self._notify_interrupt_event(
                action="handler_auto_execution_blocked",
                interrupt_id=pending.interrupt_id,
                source=pending.source_id,
                policy_id=pending.policy_id,
                preemption_mode=pending.preemption_mode,
                disposition=pending.after_handler,
                active_node_id=pending.active_node_id,
                active_move_id=pending.active_move_id,
                details=dict(pending.details),
            )
            return False

        try:
            handler_tree = self._runner.build_interrupt_handler_tree(pending.handler_id)
        except Exception as exc:
            pending.details["handler_state"] = "blocked"
            pending.details["handler_blocked_reason"] = "handler_build_failed"
            pending.details["handler_last_error"] = str(exc)
            self._set_stop_requested_flag(
                True,
                client_name="cycle_manager_interrupt_handler_restore_stop_flag",
            )
            self._publish_interrupt_context(pending)
            self._notify_interrupt_event(
                action="handler_auto_execution_blocked",
                interrupt_id=pending.interrupt_id,
                source=pending.source_id,
                policy_id=pending.policy_id,
                preemption_mode=pending.preemption_mode,
                disposition=pending.after_handler,
                active_node_id=pending.active_node_id,
                active_move_id=pending.active_move_id,
                details=dict(pending.details),
            )
            return False

        active = ActiveInterruptHandler(
            interrupt_id=pending.interrupt_id,
            policy_id=pending.policy_id,
            handler_id=pending.handler_id,
            tree=handler_tree,
            resource_claims=resource_claims,
            timeout_s=handler.timeout_s,
        )
        if active.timeout_s is not None:
            active.deadline_at_ns = active.started_at_ns + int(active.timeout_s * 1_000_000_000)
        pending.details["handler_state"] = "running"
        pending.details["handler_started_at_ns"] = active.started_at_ns
        pending.details["handler_start_latency_ns"] = max(
            0,
            active.started_at_ns - pending.latched_at_ns,
        )
        quiesced_at_ns = pending.details.get("quiesce_completed_at_ns")
        if isinstance(quiesced_at_ns, int):
            pending.details["handler_post_quiesce_latency_ns"] = max(
                0,
                active.started_at_ns - quiesced_at_ns,
            )
        if active.deadline_at_ns is not None:
            pending.details["handler_deadline_at_ns"] = active.deadline_at_ns
        self._publish_interrupt_context(pending)
        with self._lock:
            self._active_interrupt_handler = active
            self._state = CycleState.INTERRUPT_HANDLER
        self._tick_pause_event.set()
        self._notify_interrupt_event(
            action="handler_started",
            interrupt_id=pending.interrupt_id,
            source=pending.source_id,
            policy_id=pending.policy_id,
            preemption_mode=pending.preemption_mode,
            disposition=pending.after_handler,
            active_node_id=pending.active_node_id,
            active_move_id=pending.active_move_id,
            details=dict(pending.details),
        )
        return True

    def _prepare_interrupt_handler_runtime(
        self,
        *,
        pending: PendingInterrupt,
        resource_claims: tuple[str, ...],
    ) -> Optional[str]:
        self._set_stop_requested_flag(
            False,
            client_name="cycle_manager_interrupt_handler_enable_execution",
        )
        pending.details["handler_stop_requested_cleared"] = True

        if "motion" not in resource_claims:
            return None

        pending.details["handler_kernel_preparation"] = "stop_and_reset_to_idle"
        status = self._kernel.get_status().state
        if status in {
            ExecutionState.EXECUTING,
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }:
            stop_result = self._kernel.stop()
            if not stop_result.accepted:
                return f"kernel_stop_failed: {stop_result.error or 'rejected'}"
            status = self._kernel.get_status().state

        if status != ExecutionState.IDLE:
            reset_result = self._kernel.reset()
            if not reset_result.accepted and self._kernel.get_status().state != ExecutionState.IDLE:
                return f"kernel_reset_failed: {reset_result.error or 'rejected'}"

        pending.details["handler_kernel_ready_state"] = self._kernel.get_status().state.value
        return None

    def _restart_interrupted_subtree(self, pending: PendingInterrupt) -> Optional[str]:
        if self._tree is None or self._tree.root is None:
            return "missing_runtime_tree"

        target_name = None
        program = self._runner.program
        if program is not None and pending.policy_id is not None:
            policy = program.interrupt_policies.get(pending.policy_id)
            if policy is not None:
                target_name = str(policy.applies_to.get("subtree_root") or "").strip() or None
        if target_name is None:
            target_name = pending.active_node_name
        if not target_name:
            return "missing_restart_target"

        target_node = next(
            (node for node in self._iter_runtime_tree_nodes(self._tree.root) if node.name == target_name),
            None,
        )
        if target_node is None:
            return f"restart_target_not_found:{target_name}"

        status = self._kernel.get_status().state
        if status in {
            ExecutionState.EXECUTING,
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }:
            stop_result = self._kernel.stop()
            if not stop_result.accepted:
                return f"kernel_stop_failed: {stop_result.error or 'rejected'}"
            status = self._kernel.get_status().state
        if status != ExecutionState.IDLE:
            reset_result = self._kernel.reset()
            if not reset_result.accepted and self._kernel.get_status().state != ExecutionState.IDLE:
                return f"kernel_reset_failed: {reset_result.error or 'rejected'}"

        target_node.stop(py_trees.common.Status.INVALID)
        self._set_stop_requested_flag(
            False,
            client_name="cycle_manager_interrupt_restart_subtree_resume",
        )
        with self._lock:
            self._state = CycleState.RUNNING
            self._active_interrupt_handler = None
        self._tick_pause_event.set()
        pending.details["restart_target_name"] = target_name
        pending.details["restart_completed_at_ns"] = time.monotonic_ns()
        pending.details["restart_completion_latency_ns"] = max(
            0,
            pending.details["restart_completed_at_ns"] - pending.latched_at_ns,
        )
        return None

    def _queue_deferred_interrupt(self, pending: PendingInterrupt) -> None:
        signature = (
            pending.source_id,
            pending.policy_id,
            pending.active_node_name,
            pending.signal,
        )
        existing = {
            (queued.source_id, queued.policy_id, queued.active_node_name, queued.signal)
            for queued in self._deferred_interrupts
        }
        if signature in existing:
            return
        pending.details["deferred"] = True
        pending.details["deferred_at_ns"] = time.monotonic_ns()
        self._deferred_interrupts.append(pending)
        self._notify_interrupt_event(
            action="deferred",
            interrupt_id=pending.interrupt_id,
            source=pending.source_id,
            policy_id=pending.policy_id,
            preemption_mode=pending.preemption_mode,
            disposition=pending.after_handler,
            active_node_id=pending.active_node_id,
            active_move_id=pending.active_move_id,
            details=dict(pending.details),
        )

    def _pop_deferred_interrupt(self) -> Optional[PendingInterrupt]:
        if not self._deferred_interrupts:
            return None
        best_index = 0
        best_pending = self._deferred_interrupts[0]
        for index, candidate in enumerate(self._deferred_interrupts):
            if (
                candidate.priority > best_pending.priority
                or (
                    candidate.priority == best_pending.priority
                    and candidate.latched_at_ns < best_pending.latched_at_ns
                )
            ):
                best_index = index
                best_pending = candidate
        self._deferred_interrupts.rotate(-best_index)
        selected = self._deferred_interrupts.popleft()
        self._deferred_interrupts.rotate(best_index)
        selected.details.pop("deferred", None)
        selected.details["deferred_consumed_at_ns"] = time.monotonic_ns()
        selected.details["deferred_wait_ns"] = max(
            0,
            selected.details["deferred_consumed_at_ns"] - selected.latched_at_ns,
        )
        return selected

    def _apply_interrupt_disposition(
        self,
        pending: PendingInterrupt,
        *,
        handler_failed: bool = False,
        error: Optional[str] = None,
    ) -> None:
        """Apply post-handler disposition conservatively and audibly."""
        disposition = pending.after_handler
        if handler_failed:
            disposition = "failed"
        pending.details["handler_state"] = "failed" if handler_failed else "completed"
        if error is not None:
            pending.details["handler_last_error"] = error
        pending.details["handler_completed_at_ns"] = time.monotonic_ns()
        handler_started_at_ns = pending.details.get("handler_started_at_ns")
        if isinstance(handler_started_at_ns, int):
            pending.details["handler_duration_ns"] = max(
                0,
                pending.details["handler_completed_at_ns"] - handler_started_at_ns,
            )
        pending.details["handler_completion_latency_ns"] = max(
            0,
            pending.details["handler_completed_at_ns"] - pending.latched_at_ns,
        )
        self._publish_interrupt_context(pending)

        auto_resume = (
            not handler_failed
            and disposition in {"resume_node", "resume_subtree"}
            and pending.preemption_mode == "hold_cycle"
        )
        if auto_resume:
            bb = py_trees.blackboard.Client(name="cycle_manager_interrupt_resume")
            bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
            bb.stop_requested = False
            kernel_status = self._kernel.get_status()
            if kernel_status.state == ExecutionState.PAUSED:
                self._kernel.resume()
            with self._lock:
                self._state = CycleState.RUNNING
                self._active_interrupt_handler = None
            self._notify_interrupt_event(
                action="handler_completed",
                interrupt_id=pending.interrupt_id,
                source=pending.source_id,
                policy_id=pending.policy_id,
                preemption_mode=pending.preemption_mode,
                disposition=disposition,
                active_node_id=pending.active_node_id,
                active_move_id=pending.active_move_id,
                details=dict(pending.details),
            )
            self._clear_pending_interrupt(action="cleared", reason="handler_auto_resume")
            return

        if not handler_failed and disposition == "restart_subtree":
            restart_error = self._restart_interrupted_subtree(pending)
            if restart_error is None:
                self._notify_interrupt_event(
                    action="handler_completed",
                    interrupt_id=pending.interrupt_id,
                    source=pending.source_id,
                    policy_id=pending.policy_id,
                    preemption_mode=pending.preemption_mode,
                    disposition=disposition,
                    active_node_id=pending.active_node_id,
                    active_move_id=pending.active_move_id,
                    details=dict(pending.details),
                )
                self._clear_pending_interrupt(
                    action="cleared",
                    reason="handler_restart_subtree",
                )
                return
            handler_failed = True
            disposition = "failed"
            pending.details["handler_failure_reason"] = "restart_subtree_failed"
            pending.details["handler_last_error"] = restart_error
            error = restart_error

        if not handler_failed and disposition == "resumable_hold":
            next_state = CycleState.STOPPED
            reason = "handler_completed_resumable_hold"
        else:
            next_state = CycleState.STOPPED
            reason = (
                "handler_failed"
                if handler_failed
                else f"handler_completed_{disposition}"
            )

        if handler_failed or disposition in {"failed", "escalate"}:
            self._error = error or f"Interrupt handler '{pending.handler_id}' failed"

        self._set_stop_requested_flag(
            True,
            client_name="cycle_manager_interrupt_handler_restore_stop_flag",
        )
        with self._lock:
            self._state = next_state
            active = self._active_interrupt_handler
            self._active_interrupt_handler = None
        self._cleanup_interrupt_handler_tree(active)
        self._tick_pause_event.clear()
        self._notify_interrupt_event(
            action="handler_completed" if not handler_failed else "handler_failed",
            interrupt_id=pending.interrupt_id,
            source=pending.source_id,
            policy_id=pending.policy_id,
            preemption_mode=pending.preemption_mode,
            disposition=disposition,
            active_node_id=pending.active_node_id,
            active_move_id=pending.active_move_id,
            details={"reason": reason} | dict(pending.details),
        )

    def _tick_interrupt_handler(self) -> None:
        """Tick the active interrupt handler subtree once."""
        with self._lock:
            active = self._active_interrupt_handler
            pending = self._pending_interrupt
        if active is None or pending is None:
            with self._lock:
                active = self._active_interrupt_handler
                self._active_interrupt_handler = None
                self._state = CycleState.STOPPED
            self._cleanup_interrupt_handler_tree(active)
            self._tick_pause_event.clear()
            return

        try:
            active.tree.tick()
        except Exception:
            error_msg = traceback.format_exc()
            active.last_error = error_msg
            active.completed_at_ns = time.monotonic_ns()
            pending.details["handler_last_error"] = error_msg
            self._apply_interrupt_disposition(
                pending,
                handler_failed=True,
                error=error_msg,
            )
            return

        active.tick_count += 1
        active.last_status = (
            active.tree.root.status.value
            if active.tree.root.status is not None
            else py_trees.common.Status.INVALID.value
        )
        pending.details["handler_tick_count"] = active.tick_count
        pending.details["handler_root_status"] = active.last_status
        pending.details["handler_elapsed_ns"] = time.monotonic_ns() - active.started_at_ns
        self._publish_interrupt_context(pending)

        status = active.tree.root.status
        if status == py_trees.common.Status.RUNNING:
            if (
                active.deadline_at_ns is not None
                and time.monotonic_ns() >= active.deadline_at_ns
            ):
                active.completed_at_ns = time.monotonic_ns()
                pending.details["handler_failure_reason"] = "timeout"
                pending.details["handler_timed_out_at_ns"] = active.completed_at_ns
                error_msg = (
                    f"Interrupt handler '{active.handler_id}' timed out after "
                    f"{active.timeout_s:.3f}s"
                )
                active.last_error = error_msg
                pending.details["handler_last_error"] = error_msg
                self._apply_interrupt_disposition(
                    pending,
                    handler_failed=True,
                    error=error_msg,
                )
                return
            return

        active.completed_at_ns = time.monotonic_ns()
        self._apply_interrupt_disposition(pending)

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
            with self._lock:
                current_state = self._state
            if current_state == CycleState.INTERRUPT_HANDLER:
                kernel_events = self._drain_kernel_events()
                deferred = self._match_interrupt_policy(self._node_statuses, kernel_events)
                if deferred is not None:
                    self._queue_deferred_interrupt(deferred)
                start_time = time.monotonic()
                self._tick_interrupt_handler()
                elapsed = time.monotonic() - start_time
                sleep_time = period_s - elapsed
                if sleep_time > 0:
                    time.sleep(sleep_time)
                continue

            kernel_events = self._drain_kernel_events()
            pending = self._match_interrupt_policy(self._node_statuses, kernel_events)
            if pending is not None:
                self._apply_interrupt_preemption(pending)
                continue
            
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

            pending = self._match_interrupt_policy(statuses, kernel_events)
            if pending is not None:
                self._apply_interrupt_preemption(pending)
                continue

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
                            with self._lock:
                                self._error = failure_feedback
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
                        with self._lock:
                            self._error = failure_feedback
                            self._pending_delegated_recovery = request
                            self._state = CycleState.STOPPED
                            self._tick_thread = None
                        self._notify_delegated_recovery_requested(request)
                        break
                with self._lock:
                    self._error = failure_feedback if status == py_trees.common.Status.FAILURE else None
                    self._state = CycleState.IDLE
                    self._tick_thread = None
                break
            
            # Sleep remainder of period
            elapsed = time.monotonic() - start_time
            sleep_time = period_s - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

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

    @property
    def pending_interrupt(self) -> Optional[PendingInterrupt]:
        """Current latched interrupt waiting for reset/resume/handler execution."""
        return self._pending_interrupt

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

    def _notify_interrupt_event(self, **payload: Any) -> None:
        """Best-effort callback for interrupt lifecycle events."""
        if self._on_interrupt_event is None:
            return
        try:
            self._on_interrupt_event(**payload)
        except Exception:
            logger.exception("interrupt callback failed")

    def _read_interrupt_source_active(
        self,
        source,
        kernel_events: list[dict[str, Any]],
    ) -> tuple[bool, dict[str, Any]]:
        """Resolve an interrupt source into an active/inactive signal."""
        if source.adapter == "kernel":
            for event in kernel_events:
                if event.get("type") == source.signal:
                    return True, {"kernel_event": dict(event)}
            return False, {}

        registry_record = self._signal_registry.get_signal(
            source.adapter,
            source.signal,
            now_ns=time.monotonic_ns(),
        )
        if registry_record is not None:
            details = {
                "registry_signal": registry_record.signal,
                "registry_adapter": registry_record.adapter,
                "registry_provider": registry_record.provider_id,
                "registry_freshness": registry_record.freshness,
                "mirror_key": registry_record.mirror_key,
                "observed_at_ns": registry_record.observed_at_ns,
            }
            if registry_record.metadata:
                details["metadata"] = dict(registry_record.metadata)
            if registry_record.freshness != "fresh":
                return False, details
            return bool(registry_record.value), details

        runtime_key = self._normalize_blackboard_key(source.signal)
        try:
            value = py_trees.blackboard.Blackboard.get(runtime_key)
        except KeyError:
            return False, {"runtime_key": runtime_key, "present": False}
        return bool(value), {"runtime_key": runtime_key, "present": True}

    def _active_program_nodes(self, statuses: dict[str, str]) -> list[Any]:
        """Return currently running program tree nodes in deterministic order."""
        program = self._runner.program
        if program is None or program.tree is None:
            return []
        return [
            node
            for node in _iter_tree_nodes(program.tree)
            if statuses.get(node.name) == py_trees.common.Status.RUNNING.value
        ]

    def _policy_matches_node(self, policy, node) -> bool:
        applies_to = policy.applies_to or {}
        if not applies_to:
            return True

        if "subtree_root" in applies_to:
            root_name = applies_to["subtree_root"]
            program = self._runner.program
            if program is None or program.tree is None:
                return False
            matching_root = next(
                (candidate for candidate in _iter_tree_nodes(program.tree) if candidate.name == root_name),
                None,
            )
            if matching_root is None:
                return False
            subtree_names = {candidate.name for candidate in _iter_tree_nodes(matching_root)}
            if node.name not in subtree_names:
                return False

        node_ids = set(applies_to.get("node_ids", []))
        if node_ids and node.id not in node_ids:
            return False

        node_kinds = set(applies_to.get("node_kinds", []))
        if node_kinds and node.id not in node_kinds:
            return False

        node_names = set(applies_to.get("node_names", []))
        if node_names and node.name not in node_names:
            return False

        if applies_to.get("interruptible_only"):
            catalog = self._runner.catalog
            if catalog is None:
                return False
            try:
                entry = catalog.get(node.id)
            except KeyError:
                return False
            if not entry.interruptible:
                return False

        return True

    def _match_interrupt_policy(
        self,
        statuses: dict[str, str],
        kernel_events: list[dict[str, Any]],
    ) -> Optional[PendingInterrupt]:
        """Return the highest-priority interrupt that should be latched now."""
        deployment = self._runner.deployment
        program = self._runner.program
        if (
            deployment is None
            or program is None
            or not deployment.interrupt_sources
        ):
            return None
        if self._pending_interrupt is None:
            deferred = self._pop_deferred_interrupt()
            if deferred is not None:
                return deferred

        active_nodes = self._active_program_nodes(statuses)
        source_candidates: list[tuple[int, str, Any, dict[str, Any]]] = []
        for source_id, source in deployment.interrupt_sources.items():
            active, details = self._read_interrupt_source_active(source, kernel_events)
            previous = self._interrupt_source_levels.get(source_id, False)
            self._interrupt_source_levels[source_id] = active
            if not active:
                continue
            if source.kind == "edge" and previous:
                continue
            source_candidates.append((source.priority, source_id, source, details))

        source_candidates.sort(key=lambda item: (-item[0], item[1]))
        kernel_status = self._kernel.get_status()
        for _, source_id, source, source_details in source_candidates:
            matched_policy = None
            matched_node = None
            for policy in program.interrupt_policies.values():
                if policy.source != source_id:
                    continue
                matched_node = next(
                    (node for node in active_nodes if self._policy_matches_node(policy, node)),
                    None,
                )
                if matched_node is None:
                    continue
                matched_policy = policy
                break
            if matched_policy is not None and matched_node is not None:
                return PendingInterrupt(
                    interrupt_id=uuid.uuid4().hex[:12],
                    source_id=source_id,
                    source_adapter=source.adapter,
                    signal=source.signal,
                    kind=source.kind,
                    priority=source.priority,
                    maskable=source.maskable,
                    policy_id=matched_policy.id,
                    handler_id=matched_policy.handler,
                    preemption_mode=matched_policy.preemption_mode,
                    after_handler=matched_policy.after_handler,
                    active_node_name=matched_node.name,
                    active_node_id=matched_node.id,
                    active_move_id=kernel_status.active_move_id or None,
                    details=source_details,
                )
            if not source.maskable:
                fallback_node = active_nodes[0] if active_nodes else None
                return PendingInterrupt(
                    interrupt_id=uuid.uuid4().hex[:12],
                    source_id=source_id,
                    source_adapter=source.adapter,
                    signal=source.signal,
                    kind=source.kind,
                    priority=source.priority,
                    maskable=source.maskable,
                    policy_id=None,
                    handler_id=None,
                    preemption_mode="latched_fault",
                    after_handler="halted",
                    active_node_name=fallback_node.name if fallback_node is not None else None,
                    active_node_id=fallback_node.id if fallback_node is not None else None,
                    active_move_id=kernel_status.active_move_id or None,
                    details={"reason": "unhandled_non_maskable_source"} | dict(source_details),
                )
        return None

    def _apply_interrupt_preemption(self, pending: PendingInterrupt) -> None:
        """Latch the interrupt into a deterministic stopped/held runtime state."""
        self._quiesce_runtime(
            client_name="cycle_manager_interrupt_stop",
            kernel_action=(
                "pause_if_executing"
                if pending.preemption_mode == "hold_cycle"
                else "stop_if_active"
            ),
        )

        with self._lock:
            self._pending_interrupt = pending
            self._active_interrupt_handler = None
            self._state = CycleState.STOPPED

        pending.details["quiesce_completed_at_ns"] = time.monotonic_ns()
        pending.details["quiesce_latency_ns"] = max(
            0,
            pending.details["quiesce_completed_at_ns"] - pending.latched_at_ns,
        )
        self._publish_interrupt_context(pending)

        self._notify_interrupt_event(
            action="latched",
            interrupt_id=pending.interrupt_id,
            source=pending.source_id,
            policy_id=pending.policy_id,
            preemption_mode=pending.preemption_mode,
            disposition=pending.after_handler,
            active_node_id=pending.active_node_id,
            active_move_id=pending.active_move_id,
            details={
                "source_adapter": pending.source_adapter,
                "signal": pending.signal,
                "priority": pending.priority,
                "maskable": pending.maskable,
                "active_node_name": pending.active_node_name,
                "handler_id": pending.handler_id,
            }
            | dict(pending.details),
        )

        if self._try_activate_interrupt_handler(pending):
            return

    def _clear_pending_interrupt(self, *, action: str, reason: str) -> None:
        with self._lock:
            pending = self._pending_interrupt
            active = self._active_interrupt_handler
            if pending is None:
                return
            self._pending_interrupt = None
            self._active_interrupt_handler = None
        self._cleanup_interrupt_handler_tree(active)
        self._clear_interrupt_context()
        cleared_at_ns = time.monotonic_ns()
        clear_details = {
            "reason": reason,
            "cleared_at_ns": cleared_at_ns,
            "clear_latency_ns": max(0, cleared_at_ns - pending.latched_at_ns),
        }
        if reason == "operator_resume":
            clear_details["operator_resume_latency_ns"] = max(
                0,
                cleared_at_ns - pending.latched_at_ns,
            )
        elif reason == "handler_auto_resume":
            clear_details["auto_resume_latency_ns"] = max(
                0,
                cleared_at_ns - pending.latched_at_ns,
            )
        elif reason == "handler_restart_subtree":
            clear_details["restart_clear_latency_ns"] = max(
                0,
                cleared_at_ns - pending.latched_at_ns,
            )

        self._notify_interrupt_event(
            action=action,
            interrupt_id=pending.interrupt_id,
            source=pending.source_id,
            policy_id=pending.policy_id,
            preemption_mode=pending.preemption_mode,
            disposition=pending.after_handler,
            active_node_id=pending.active_node_id,
            active_move_id=pending.active_move_id,
            details=clear_details,
        )

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
        _EVENT_TO_TRIGGER = {
            "planning_failed": RecoveryTrigger.PLANNING_FAILED,
            "validation_failed": RecoveryTrigger.VALIDATION_FAILED,
            "execute_failed": RecoveryTrigger.EXECUTE_FAILED,
            "timeout": RecoveryTrigger.TIMEOUT,
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
        return RecoveryTrigger.NODE_FAILURE

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
                time.sleep(sleep_time)

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
