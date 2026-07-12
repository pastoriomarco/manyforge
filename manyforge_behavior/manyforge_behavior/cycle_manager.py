"""CycleManager — runtime lifecycle controller for continuous robot programs."""

from __future__ import annotations

import logging
import json
import math
import os
import queue
import sys
import threading
import time
import traceback
import uuid
from copy import deepcopy
from dataclasses import dataclass, field
from enum import Enum
from itertools import islice
from typing import Any, Callable, Optional

import py_trees

logger = logging.getLogger(__name__)

from collections import OrderedDict, deque

# The stop watchers trust the kernel's internal stop escalation (controlled
# stop -> hard-stop floor) to reach a terminal state within seconds. This
# deadline is the cycle layer's own bound on that trust: past it, the watcher
# forces the cycle out of STOPPING/motion-stop so operator recovery (Reset)
# is never wedged behind a kernel that stopped settling. Generous on purpose —
# it should only ever fire on a genuine kernel/backend wedge.
_STOP_SETTLE_DEADLINE_S = float(os.environ.get("MANYFORGE_STOP_SETTLE_DEADLINE_S", "20.0"))
# A status read failing this many consecutive times (at the 0.05 s retry
# cadence) means the kernel is unreachable, not busy — escalate instead of
# retrying forever.
_STOP_SETTLE_MAX_STATUS_FAILURES = 100
_KERNEL_CONTROL_CALL_TIMEOUT_S = float(
    os.environ.get("MANYFORGE_KERNEL_CONTROL_CALL_TIMEOUT_S", "1.0")
)
_RESET_PHASE_DEADLINE_S = float(
    os.environ.get("MANYFORGE_RESET_PHASE_DEADLINE_S", "20.0")
)
_SIGNAL_PROVIDER_POLL_TICKS = max(
    1,
    int(os.environ.get("MANYFORGE_SIGNAL_PROVIDER_POLL_TICKS", "10")),
)
_PAUSED_TICK_WAIT_S = 0.050
_INTERVENTION_SESSION_EVENT_LIMIT = 4096
_RECOVERY_REQUEST_CACHE_LIMIT = 256

from manyforge_behavior.blackboard_bindings import key_ref_id
from manyforge_behavior.blackboard_namespace import write_namespace_bundle
from manyforge_behavior.cycle_recovery import (
    CycleRecoveryCoordinator,
    CycleRecoveryFacts,
    CycleRecoveryOffer,
    is_kernel_recovery_action,
)
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
from manyforge_behavior.runtime_pause import RuntimePauseMixin, RuntimePauseRegistry


class CycleState(Enum):
    """Lifecycle states for the CycleManager."""
    IDLE = "idle"
    RUNNING = "running"
    STOPPING = "stopping"
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

    Persisted traces carry these as plain strings and are never parsed
    back into this enum, so there are no legacy aliases here.
    """
    PLANNING_FAILED = "planning_failed"
    VALIDATION_FAILED = "validation_failed"
    EXECUTE_FAILED = "execute_failed"
    EXECUTE_TIMEOUT = "execute_timeout"
    SCENE_MISMATCH = "scene_mismatch"
    PRECONDITION_FAILED = "precondition_failed"
    POSTCONDITION_FAILED = "postcondition_failed"


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
    events: deque[dict[str, Any]] = field(
        default_factory=lambda: deque(maxlen=_INTERVENTION_SESSION_EVENT_LIMIT)
    )
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
    if not getattr(node, "_manyforge_internal_status_node", False):
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

    Intervention recovery (M5.5): Run never silently clears
    `INTERVENTION_REQUIRED`. The operator must explicitly choose
    acknowledge-and-continue (no backend reset) or Reset before the cycle can
    run again. The kernel owns the fresh stopped/readable checks for the
    non-reset path.

    The CycleManager owns:
    - The OrchestratorRunner (tree builder)
    - The KernelClient (execution engine)
    - The tick thread (runs the tree at ~100Hz)
    - Blackboard keys: stop_requested, velocity_scale
    """
    
    def __init__(self, runner: OrchestratorRunner, kernel: KernelClient):
        self._runner = runner
        self._kernel = kernel
        self._runtime_pause_registry: RuntimePauseRegistry = (
            runner.runtime_pause_registry
        )
        self._cycle_runtime_fault_ids: dict[str, str] = {}
        self._state = CycleState.IDLE
        self._tree = None
        self._tick_thread = None
        self._tick_cleanup_thread: Optional[threading.Thread] = None
        self._stop_settle_thread: Optional[threading.Thread] = None
        self._stop_settle_generation = 0
        self._motion_stop_thread: Optional[threading.Thread] = None
        self._motion_stop_generation = 0
        self._motion_stop_state = "idle"
        self._tick_stop_event = threading.Event()  # signals tick thread to exit
        self._tick_exit_event = threading.Event()
        self._tick_pause_event = threading.Event()  # cleared = paused, set = running
        self._velocity_scale = 1.0
        # Lock order with Composer's EventRecorder is recorder -> cycle-manager:
        # EventRecorder.observe() may hold its RLock while reading these
        # snapshots. Never call event_recorder.observe() while holding _lock.
        self._lock = threading.Lock()
        # Serializes one complete behavior-tree tick against recovery action
        # admission/resume. Lock order is lifecycle transition -> tree tick ->
        # cycle state -> runtime-pause registry leaf. Stop/full-stop never wait
        # on this lock and retain their reserved control path.
        self._tree_tick_lock = threading.Lock()
        # Reentrant only for the same lifecycle request: Run may consume the
        # current offer through apply_recovery_action(), and a reset action may
        # call reset() through the Composer callback. Other threads remain
        # serialized behind this control-lane lock.
        self._run_transition_lock = threading.RLock()
        self._run_transition_in_progress = False
        self._pending_run_stop: Optional[str] = None
        self._cycle_recovery_generation = 0
        self._cycle_recovery_fact_signature: Optional[tuple[Any, ...]] = None
        self._recovery_context_provider: Optional[
            Callable[[], dict[str, int]]
        ] = None
        self._on_recovery_action_event: Optional[
            Callable[[str, dict[str, Any]], None]
        ] = None
        self._on_recovery_offer_published: Optional[
            Callable[[dict[str, Any]], None]
        ] = None
        self._recovery_event_local = threading.local()
        # Bounded NRT request-id replay ledger for every recovery action. Kernel
        # actions additionally re-check assessment/generation authority in C++;
        # this outer ledger prevents a lost response from repeating any kernel,
        # cycle-only, or reset side effect.
        self._recovery_request_lock = threading.Lock()
        self._recovery_requests: OrderedDict[
            str, tuple[tuple[Any, ...], Optional[dict[str, Any]]]
        ] = OrderedDict()
        self._recovery_coordinator = CycleRecoveryCoordinator(
            on_offer_published=self._handle_recovery_offer_published,
        )
        self._on_run_from_idle: Optional[Callable[[], None]] = None
        self._error: Optional[str] = None
        # Structured cause for the viewer: when the cycle's `_error` is set from
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
        self._on_reset_phase_event: Optional[Callable[[dict[str, Any]], None]] = None
        self._on_recovery_exhausted: Optional[
            Callable[[str, str, str], None]
        ] = None
        self._parent_run_id_provider: Optional[Callable[[], Optional[str]]] = None
        self._kernel_events: deque[dict[str, Any]] = deque(maxlen=4096)
        # Serializes collector drain + indexed append. The behavior tick,
        # status routes, websocket recorder, and recovery pump may all request
        # a drain; without this lock later kernel sequences could be indexed
        # before an earlier concurrently drained batch.
        self._kernel_event_drain_lock = threading.Lock()
        self._kernel_event_index: int = 0
        self._kernel_collector_dropped_total: int = 0
        self._kernel_manager_dropped_total: int = 0
        self._kernel_last_sequence_seen: int = 0
        self._signal_registry = RuntimeSignalRegistry()
        self._signal_managed_keys: set[str] = set()
        self._signal_provider_poll_ticks = _SIGNAL_PROVIDER_POLL_TICKS
        self._signal_provider_poll_countdown = 0
        self._last_signal_registry_change_counter = (
            self._signal_registry.change_counter()
        )
        # Program-run correlation id (spec/500 §2.2): minted once per
        # program start (run from IDLE), written to the blackboard so
        # motion nodes attach it to every ExecuteRequest. Resume from
        # STOPPED keeps the same id — it is the same program run.
        self._program_run_id: Optional[str] = None
        # Set initial pause event to set (not paused)
        self._tick_pause_event.set()
        self._tick_exit_event.set()

    @staticmethod
    def _kernel_stop_terminal(state: ExecutionState) -> bool:
        return state in {
            ExecutionState.IDLE,
            ExecutionState.STOPPED,
            ExecutionState.COMPLETED,
            ExecutionState.FAILED,
            ExecutionState.INTERVENTION_REQUIRED,
        }

    def _call_with_timeout(
        self,
        label: str,
        fn: Callable[[], Any],
        timeout_s: float,
    ) -> Any:
        result_queue: queue.Queue[tuple[bool, Any]] = queue.Queue(maxsize=1)

        def worker() -> None:
            try:
                result_queue.put((True, fn()))
            except BaseException as exc:  # noqa: BLE001 - propagate worker result.
                result_queue.put((False, exc))

        thread = threading.Thread(
            target=worker,
            name=f"manyforge_cycle_{label}",
            daemon=True,
        )
        thread.start()
        try:
            ok, value = result_queue.get(timeout=max(0.0, timeout_s))
        except queue.Empty as exc:
            raise TimeoutError(
                f"{label} did not return within {timeout_s:g}s"
            ) from exc
        if ok:
            return value
        raise value

    def _kernel_control_command(
        self,
        label: str,
        fn_with_epoch: Callable[[int], Any],
        timeout_s: Optional[float] = None,
    ) -> Any:
        timeout = _KERNEL_CONTROL_CALL_TIMEOUT_S if timeout_s is None else timeout_s
        epoch = self._kernel.control_command_epoch()
        try:
            return self._call_with_timeout(
                label,
                lambda: fn_with_epoch(epoch),
                timeout,
            )
        except TimeoutError:
            try:
                self._kernel.bump_control_command_epoch()
            except Exception:  # noqa: BLE001 - preserve original timeout surface.
                logger.exception(
                    "Failed to invalidate timed-out kernel control command %s",
                    label,
                )
            raise

    def _kernel_status_with_timeout(self, label: str) -> ExecutionStatus:
        return self._call_with_timeout(
            label,
            self._kernel.get_status,
            _KERNEL_CONTROL_CALL_TIMEOUT_S,
        )

    def _kernel_state_with_timeout(self, label: str) -> ExecutionState:
        return self._kernel_status_with_timeout(label).state

    def _request_kernel_stop_best_effort(self, label: str) -> None:
        try:
            self._call_with_timeout(
                label,
                self._kernel.stop,
                _KERNEL_CONTROL_CALL_TIMEOUT_S,
            )
        except Exception:  # noqa: BLE001 - escalation must still park the cycle.
            logger.warning("Best-effort kernel.stop() timed out or failed during %s", label)

    def _record_pending_run_stop_locked(self, kind: str) -> None:
        if kind == "full_stop" or self._pending_run_stop != "full_stop":
            self._pending_run_stop = kind

    def _finish_run_transition_locked(self, *, committed: bool) -> Optional[str]:
        pending = self._pending_run_stop if committed else None
        self._run_transition_in_progress = False
        self._pending_run_stop = None
        return pending

    def _escalate_stop_settle(
        self,
        generation: int,
        *,
        kernel_state: Optional[ExecutionState],
        error_code: str,
    ) -> None:
        """Force the cycle out of STOPPING when the settle watcher gives up.

        Fail-closed recovery surface: STOPPED with `_error` set (spec/410),
        so the recovery offer permits Reset only. Without this, a kernel that
        never reaches a terminal
        stop state leaves the cycle in STOPPING forever with reset refused.
        """
        kernel_state_name = kernel_state.value if kernel_state is not None else "unreadable"
        # Best-effort repeat of the stop request; the kernel owns the actual
        # escalation (hard-stop floor) internally.
        self._request_kernel_stop_best_effort("stop_settle_escalation_stop")
        with self._lock:
            if self._stop_settle_generation != generation:
                return
            self._stop_settle_thread = None
            if self._state != CycleState.STOPPING:
                return
            self._state = CycleState.STOPPED
            self._error = (
                f"Stop did not settle within {_STOP_SETTLE_DEADLINE_S:.0f}s "
                f"(kernel state: {kernel_state_name}); cycle forced to STOPPED. "
                "Reset before running again."
            )
            self._error_code = error_code
            self._error_details = {
                "kernel_state": kernel_state_name,
                "deadline_s": f"{_STOP_SETTLE_DEADLINE_S:g}",
            }
        logger.error(
            "Stop settle watcher escalated (%s): kernel state %s after %.0fs",
            error_code,
            kernel_state_name,
            _STOP_SETTLE_DEADLINE_S,
        )

    def _await_kernel_stop_terminal(self, generation: int) -> None:
        deadline = time.monotonic() + _STOP_SETTLE_DEADLINE_S
        status_failures = 0
        while True:
            try:
                kernel_state = self._kernel_state_with_timeout("stop_settle_status")
            except TimeoutError:
                self._escalate_stop_settle(
                    generation,
                    kernel_state=None,
                    error_code="stop_settle_status_timeout",
                )
                return
            except Exception:
                status_failures += 1
                logger.exception("Failed while waiting for kernel stop terminal state")
                if status_failures >= _STOP_SETTLE_MAX_STATUS_FAILURES:
                    self._escalate_stop_settle(
                        generation,
                        kernel_state=None,
                        error_code="stop_settle_status_unreadable",
                    )
                    return
                time.sleep(0.05)
                continue
            status_failures = 0
            if time.monotonic() >= deadline and not self._kernel_stop_terminal(kernel_state):
                self._escalate_stop_settle(
                    generation,
                    kernel_state=kernel_state,
                    error_code="stop_settle_timeout",
                )
                return
            if self._kernel_stop_terminal(kernel_state):
                with self._lock:
                    if (
                        self._state == CycleState.STOPPING
                        and self._stop_settle_generation == generation
                    ):
                        self._state = CycleState.STOPPED
                    if self._stop_settle_generation == generation:
                        self._stop_settle_thread = None
                return
            time.sleep(0.02)

    def _start_stop_settle_watcher_locked(self) -> None:
        self._stop_settle_generation += 1
        generation = self._stop_settle_generation
        self._stop_settle_thread = threading.Thread(
            target=self._await_kernel_stop_terminal,
            args=(generation,),
            daemon=True,
        )
        self._stop_settle_thread.start()

    def _escalate_motion_stop(
        self,
        generation: int,
        *,
        kernel_state: Optional[ExecutionState],
        error_code: str,
    ) -> None:
        """Force the motion-stop out of 'stopping' when its watcher gives up.

        Mirrors the FAILED/INTERVENTION branch of
        `_complete_motion_stop_from_kernel_state`: full-stop the cycle so the
        operator regains Stop/Reset instead of `reset()` refusing forever with
        "motion stop is in progress".
        """
        kernel_state_name = kernel_state.value if kernel_state is not None else "unreadable"
        self._request_kernel_stop_best_effort("motion_stop_escalation_stop")
        clear_motion_stop_flag = False
        set_stop_requested = False
        with self._lock:
            if self._motion_stop_generation != generation:
                return
            self._motion_stop_thread = None
            if self._motion_stop_state != "stopping":
                return
            self._motion_stop_state = "idle"
            self._state = CycleState.STOPPED
            self._error = (
                f"Motion stop did not settle within {_STOP_SETTLE_DEADLINE_S:.0f}s "
                f"(kernel state: {kernel_state_name}); cycle was fully stopped. "
                "Reset before running again."
            )
            self._error_code = error_code
            self._error_details = {
                "kernel_state": kernel_state_name,
                "deadline_s": f"{_STOP_SETTLE_DEADLINE_S:g}",
            }
            self._tick_pause_event.clear()
            clear_motion_stop_flag = True
            set_stop_requested = True
        if clear_motion_stop_flag:
            self._set_motion_stop_active_flag(
                False,
                client_name="cycle_manager_motion_stop_escalated",
            )
        if set_stop_requested:
            self._set_stop_requested_flag(
                True,
                client_name="cycle_manager_motion_stop_escalated",
            )
        logger.error(
            "Motion stop watcher escalated (%s): kernel state %s after %.0fs",
            error_code,
            kernel_state_name,
            _STOP_SETTLE_DEADLINE_S,
        )

    def _await_motion_stop_terminal(self, generation: int) -> None:
        deadline = time.monotonic() + _STOP_SETTLE_DEADLINE_S
        status_failures = 0
        while True:
            try:
                kernel_state = self._kernel_state_with_timeout("motion_stop_status")
            except TimeoutError:
                self._escalate_motion_stop(
                    generation,
                    kernel_state=None,
                    error_code="motion_stop_status_timeout",
                )
                return
            except Exception:
                status_failures += 1
                logger.exception("Failed while waiting for motion stop terminal state")
                if status_failures >= _STOP_SETTLE_MAX_STATUS_FAILURES:
                    self._escalate_motion_stop(
                        generation,
                        kernel_state=None,
                        error_code="motion_stop_status_unreadable",
                    )
                    return
                time.sleep(0.05)
                continue
            status_failures = 0
            if self._kernel_stop_terminal(kernel_state):
                self._complete_motion_stop_from_kernel_state(
                    kernel_state,
                    generation=generation,
                )
                return
            if time.monotonic() >= deadline:
                self._escalate_motion_stop(
                    generation,
                    kernel_state=kernel_state,
                    error_code="motion_stop_timeout",
                )
                return
            time.sleep(0.02)

    def _start_motion_stop_watcher_locked(self) -> None:
        self._motion_stop_generation += 1
        generation = self._motion_stop_generation
        self._motion_stop_thread = threading.Thread(
            target=self._await_motion_stop_terminal,
            args=(generation,),
            daemon=True,
        )
        self._motion_stop_thread.start()

    def _complete_motion_stop_from_kernel_state(
        self,
        kernel_state: ExecutionState,
        *,
        generation: Optional[int] = None,
    ) -> None:
        clear_motion_stop_flag = False
        set_stop_requested = False
        with self._lock:
            if generation is not None and self._motion_stop_generation != generation:
                return
            if self._motion_stop_state != "stopping":
                if generation is not None and self._motion_stop_generation == generation:
                    self._motion_stop_thread = None
                return
            if kernel_state in {
                ExecutionState.FAILED,
                ExecutionState.INTERVENTION_REQUIRED,
            }:
                self._motion_stop_state = "idle"
                self._state = CycleState.STOPPED
                self._error = (
                    "Motion stop failed or entered intervention; cycle was fully stopped"
                )
                self._error_code = "motion_stop_failed"
                self._error_details = {"kernel_state": kernel_state.value}
                self._tick_pause_event.clear()
                clear_motion_stop_flag = True
                set_stop_requested = True
            else:
                self._motion_stop_state = "held"
            if generation is not None and self._motion_stop_generation == generation:
                self._motion_stop_thread = None
        if clear_motion_stop_flag:
            self._set_motion_stop_active_flag(
                False,
                client_name="cycle_manager_motion_stop_failed",
            )
        if set_stop_requested:
            self._set_stop_requested_flag(
                True,
                client_name="cycle_manager_motion_stop_failed",
            )

    def _clear_motion_stop_after_tree_terminal_locked(self) -> bool:
        if self._motion_stop_state == "idle":
            return False
        self._motion_stop_generation += 1
        self._motion_stop_thread = None
        self._motion_stop_state = "idle"
        return True

    def _tree_is_terminal(self) -> bool:
        """True when the current tree has already reached a terminal status."""
        if self._tree is None:
            return False
        return self._tree.root.status in (
            py_trees.common.Status.SUCCESS,
            py_trees.common.Status.FAILURE,
        )

    def _has_resumable_execution_context_locked(self) -> bool:
        return (
            self._state == CycleState.STOPPED
            and self._tree is not None
            and not self._tree_is_terminal()
        )

    def has_resumable_execution_context(self) -> bool:
        """True when STOPPED still holds a behavior-tree context that Run resumes."""
        with self._lock:
            return self._has_resumable_execution_context_locked()

    def live_scene_mutation_block_reason(self) -> Optional[str]:
        """Return a stable reason if live scene mutation would invalidate runtime state."""
        with self._lock:
            if self._state in {
                CycleState.RUNNING,
                CycleState.STOPPING,
                CycleState.DIRECT_CONTROL,
            }:
                return f"execution_active:{self._state.name}"
            if self._has_resumable_execution_context_locked():
                return "resumable_stopped_context"
            return None

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

    def _emit_reset_phase_event(
        self,
        *,
        phase: str,
        status: str,
        request_id: Optional[str],
        duration_s: Optional[float] = None,
        error: Optional[str] = None,
        error_code: Optional[str] = None,
    ) -> None:
        callback = self._on_reset_phase_event
        if callback is None:
            return
        with self._lock:
            cycle_state = self._state.value
            motion_stop_state = self._motion_stop_state
            program_run_id = self._program_run_id
        payload: dict[str, Any] = {
            "phase": phase,
            "status": status,
            "cycle_state": cycle_state,
            "motion_stop_state": motion_stop_state,
            "program_run_id": program_run_id,
        }
        if request_id:
            payload["request_id"] = request_id
        if duration_s is not None:
            payload["duration_ms"] = round(duration_s * 1000.0, 3)
            payload["deadline_s"] = _RESET_PHASE_DEADLINE_S
        if error:
            payload["error"] = error
        if error_code:
            payload["error_code"] = error_code
        try:
            callback(payload)
        except Exception:  # noqa: BLE001 - telemetry must not affect reset
            logger.exception("Reset phase callback failed")

    def _mark_reset_phase_failed(
        self,
        *,
        phase: str,
        error: str,
        error_code: str,
        duration_s: Optional[float] = None,
    ) -> None:
        with self._lock:
            self._error = error
            self._error_code = error_code
            self._error_details = {
                "phase": phase,
                "reason_code": error_code,
                "deadline_s": f"{_RESET_PHASE_DEADLINE_S:g}",
            }
            if duration_s is not None:
                self._error_details["duration_s"] = f"{duration_s:.3f}"
            self._state = CycleState.STOPPED
            self._pending_delegated_recovery = None

    def _run_reset_phase(
        self,
        phase: str,
        fn: Callable[[], Any],
        *,
        request_id: Optional[str],
        fail_closed: bool = True,
        emit_completed: bool = True,
        bounded_by_callee: bool = False,
    ) -> Any:
        self._emit_reset_phase_event(
            phase=phase,
            status="started",
            request_id=request_id,
        )
        started = time.monotonic()
        try:
            if bounded_by_callee:
                result = fn()
            else:
                result = self._call_with_timeout(
                    f"reset_{phase}",
                    fn,
                    _RESET_PHASE_DEADLINE_S,
                )
        except TimeoutError as exc:
            duration_s = time.monotonic() - started
            error_code = f"reset_{phase}_timeout"
            error = (
                f"Reset phase '{phase}' exceeded "
                f"{_RESET_PHASE_DEADLINE_S:.0f}s deadline "
                f"({duration_s:.1f}s). Cycle left STOPPED; retry Reset."
            )
            if fail_closed:
                self._mark_reset_phase_failed(
                    phase=phase,
                    error=error,
                    error_code=error_code,
                    duration_s=duration_s,
                )
            self._emit_reset_phase_event(
                phase=phase,
                status="failed",
                request_id=request_id,
                duration_s=duration_s,
                error=error,
                error_code=error_code,
            )
            raise RuntimeError(error) from exc
        except Exception as exc:
            duration_s = time.monotonic() - started
            error = str(exc) or type(exc).__name__
            error_code = f"reset_{phase}_failed"
            if fail_closed:
                self._mark_reset_phase_failed(
                    phase=phase,
                    error=error,
                    error_code=error_code,
                    duration_s=duration_s,
                )
            self._emit_reset_phase_event(
                phase=phase,
                status="failed",
                request_id=request_id,
                duration_s=duration_s,
                error=error,
                error_code=error_code,
            )
            raise
        duration_s = time.monotonic() - started
        if emit_completed:
            self._emit_reset_phase_event(
                phase=phase,
                status="completed",
                request_id=request_id,
                duration_s=duration_s,
            )
        return result

    def _prepare_fresh_cycle(
        self,
        *,
        reset_request_id: Optional[str] = None,
        record_reset_phases: bool = False,
    ) -> None:
        """Rebuild tree and reapply the program scene without touching robot state."""
        if self._runner.program is None:
            self._runner.load()

        def clear_runtime_flags() -> None:
            self._runtime_pause_registry.clear_all()
            with self._lock:
                self._cycle_runtime_fault_ids.clear()
            self._clear_gripper_fault_blackboard()
            self._set_motion_stop_active_flag(
                False,
                client_name="cycle_manager_prepare_fresh",
            )

        if record_reset_phases:
            self._run_reset_phase(
                "runtime_flags",
                clear_runtime_flags,
                request_id=reset_request_id,
            )
        else:
            clear_runtime_flags()
        with self._lock:
            self._motion_stop_state = "idle"
        if record_reset_phases:
            self._run_reset_phase(
                "scene_clear",
                self._clear_runtime_scene,
                request_id=reset_request_id,
            )
            self._tree = self._run_reset_phase(
                "tree_rebuild",
                self._runner.build_tree,
                request_id=reset_request_id,
            )
            self._run_reset_phase(
                "scene_apply",
                self._runner.apply_scene,
                request_id=reset_request_id,
            )
        else:
            self._clear_runtime_scene()
            self._tree = self._runner.build_tree()
            self._runner.apply_scene()
        self._write_bb_keys()
        with self._lock:
            self._node_statuses = {}
            self._pending_delegated_recovery = None

    def seed_authored_scene(self) -> None:
        """Seed the live planning scene with the program's authored objects.

        This is the explicit, load-time scene seed. Unlike `_prepare_fresh_cycle`
        it does NOT build a behavior tree or change cycle state: it only clears
        any stale runtime objects and re-applies the program's authored scene,
        leaving the cycle IDLE with `_tree is None` so the first Run still
        prepares a fresh cycle. This populates the live scene for at-idle
        visualization without any implicit reset loop — the only writers of the
        live scene remain the program cycle and explicit Reset.
        """
        if self._runner.program is None:
            self._runner.load()
        self._clear_runtime_scene()
        self._runner.apply_scene()

    @property
    def state(self) -> CycleState:
        """Current lifecycle state."""
        return self._state

    @property
    def motion_stop_state(self) -> str:
        """Motion-stop lifecycle independent of cycle tick state."""
        with self._lock:
            return self._motion_stop_state
    
    @property
    def velocity_scale(self) -> float:
        """Current velocity scale (0.0 to 1.0)."""
        with self._lock:
            return self._velocity_scale

    @property
    def program_run_id(self) -> Optional[str]:
        """Correlation id of the current program run (spec/500 §2.2)."""
        with self._lock:
            return self._program_run_id

    @property
    def error(self) -> Optional[str]:
        """Last tick-loop exception or surfaced terminal node failure, or None."""
        with self._lock:
            return self._error

    @property
    def error_code(self) -> Optional[str]:
        """Stable machine-readable code matching the kernel ErrorDTO, when
        the latest error came from a kernel/backend fault. Motion-soft-fail
        errors that only produced a human message leave this None."""
        with self._lock:
            return self._error_code

    @property
    def error_details(self) -> Optional[dict[str, Any]]:
        """Structured diagnostics map matching the kernel ErrorDTO. Carries
        collision-pair / classification / waypoint-index / reason-code style
        details when the cause is a kernel fault. None when the cause did
        not produce a structured payload."""
        with self._lock:
            return deepcopy(self._error_details)

    def get_error_snapshot(
        self,
    ) -> tuple[Optional[str], Optional[str], Optional[dict[str, Any]]]:
        """Return one lock-coherent, observer-safe cycle error tuple."""
        with self._lock:
            return (
                self._error,
                self._error_code,
                deepcopy(self._error_details),
            )

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
                    "actuation_kind": config.actuation_kind,
                    "state_authority": config.state_authority,
                    "completion_policy": config.completion_policy,
                    "grasp_confirmation_policy": config.grasp_confirmation_policy,
                    "transport": dict(config.transport),
                    "open_state_ref": config.open_state_ref,
                    "closed_state_ref": config.closed_state_ref,
                    "semantic_configured": config.semantic_configured,
                    "migration_notes": list(config.migration_notes),
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

    def _drain_kernel_events(self) -> list[dict[str, Any]]:
        """Pull native kernel events from the kernel and buffer them.

        Loss accounting (observability plan OW1): the collector-side
        overflow-drop total rides along with every drain, and evictions of
        events that were buffered here but never served to a consumer are
        counted as manager-side drops. Both totals are monotonic and exposed
        via kernel_event_pipeline_status().
        """
        with self._kernel_event_drain_lock:
            stats = self._kernel.drain_kernel_events_with_stats()
            events = stats.get("events") or []
            collector_dropped = int(stats.get("dropped_total") or 0)
            with self._lock:
                self._kernel_collector_dropped_total = collector_dropped
                if events:
                    maxlen = self._kernel_events.maxlen or 0
                    overflow = max(
                        0, len(self._kernel_events) + len(events) - maxlen
                    ) if maxlen else 0
                    if overflow:
                        self._kernel_manager_dropped_total += overflow
                    for event in events:
                        self._kernel_event_index += 1
                        event["event_index"] = self._kernel_event_index
                        self._kernel_events.append(event)
                    last_sequence = int(events[-1].get("sequence") or 0)
                    if last_sequence:
                        self._kernel_last_sequence_seen = last_sequence
        recovery_projection_needed = any(
            str(event.get("type") or "")
            in {
                "recovery_assessment_changed",
                "recovery_action_accepted",
                "recovery_action_rejected",
                "state_transition",
                "stop_certificate_published",
                "interrupt_latched",
                "interrupt_preempt_applied",
            }
            for event in events
        )
        if recovery_projection_needed:
            try:
                self.refresh_recovery_offer()
            except Exception:  # noqa: BLE001 - event drain remains available.
                logger.exception("Recovery offer projection failed after kernel event")
        return events

    def wait_for_kernel_events_and_project(self, timeout_s: float) -> bool:
        """Wait on the NRT collector notification and project received facts.

        Composer owns the caller thread. Native waits release the GIL and the
        kernel event publisher only performs a notify; controller and safety
        lanes never wait for this consumer.
        """
        if not self._kernel.wait_for_kernel_events(timeout_s):
            return False
        self._drain_kernel_events()
        return True

    def kernel_event_pipeline_status(self) -> dict[str, int]:
        """Monotonic loss counters for the kernel->Python event pipeline."""
        with self._lock:
            return {
                "collector_dropped_total": self._kernel_collector_dropped_total,
                "manager_dropped_total": self._kernel_manager_dropped_total,
                "last_sequence_seen": self._kernel_last_sequence_seen,
            }

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
            if not self._kernel_events:
                return []
            first_index = int(self._kernel_events[0].get("event_index", 0) or 0)
            last_index = int(self._kernel_events[-1].get("event_index", 0) or 0)
            if event_index < first_index:
                start_offset = 0
            elif event_index >= last_index:
                return []
            else:
                start_offset = event_index - first_index + 1
            return [
                dict(event)
                for event in islice(self._kernel_events, start_offset, None)
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
        self._last_signal_registry_change_counter = (
            self._signal_registry.change_counter()
        )

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
        self._last_signal_registry_change_counter = (
            self._signal_registry.change_counter()
        )

    def publish_force_current_evidence(
        self,
        *,
        gripper_id: str,
        value: float,
        min_value: float | None = None,
        max_value: float | None = None,
        units: str = "",
        adapter: str = "gripper_evidence",
        signal: str | None = None,
        mirror_key: str | None = None,
        stale_after_ms: int = 250,
        source: str = "",
        metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Publish bounded force/current grasp evidence as a boolean signal.

        Adapters own the sensor-specific sampling and unit conversion. This
        producer is the runtime contract boundary: it converts one finite,
        already-normalized force/current sample into the boolean source consumed
        by ConfirmGripperGrasp while preserving the raw evidence in metadata.
        """

        gripper = str(gripper_id or "").strip()
        if not gripper:
            raise ValueError("gripper_id must be non-empty")
        try:
            sample = float(value)
        except (TypeError, ValueError) as exc:
            raise ValueError("force/current evidence value must be numeric") from exc
        if not math.isfinite(sample):
            raise ValueError("force/current evidence value must be finite")
        if min_value is None and max_value is None:
            raise ValueError(
                "force/current evidence requires min_value or max_value"
            )

        min_bound: float | None = None
        max_bound: float | None = None
        if min_value is not None:
            min_bound = float(min_value)
            if not math.isfinite(min_bound):
                raise ValueError("force/current evidence min_value must be finite")
        if max_value is not None:
            max_bound = float(max_value)
            if not math.isfinite(max_bound):
                raise ValueError("force/current evidence max_value must be finite")
        if min_bound is not None and max_bound is not None and min_bound > max_bound:
            raise ValueError(
                "force/current evidence min_value must be <= max_value"
            )

        confirmed = (
            (min_bound is None or sample >= min_bound)
            and (max_bound is None or sample <= max_bound)
        )
        signal_id = str(signal or f"gripper.force_current.{gripper}").strip()
        mirror = str(
            mirror_key or f"gripper/{gripper}/force_current_confirmed"
        ).strip()
        if not signal_id:
            raise ValueError("force/current evidence signal must be non-empty")
        if not mirror:
            raise ValueError("force/current evidence mirror_key must be non-empty")

        evidence_metadata = {
            "evidence_kind": "force_or_current",
            "gripper_id": gripper,
            "value": sample,
            "units": str(units or ""),
            "min_value": min_bound,
            "max_value": max_bound,
            "confirmed": confirmed,
        }
        if source:
            evidence_metadata["source"] = str(source)
        evidence_metadata.update(metadata or {})

        self.publish_signal(
            adapter=str(adapter or "gripper_evidence"),
            signal=signal_id,
            value=confirmed,
            stale_after_ms=max(1, int(stale_after_ms)),
            mirror_key=mirror,
            clear_on_stale=True,
            metadata=evidence_metadata,
        )
        return {
            "accepted": True,
            "confirmed": confirmed,
            "adapter": str(adapter or "gripper_evidence"),
            "signal": signal_id,
            "mirror_key": mirror,
            "metadata": evidence_metadata,
        }

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

    def _set_motion_stop_active_flag(self, active: bool, *, client_name: str) -> None:
        bb = py_trees.blackboard.Client(name=client_name)
        bb.register_key(key="motion_stop_active", access=py_trees.common.Access.WRITE)
        bb.motion_stop_active = active

    def _enter_motion_paused_state(
        self,
        error_message: str,
        *,
        runtime_reason_code: Optional[str] = None,
        runtime_details: Optional[dict[str, Any]] = None,
    ) -> None:
        """Convert a generation-scoped runtime fault into held motion-stop.

        A runtime motion fault is not, by itself, a behavior-tree failure:
        the motion branch should hold while continue-policy branches keep
        ticking. Only backend states that require operator intervention fail
        closed by pausing the whole tick loop.
        """
        structured_code, structured_details = self._capture_kernel_error_diagnostics()
        structured_code = structured_code or runtime_reason_code
        if runtime_details:
            structured_details = {
                **(structured_details or {}),
                **runtime_details,
            }
        clear_motion_signal = False
        with self._lock:
            if self._state != CycleState.RUNNING:
                return
            if self._motion_stop_state in {"stopping", "held"}:
                self._error = error_message
                self._error_code = structured_code
                self._error_details = structured_details
                clear_motion_signal = True
            else:
                self._motion_stop_state = "stopping"
                self._error = error_message
                self._error_code = structured_code
                self._error_details = structured_details

        if clear_motion_signal:
            return

        self._set_motion_stop_active_flag(
            True,
            client_name="cycle_manager_motion_soft_fail",
        )
        try:
            kernel_status = self._call_with_timeout(
                "motion_soft_fail_initial_status",
                self._kernel.get_status,
                _KERNEL_CONTROL_CALL_TIMEOUT_S,
            )
        except TimeoutError:
            with self._lock:
                if self._motion_stop_state == "stopping":
                    self._start_motion_stop_watcher_locked()
            return
        except Exception as exc:  # noqa: BLE001 - fail closed, but keep diagnostics
            self._fail_motion_soft_stop_closed(
                error_message,
                structured_code,
                structured_details,
                reason_code="motion_soft_fail_status_unreadable",
                kernel_state=None,
                detail=str(exc) or type(exc).__name__,
            )
            return

        if kernel_status.state in {
            ExecutionState.FAILED,
            ExecutionState.INTERVENTION_REQUIRED,
        }:
            self._fail_motion_soft_stop_closed(
                error_message,
                structured_code,
                structured_details,
                reason_code="motion_soft_fail_requires_intervention",
                kernel_state=kernel_status.state,
            )
            return

        if kernel_status.state == ExecutionState.INTERRUPTED:
            with self._lock:
                if self._motion_stop_state == "stopping":
                    self._motion_stop_state = "held"
            return

        stoppable = {
            ExecutionState.EXECUTING,
            ExecutionState.PLANNING,
            ExecutionState.VALIDATING,
            ExecutionState.PAUSED,
            ExecutionState.STOPPING,
        }
        if kernel_status.state in stoppable:
            try:
                self._call_with_timeout(
                    "motion_soft_fail_stop",
                    self._kernel.stop,
                    _KERNEL_CONTROL_CALL_TIMEOUT_S,
                )
                kernel_state = self._kernel_state_with_timeout(
                    "motion_soft_fail_post_status"
                )
            except TimeoutError:
                with self._lock:
                    if self._motion_stop_state == "stopping":
                        self._start_motion_stop_watcher_locked()
                return
            except Exception as exc:  # noqa: BLE001 - fail closed, but keep diagnostics
                self._fail_motion_soft_stop_closed(
                    error_message,
                    structured_code,
                    structured_details,
                    reason_code="motion_soft_fail_stop_failed",
                    kernel_state=None,
                    detail=str(exc) or type(exc).__name__,
                )
                return
            self._complete_motion_stop_from_kernel_state(kernel_state)
            return

        with self._lock:
            if self._motion_stop_state == "stopping":
                self._motion_stop_state = "held"

    def _fail_motion_soft_stop_closed(
        self,
        error_message: str,
        structured_code: Optional[str],
        structured_details: Optional[dict[str, Any]],
        *,
        reason_code: str,
        kernel_state: Optional[ExecutionState],
        detail: Optional[str] = None,
    ) -> None:
        details = dict(structured_details or {})
        details["motion_soft_stop_reason"] = reason_code
        if kernel_state is not None:
            details["kernel_state"] = kernel_state.value
        if detail:
            details["detail"] = detail
        with self._lock:
            self._motion_stop_state = "idle"
            self._state = CycleState.STOPPED
            self._error = error_message
            self._error_code = structured_code or reason_code
            self._error_details = details or None
            self._tick_pause_event.clear()
        self._set_motion_stop_active_flag(
            False,
            client_name="cycle_manager_motion_soft_fail_failed",
        )
        self._set_stop_requested_flag(
            True,
            client_name="cycle_manager_motion_soft_fail_failed",
        )

    def _kernel_events_require_intervention(self, events: list[dict[str, Any]]) -> bool:
        """True when freshly drained kernel events prove intervention is latched.

        This is intentionally event-driven instead of a 100 Hz status poll.
        A held motion-soft-stop may be entered while the kernel is still
        `INTERRUPTED`; if the kernel later escalates the same stop/fault to
        `INTERVENTION_REQUIRED`, the cycle must fail closed instead of
        continuing to advertise plain Resume.
        """

        for event in events:
            event_type = event.get("type") or event.get("event_type")
            if event_type != "state_transition":
                continue
            to_state = str(event.get("to_state") or event.get("to") or "").lower()
            if to_state == ExecutionState.INTERVENTION_REQUIRED.value:
                return True
        return False

    def _fail_held_motion_stop_closed_if_intervention_latched(
        self,
        events: list[dict[str, Any]],
    ) -> bool:
        if not events or not self._kernel_events_require_intervention(events):
            return False
        with self._lock:
            if self._state != CycleState.RUNNING or self._motion_stop_state != "held":
                return False
            error_message = self._error or (
                "Motion stop entered operator intervention; cycle was fully stopped"
            )
        structured_code, structured_details = self._capture_kernel_error_diagnostics()
        self._fail_motion_soft_stop_closed(
            error_message,
            structured_code,
            structured_details,
            reason_code="motion_soft_fail_requires_intervention",
            kernel_state=ExecutionState.INTERVENTION_REQUIRED,
        )
        return True

    def raise_runtime_condition_fault(
        self,
        *,
        condition_id: str,
        message: str,
        details: Optional[dict[str, Any]] = None,
    ) -> None:
        """Fail closed on an externally evaluated runtime condition.

        Pose-source measured-grasp integrity and similar supervisors run
        outside the behavior tree but still need the same operator-visible
        stop/error semantics. This method is the narrow bridge: it quiesces
        active motion through the kernel stop path, stores structured error
        details, and leaves the held tree resumable only after the operator
        explicitly handles the error.
        """
        detail_payload = dict(details or {})
        detail_payload.setdefault("condition_id", condition_id)
        detail_payload.setdefault("reason_code", condition_id)
        with self._lock:
            current_state = self._state
        if current_state not in {
            CycleState.RUNNING,
            CycleState.STOPPING,
            CycleState.STOPPED,
        }:
            return

        if current_state in {CycleState.RUNNING, CycleState.STOPPING}:
            self._quiesce_runtime(
                client_name="cycle_manager_runtime_condition_fault",
                kernel_action="stop_if_active",
            )
        with self._lock:
            self._error = message
            self._error_code = condition_id
            self._error_details = detail_payload
            self._state = CycleState.STOPPED
            existing_fault_id = self._cycle_runtime_fault_ids.get(condition_id)
            existing_fault = (
                self._runtime_pause_registry.get(existing_fault_id)
                if existing_fault_id is not None
                else None
            )
            if existing_fault is None:
                fault = self._runtime_pause_registry.raise_fault(
                    node_name="cycle_runtime_supervisor",
                    node_id="cycle_runtime_supervisor",
                    domain="supervisor",
                    reason_code=condition_id,
                    message=message,
                    details=detail_payload,
                    resume_owner="cycle_manager",
                )
                self._cycle_runtime_fault_ids[condition_id] = fault.fault_id
        self.refresh_recovery_offer()

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
        event_code, event_details = self._harvest_kernel_event_diagnostics()
        if event_code or event_details:
            return event_code, event_details
        # Gripper faults never reach the execution snapshot or the kernel
        # event stream — the gripper runtime reports through command/status
        # results only. The command_gripper node mirrors a classified
        # failure onto the blackboard so the digest can carry the reason
        # token instead of bare tree-failure prose.
        return self._read_gripper_fault_diagnostics()

    def _read_gripper_fault_diagnostics(
        self,
    ) -> tuple[Optional[str], Optional[dict[str, Any]]]:
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_gripper_fault_read")
            bb.register_key(
                key="gripper_fault_details",
                access=py_trees.common.Access.READ,
            )
            details = bb.get("gripper_fault_details")
        except (KeyError, AttributeError):
            return None, None
        if not isinstance(details, dict) or not details:
            return None, None
        reason_code = details.get("reason_code")
        code = (
            reason_code
            if isinstance(reason_code, str)
            and reason_code in _ROS2_CONTROL_BACKEND_REASON_CODES
            else None
        )
        return code, dict(details)

    def _clear_gripper_fault_blackboard(self) -> None:
        try:
            bb = py_trees.blackboard.Client(name="cycle_manager_gripper_fault_clear")
            bb.register_key(
                key="gripper_fault_details",
                access=py_trees.common.Access.WRITE,
            )
            bb.set("gripper_fault_details", None)
        except (KeyError, AttributeError):
            pass

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
            # the viewer can route to the right operator-facing copy.
            return "runtime_collision", latest_collision
        if latest_latch is not None:
            return "motion_interrupt_latched", latest_latch
        return None, None

    def _quiesce_runtime(
        self,
        *,
        client_name: str,
        kernel_action: str,
    ) -> None:
        """Enter a quiesced runtime mode shared by stop and reset paths."""
        self._set_stop_requested_flag(True, client_name=client_name)

        kernel_status = self._call_with_timeout(
            f"{client_name}_status",
            self._kernel.get_status,
            _KERNEL_CONTROL_CALL_TIMEOUT_S,
        )
        if kernel_action == "pause_if_executing":
            # The in-flight execution may still be in its preparation phase
            # (planning/validating). Pausing only EXECUTING leaves the
            # kernel's async worker free to finish preparation and dispatch
            # the motion AFTER the cycle has reported stopped — the operator
            # sees the arm start/keep moving post-Stop. Preparation-phase
            # executions are stopped outright (Run from STOPPED replans from
            # fresh state anyway, so nothing is lost); actual motion is
            # paused to stay resumable. Re-check a bounded number of times
            # because the kernel can cross the preparation->executing
            # boundary between the status read and the action.
            for _ in range(3):
                kernel_status = self._call_with_timeout(
                    f"{client_name}_poll_status",
                    self._kernel.get_status,
                    _KERNEL_CONTROL_CALL_TIMEOUT_S,
                )
                if kernel_status.state == ExecutionState.EXECUTING:
                    self._call_with_timeout(
                        f"{client_name}_pause",
                        self._kernel.pause,
                        _KERNEL_CONTROL_CALL_TIMEOUT_S,
                    )
                    break
                if kernel_status.state in (
                    ExecutionState.PLANNING,
                    ExecutionState.VALIDATING,
                ):
                    self._call_with_timeout(
                        f"{client_name}_prep_stop",
                        self._kernel.stop,
                        _KERNEL_CONTROL_CALL_TIMEOUT_S,
                    )
                    continue
                break
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
                self._call_with_timeout(
                    f"{client_name}_stop",
                    self._kernel.stop,
                    _KERNEL_CONTROL_CALL_TIMEOUT_S,
                )
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

    def set_on_reset_phase_event(
        self,
        callback: Optional[Callable[[dict[str, Any]], None]],
    ) -> None:
        """Install a callback invoked when reset phases start/finish/fail."""
        self._on_reset_phase_event = callback

    def set_on_recovery_exhausted(
        self,
        callback: Optional[Callable[[str, str, str], None]],
    ) -> None:
        """Install a callback invoked when program_only recovery is exhausted.

        Callback receives (node_instance, node_id, trigger_reason_value).
        """
        self._on_recovery_exhausted = callback

    def set_recovery_context_provider(
        self,
        provider: Optional[Callable[[], dict[str, int]]],
    ) -> None:
        """Install the Composer-owned generation/context provider.

        The callback is invoked without the cycle lock. It must return quickly
        and must not call back into CycleManager.
        """
        self._recovery_context_provider = provider

    def set_on_recovery_offer_published(
        self,
        callback: Optional[Callable[[dict[str, Any]], None]],
    ) -> None:
        self._on_recovery_offer_published = callback

    def set_on_recovery_action_event(
        self,
        callback: Optional[Callable[[str, dict[str, Any]], None]],
    ) -> None:
        self._on_recovery_action_event = callback

    def _emit_recovery_action_event(
        self,
        event: str,
        details: dict[str, Any],
    ) -> None:
        self._queue_or_dispatch_recovery_event("action", event, details)

    def _handle_recovery_offer_published(self, offer: dict[str, Any]) -> None:
        self._queue_or_dispatch_recovery_event("offer", "published", offer)

    def _queue_or_dispatch_recovery_event(
        self,
        kind: str,
        event: str,
        details: dict[str, Any],
    ) -> None:
        deferred = getattr(self._recovery_event_local, "events", None)
        item = (kind, event, deepcopy(details))
        if deferred is not None:
            deferred.append(item)
            return
        self._dispatch_recovery_event(item)

    def _dispatch_recovery_event(
        self,
        item: tuple[str, str, dict[str, Any]],
    ) -> None:
        kind, event, details = item
        try:
            if kind == "offer":
                callback = self._on_recovery_offer_published
                if callback is not None:
                    callback(details)
                return
            callback = self._on_recovery_action_event
            if callback is not None:
                callback(event, details)
        except Exception:  # noqa: BLE001 - telemetry cannot change authority.
            logger.exception("Recovery %s event callback failed", kind)

    def _begin_recovery_event_deferral(self) -> list[tuple[str, str, dict[str, Any]]]:
        if getattr(self._recovery_event_local, "events", None) is not None:
            raise RuntimeError("nested recovery event deferral is not supported")
        events: list[tuple[str, str, dict[str, Any]]] = []
        self._recovery_event_local.events = events
        return events

    def _finish_recovery_event_deferral(
        self,
        events: list[tuple[str, str, dict[str, Any]]],
    ) -> None:
        del self._recovery_event_local.events
        for item in events:
            self._dispatch_recovery_event(item)

    def _active_runtime_fault_facts(self) -> tuple[tuple[str, ...], tuple[str, ...]]:
        faults = sorted(
            self._runtime_pause_registry.active_faults(),
            key=lambda item: (item.generation, item.fault_id),
        )
        return (
            tuple(fault.fault_id for fault in faults),
            tuple(fault.domain for fault in faults),
        )

    def _capture_recovery_facts(self) -> CycleRecoveryFacts:
        fault_ids, fault_domains = self._active_runtime_fault_facts()
        context = {
            "program_generation": 1,
            "deployment_generation": 1,
            "pose_source_apply_generation": 1,
        }
        provider = self._recovery_context_provider
        if provider is not None:
            try:
                supplied = provider()
                context = {
                    "program_generation": int(supplied.get("program_generation") or 0),
                    "deployment_generation": int(
                        supplied.get("deployment_generation") or 0
                    ),
                    "pose_source_apply_generation": int(
                        supplied.get("pose_source_apply_generation") or 0
                    ),
                }
            except Exception:  # noqa: BLE001 - missing authority fails closed.
                logger.exception("Recovery context provider failed")
                context = {
                    "program_generation": 0,
                    "deployment_generation": 0,
                    "pose_source_apply_generation": 0,
                }
        try:
            scene = self._kernel.get_scene_authority_status()
        except Exception:  # noqa: BLE001 - missing scene token fails closed.
            logger.exception("Scene authority status read failed during recovery projection")
            scene = {}

        with self._lock:
            intervention_active = bool(
                self._intervention is not None
                and self._intervention.status == "running"
            )
            signature = (
                self._state.value,
                self._motion_stop_state,
                self._has_resumable_execution_context_locked(),
                self._error_code,
                fault_ids,
                fault_domains,
                intervention_active,
                self._state == CycleState.DIRECT_CONTROL,
                self._run_transition_in_progress,
            )
            if signature != self._cycle_recovery_fact_signature:
                self._cycle_recovery_generation += 1
                self._cycle_recovery_fact_signature = signature
            cycle_generation = self._cycle_recovery_generation
            state = self._state.value
            motion_stop_state = self._motion_stop_state
            tree_resumable = self._tree is not None and not self._tree_is_terminal()
            has_resumable_context = (
                self._has_resumable_execution_context_locked()
                or (
                    self._state == CycleState.RUNNING
                    and tree_resumable
                    and (self._motion_stop_state == "held" or bool(fault_ids))
                )
            )
            cycle_error_code = self._error_code
            direct_control_active = self._state == CycleState.DIRECT_CONTROL
            lifecycle_pending = (
                self._run_transition_in_progress
                or self._state == CycleState.STOPPING
            )
        return CycleRecoveryFacts(
            cycle_state=state,
            motion_stop_state=motion_stop_state,
            has_resumable_context=has_resumable_context,
            cycle_error_code=cycle_error_code,
            active_runtime_fault_ids=fault_ids,
            active_runtime_fault_domains=fault_domains,
            intervention_active=intervention_active,
            direct_control_active=direct_control_active,
            lifecycle_command_pending=lifecycle_pending,
            cycle_generation=cycle_generation,
            program_generation=context["program_generation"],
            deployment_generation=context["deployment_generation"],
            scene_admission_epoch=int(scene.get("scene_admission_epoch") or 0),
            pose_source_apply_generation=context["pose_source_apply_generation"],
            live_collision_world_revision=int(scene.get("revision_id") or 0),
        )

    def refresh_recovery_offer(self) -> Optional[dict[str, Any]]:
        """Synchronously project the current immutable recovery offer.

        This is a bounded NRT authority read used at kernel-event and command
        boundaries. Browser polling only observes the result; it is not the
        mechanism that makes a kernel assessment safe or unsafe.
        """
        try:
            assessment = self._kernel.get_recovery_assessment()
        except Exception as exc:  # noqa: BLE001 - projection must fail closed.
            logger.exception("Kernel recovery assessment read failed")
            assessment = {
                "assessment_revision": 0,
                "control_command_epoch": 0,
                "execution_id": 0,
                "execution_state": "unknown",
                "recovery_class": "unknown_fault",
                "status": "waiting",
                "allowed_actions": [],
                "reason_code": f"recovery_assessment_unreadable:{type(exc).__name__}",
                "evidence_refs": [],
            }
        offer = self._recovery_coordinator.project(
            assessment,
            self._capture_recovery_facts(),
        )
        return offer.to_wire() if offer is not None else None

    def get_recovery_offer(self) -> Optional[dict[str, Any]]:
        offer = self._recovery_coordinator.current_offer()
        return offer.to_wire() if offer is not None else None

    @staticmethod
    def _recovery_offer_event_details(
        offer: Optional[CycleRecoveryOffer],
    ) -> dict[str, Any]:
        if offer is None:
            return {}
        return {
            "offer_id": offer.offer_id,
            "offer_revision": offer.offer_revision,
            "generated_at_ns": offer.generated_at_ns,
            "valid_until_ns": offer.valid_until_ns,
            "kernel_assessment_revision": offer.kernel_assessment_revision,
            "control_command_epoch": offer.control_command_epoch,
            "execution_id": offer.execution_id,
            "latch_id": offer.latch_id,
            "stop_certificate_id": offer.stop_certificate_id,
            "stop_owner_generation": offer.stop_owner_generation,
            "cycle_generation": offer.cycle_generation,
            "program_generation": offer.program_generation,
            "deployment_generation": offer.deployment_generation,
            "scene_admission_epoch": offer.scene_admission_epoch,
            "pose_source_apply_generation": offer.pose_source_apply_generation,
            "live_collision_world_revision": offer.live_collision_world_revision,
            "recovery_class": offer.recovery_class,
            "status": offer.status,
            "primary_action": offer.primary_action,
            "reason_code": offer.reason_code,
            "evidence_refs": list(offer.evidence_refs),
        }

    def _select_runtime_recovery_participant(
        self,
        *,
        offer: CycleRecoveryOffer,
        action_id: str,
    ) -> tuple[Optional[Any], Optional[dict[str, Any]]]:
        """Resolve an exact active node for the pre-fault stop window.

        RuntimePauseFault ids remain the normal path. This binding is needed
        only when a certified kernel stop becomes visible before the owning
        motion node gets another tick and can raise its fault. The caller owns
        `_tree_tick_lock`, so the selected node cannot advance between this
        preflight and post-kernel grant.
        """
        if action_id not in {
            "retry_motion_from_current_state",
            "acknowledge_and_continue",
            "replan_from_stopped_state",
        }:
            return None, None
        if any(ref.startswith("runtime_fault:") for ref in offer.evidence_refs):
            return None, None
        if self._tree is None or offer.execution_id is None:
            return None, None

        active_bindings: list[tuple[Any, Any]] = []
        for node in _iter_tree_nodes(self._tree.root):
            if not isinstance(node, RuntimePauseMixin):
                continue
            if node.status != py_trees.common.Status.RUNNING:
                continue
            binding_reader = getattr(node, "runtime_recovery_binding", None)
            if not callable(binding_reader):
                continue
            binding = binding_reader()
            if binding is not None:
                active_bindings.append((node, binding))
        if not active_bindings:
            return None, None

        exact = [
            item
            for item in active_bindings
            if item[1].execution_id == offer.execution_id
        ]
        if len(exact) == 1:
            return exact[0][0], None
        return None, {
            "code": "recovery_action_rejected",
            "message": "The active behavior execution changed before recovery",
            "details": {
                "reason": "runtime_recovery_participant_mismatch",
                "expected_execution_id": offer.execution_id,
                "active_execution_ids": sorted(
                    {binding.execution_id for _, binding in active_bindings}
                ),
                "matching_participant_count": len(exact),
            },
        }

    def _resume_after_accepted_recovery(
        self,
        *,
        offer: CycleRecoveryOffer,
        recovery_request_id: str,
        runtime_participant: Optional[Any] = None,
    ) -> bool:
        fault_ids = tuple(
            ref[len("runtime_fault:") :]
            for ref in offer.evidence_refs
            if ref.startswith("runtime_fault:")
        )
        cycle_owned_faults = tuple(
            fault
            for fault_id in fault_ids
            if (fault := self._runtime_pause_registry.get(fault_id)) is not None
            and fault.resume_owner == "cycle_manager"
        )
        with self._lock:
            resumable = (
                self._state == CycleState.STOPPED
                and self._tree is not None
                and not self._tree_is_terminal()
            ) or (
                self._state == CycleState.RUNNING
                and (self._motion_stop_state == "held" or bool(fault_ids))
            )
            if not resumable:
                return False
            if runtime_participant is not None and not runtime_participant.accept_runtime_recovery(
                recovery_request_id=recovery_request_id,
                expected_execution_id=int(offer.execution_id or 0),
            ):
                return False
            if fault_ids and not self._runtime_pause_registry.request_resume(
                fault_ids,
                recovery_request_id=recovery_request_id,
            ):
                return False
            for fault in cycle_owned_faults:
                if self._runtime_pause_registry.consume_resume(
                    fault.fault_id,
                    expected_generation=fault.generation,
                ) is None:
                    return False
                self._cycle_runtime_fault_ids = {
                    condition_id: fault_id
                    for condition_id, fault_id in self._cycle_runtime_fault_ids.items()
                    if fault_id != fault.fault_id
                }
            self._pending_delegated_recovery = None
            self._motion_stop_state = "idle"
            self._error = None
            self._error_code = None
            self._error_details = None
            self._tick_pause_event.set()
            if self._tick_thread is None or not self._tick_thread.is_alive():
                if self._tree is None:
                    return False
                self._start_tick_thread(self._tick_loop)
            self._state = CycleState.RUNNING
        self._set_stop_requested_flag(
            False,
            client_name="cycle_manager_recovery_resume",
        )
        self._set_motion_stop_active_flag(
            False,
            client_name="cycle_manager_recovery_resume",
        )
        return True

    @staticmethod
    def _recovery_request_fingerprint(
        request: dict[str, Any],
        *,
        principal_id: str,
    ) -> tuple[Any, ...]:
        raw_revision = request.get("offer_revision", request.get("offerRevision", 0))
        try:
            revision = int(raw_revision or 0)
        except (TypeError, ValueError):
            revision = 0
        return (
            str(request.get("offer_id") or request.get("offerId") or ""),
            revision,
            str(request.get("action_id") or request.get("actionId") or ""),
            CycleRecoveryCoordinator.bounded_operator_reason(request.get("reason")),
            principal_id,
        )

    def _begin_recovery_request(
        self,
        request_id: str,
        fingerprint: tuple[Any, ...],
    ) -> tuple[str, Optional[dict[str, Any]]]:
        if not request_id:
            return "new_uncached", None
        with self._recovery_request_lock:
            existing = self._recovery_requests.get(request_id)
            if existing is not None:
                existing_fingerprint, result = existing
                self._recovery_requests.move_to_end(request_id)
                if existing_fingerprint != fingerprint:
                    return "conflict", None
                if result is None:
                    return "in_progress", None
                return "replay", deepcopy(result)

            while len(self._recovery_requests) >= _RECOVERY_REQUEST_CACHE_LIMIT:
                evicted = False
                for candidate_id, (_, candidate_result) in tuple(
                    self._recovery_requests.items()
                ):
                    if candidate_result is not None:
                        del self._recovery_requests[candidate_id]
                        evicted = True
                        break
                if not evicted:
                    return "capacity_exhausted", None
            self._recovery_requests[request_id] = (fingerprint, None)
            return "new", None

    def _complete_recovery_request(
        self,
        request_id: str,
        fingerprint: tuple[Any, ...],
        result: dict[str, Any],
    ) -> None:
        if not request_id:
            return
        with self._recovery_request_lock:
            existing = self._recovery_requests.get(request_id)
            if existing is not None and existing[0] == fingerprint:
                self._recovery_requests[request_id] = (
                    fingerprint,
                    deepcopy(result),
                )
                self._recovery_requests.move_to_end(request_id)

    def apply_recovery_action(
        self,
        request: dict[str, Any],
        *,
        principal_id: str,
        reset_callback: Optional[Callable[[str], None]] = None,
    ) -> dict[str, Any]:
        """Apply an idempotent exact offer-bound recovery action."""
        if not principal_id:
            raise RuntimeError("Authenticated recovery principal is required")
        request_id = str(request.get("request_id") or request.get("requestId") or "")
        fingerprint = self._recovery_request_fingerprint(
            request,
            principal_id=principal_id,
        )
        disposition, cached = self._begin_recovery_request(request_id, fingerprint)
        if disposition == "replay":
            assert cached is not None
            return cached
        if disposition != "new" and disposition != "new_uncached":
            reason_by_disposition = {
                "conflict": "recovery_request_id_reused",
                "in_progress": "recovery_request_in_progress",
                "capacity_exhausted": "recovery_request_capacity_exhausted",
            }
            reason_code = reason_by_disposition[disposition]
            error = {
                "code": "recovery_action_rejected",
                "message": "Recovery request could not be admitted",
                "details": {"reason": reason_code},
            }
            result = {
                "requestId": request_id,
                "accepted": False,
                "cycleResumed": False,
                "resultCode": reason_code,
                "error": error,
                "recoveryOffer": self.get_recovery_offer(),
            }
            # An identical in-flight duplicate is the same attempt and must not
            # duplicate durable events. Conflicting reuse and capacity refusal
            # are distinct rejected attempts and remain visible.
            if disposition != "in_progress":
                offer = self._recovery_coordinator.current_offer()
                details = {
                    **self._recovery_offer_event_details(offer),
                    "request_id": request_id,
                    "action_id": str(
                        request.get("action_id") or request.get("actionId") or ""
                    ),
                    "principal_id": principal_id,
                    "operator_reason": (
                        CycleRecoveryCoordinator.bounded_operator_reason(
                            request.get("reason")
                        )
                    ),
                }
                self._emit_recovery_action_event("requested", details)
                self._emit_recovery_action_event(
                    "rejected",
                    {**details, "error": error},
                )
            return result
        try:
            result = self._apply_recovery_action_once(
                request,
                principal_id=principal_id,
                reset_callback=reset_callback,
            )
        except BaseException as exc:
            # The exact side-effect point may be unknowable for an unexpected
            # exception. Keep the request id consumed so a transport retry can
            # never repeat an action whose outcome is ambiguous.
            self._complete_recovery_request(
                request_id,
                fingerprint,
                {
                    "requestId": request_id,
                    "accepted": False,
                    "cycleResumed": False,
                    "resultCode": "recovery_action_outcome_unknown",
                    "error": {
                        "code": "recovery_action_outcome_unknown",
                        "message": str(exc) or type(exc).__name__,
                        "details": {
                            "reason": "recovery_action_outcome_unknown",
                            "exception_type": type(exc).__name__,
                        },
                    },
                    "recoveryOffer": self.get_recovery_offer(),
                },
            )
            raise
        self._complete_recovery_request(request_id, fingerprint, result)
        return result

    def _apply_recovery_action_once(
        self,
        request: dict[str, Any],
        *,
        principal_id: str,
        reset_callback: Optional[Callable[[str], None]] = None,
    ) -> dict[str, Any]:
        """Apply one exact offer-bound recovery action.

        Non-resetting kernel actions retain C++ authority and are generation-
        checked again by `ApplyRecoveryAction`. `retry_runtime_step` is a
        cycle-only action and can only wake the exact fault ids embedded in the
        current immutable offer while the kernel assessment has no active
        recovery authority.
        """
        request_id = str(request.get("request_id") or request.get("requestId") or "")
        action_id = str(request.get("action_id") or request.get("actionId") or "")
        reason = self._recovery_coordinator.bounded_operator_reason(
            request.get("reason")
        )
        deferred_events = self._begin_recovery_event_deferral()
        reset_context: Optional[
            tuple[CycleRecoveryOffer, dict[str, Any]]
        ] = None
        try:
            with self._run_transition_lock, self._tree_tick_lock:
                self.refresh_recovery_offer()
                offer = self._recovery_coordinator.current_offer()
                requested_details = {
                    **self._recovery_offer_event_details(offer),
                    "request_id": request_id,
                    "action_id": action_id,
                    "principal_id": principal_id,
                    "operator_reason": reason,
                }
                offered_action = next(
                    (
                        item
                        for item in offer.actions
                        if item.action_id == action_id
                    ),
                    None,
                ) if offer is not None else None
                requested_details["performs_backend_reset"] = (
                    offered_action.performs_backend_reset
                    if offered_action is not None
                    else None
                )
                self._emit_recovery_action_event("requested", requested_details)
                validation = self._recovery_coordinator.validate_request(
                    request, offer
                )
                if (
                    not validation.accepted
                    or validation.offer is None
                    or validation.action is None
                ):
                    error = validation.error or {
                        "code": "recovery_action_rejected",
                        "message": "Recovery action was rejected",
                        "details": {"reason": "recovery_action_rejected"},
                    }
                    self._emit_recovery_action_event(
                        "rejected", {**requested_details, "error": error}
                    )
                    return {
                        "requestId": request_id,
                        "accepted": False,
                        "cycleResumed": False,
                        "resultCode": "rejected",
                        "error": error,
                        "recoveryOffer": (
                            validation.offer.to_wire()
                            if validation.offer is not None
                            else None
                        ),
                    }

                accepted_offer = validation.offer
                action = validation.action
                runtime_participant, participant_error = (
                    self._select_runtime_recovery_participant(
                        offer=accepted_offer,
                        action_id=action.action_id,
                    )
                )
                if participant_error is not None:
                    self._emit_recovery_action_event(
                        "rejected",
                        {**requested_details, "error": participant_error},
                    )
                    return {
                        "requestId": request_id,
                        "accepted": False,
                        "cycleResumed": False,
                        "resultCode": "cycle_context_changed",
                        "error": participant_error,
                        "recoveryOffer": accepted_offer.to_wire(),
                    }
                if action.action_id == "reset_cycle":
                    # Reset is an explicit fail-safe lifecycle command. Execute
                    # it after releasing recovery/tree locks; reset owns its own
                    # bounded lifecycle phases and may rebuild the tree.
                    reset_context = (accepted_offer, requested_details)
                else:
                    kernel_result: Optional[dict[str, Any]] = None
                    kernel_action_accepted = False
                    if is_kernel_recovery_action(action.action_id):
                        kernel_request = {
                            "request_id": request_id,
                            "action": action.action_id,
                            "principal_id": principal_id,
                            "expected_assessment_revision": (
                                accepted_offer.kernel_assessment_revision
                            ),
                            "expected_control_command_epoch": (
                                accepted_offer.control_command_epoch
                            ),
                            "expected_execution_id": accepted_offer.execution_id or 0,
                            "expected_latch_id": accepted_offer.latch_id or "",
                            "expected_stop_certificate_id": (
                                accepted_offer.stop_certificate_id or ""
                            ),
                            "expected_stop_owner_generation": (
                                accepted_offer.stop_owner_generation
                            ),
                            "reason": reason,
                        }
                        try:
                            kernel_result = self._kernel_control_command(
                                "apply_recovery_action",
                                lambda _epoch: self._kernel.apply_recovery_action(
                                    kernel_request
                                ),
                            )
                        except Exception as exc:  # noqa: BLE001 - bounded RPC boundary.
                            error = {
                                "code": "recovery_action_transport_failed",
                                "message": str(exc) or type(exc).__name__,
                                "details": {
                                    "reason": "kernel_recovery_action_transport_failed",
                                    "exception_type": type(exc).__name__,
                                },
                            }
                            self.refresh_recovery_offer()
                            newest = self._recovery_coordinator.current_offer()
                            self._emit_recovery_action_event(
                                "rejected", {**requested_details, "error": error}
                            )
                            return {
                                "requestId": request_id,
                                "accepted": False,
                                "cycleResumed": False,
                                "resultCode": "transport_failed",
                                "error": error,
                                "recoveryOffer": (
                                    newest.to_wire() if newest else None
                                ),
                            }
                        kernel_action_accepted = bool(kernel_result.get("accepted"))
                        if not kernel_action_accepted:
                            error = kernel_result.get("error") or {
                                "code": "recovery_action_rejected",
                                "message": "Kernel rejected recovery action",
                                "details": {
                                    "reason": "kernel_recovery_action_rejected"
                                },
                            }
                            self.refresh_recovery_offer()
                            newest = self._recovery_coordinator.current_offer()
                            self._emit_recovery_action_event(
                                "rejected", {**requested_details, "error": error}
                            )
                            return {
                                "requestId": request_id,
                                "accepted": False,
                                "cycleResumed": False,
                                "resultCode": "kernel_rejected",
                                "error": error,
                                "recoveryOffer": (
                                    newest.to_wire() if newest else None
                                ),
                            }
                    elif action.action_id != "retry_runtime_step":
                        error = {
                            "code": "recovery_action_rejected",
                            "message": "Recovery action is not implemented",
                            "details": {"reason": "action_not_implemented"},
                        }
                        self._emit_recovery_action_event(
                            "rejected", {**requested_details, "error": error}
                        )
                        return {
                            "requestId": request_id,
                            "accepted": False,
                            "cycleResumed": False,
                            "resultCode": "action_not_implemented",
                            "error": error,
                            "recoveryOffer": accepted_offer.to_wire(),
                        }

                    cycle_resumed = self._resume_after_accepted_recovery(
                        offer=accepted_offer,
                        recovery_request_id=request_id,
                        runtime_participant=runtime_participant,
                    )
                    if not cycle_resumed and not kernel_action_accepted:
                        error = {
                            "code": "recovery_action_rejected",
                            "message": (
                                "Recovery context changed before the owning "
                                "runtime node could resume"
                            ),
                            "details": {
                                "reason": "cycle_context_changed_before_runtime_retry"
                            },
                        }
                        self.refresh_recovery_offer()
                        newest = self._recovery_coordinator.current_offer()
                        self._emit_recovery_action_event(
                            "rejected", {**requested_details, "error": error}
                        )
                        return {
                            "requestId": request_id,
                            "accepted": False,
                            "cycleResumed": False,
                            "resultCode": "cycle_context_changed",
                            "error": error,
                            "recoveryOffer": newest.to_wire() if newest else None,
                        }

                    self.refresh_recovery_offer()
                    newest = self._recovery_coordinator.current_offer()
                    result_code = (
                        "accepted"
                        if cycle_resumed
                        else "kernel_accepted_cycle_context_superseded"
                    )
                    self._emit_recovery_action_event(
                        "accepted",
                        {
                            **requested_details,
                            "performs_backend_reset": False,
                            "kernel_action_result": kernel_result,
                            "cycle_resumed": cycle_resumed,
                            "result_code": result_code,
                        },
                    )
                    return {
                        "requestId": request_id,
                        "accepted": True,
                        "cycleResumed": cycle_resumed,
                        "resultCode": result_code,
                        "error": None,
                        "recoveryOffer": newest.to_wire() if newest else None,
                    }

            assert reset_context is not None
            accepted_offer, requested_details = reset_context
            callback = reset_callback
            if callback is None:
                callback = lambda callback_request_id: self.reset(
                    request_id=callback_request_id
                )
            try:
                callback(request_id)
            except Exception as exc:  # noqa: BLE001 - reset remains fail-closed.
                error = {
                    "code": "recovery_reset_failed",
                    "message": str(exc) or type(exc).__name__,
                    "details": {
                        "reason": "recovery_reset_failed",
                        "exception_type": type(exc).__name__,
                    },
                }
                self.refresh_recovery_offer()
                newest = self._recovery_coordinator.current_offer()
                self._emit_recovery_action_event(
                    "rejected", {**requested_details, "error": error}
                )
                return {
                    "requestId": request_id,
                    "accepted": False,
                    "cycleResumed": False,
                    "resultCode": "reset_failed",
                    "error": error,
                    "recoveryOffer": newest.to_wire() if newest else None,
                }
            self._runtime_pause_registry.clear_all()
            with self._lock:
                self._cycle_runtime_fault_ids.clear()
            self.refresh_recovery_offer()
            newest = self._recovery_coordinator.current_offer()
            self._emit_recovery_action_event(
                "accepted",
                {
                    **requested_details,
                    "performs_backend_reset": True,
                    "kernel_action_result": None,
                    "cycle_resumed": False,
                    "result_code": "reset_completed",
                },
            )
            return {
                "requestId": request_id,
                "accepted": True,
                "cycleResumed": False,
                "resultCode": "reset_completed",
                "error": None,
                "recoveryOffer": newest.to_wire() if newest else None,
            }
        finally:
            self._finish_recovery_event_deferral(deferred_events)

    def run(self, *, principal_id: str = "operator") -> Optional[dict[str, Any]]:
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
        with self._lock:
            recovery_surface_active = (
                self._state == CycleState.STOPPED
                or self._motion_stop_state == "held"
            )
        if not recovery_surface_active:
            # Close the node-fault -> tick-loop promotion window. An operator
            # Run that arrives after the exact fault is registered must consume
            # the current offer, not become a successful no-op merely because
            # the next tree tick has not marked motion_stop_state=held yet.
            recovery_surface_active = bool(
                self._runtime_pause_registry.active_faults()
            )
        if recovery_surface_active:
            offer_wire = self.refresh_recovery_offer()
            if offer_wire is not None:
                primary_action = offer_wire.get("primaryAction")
                automatic_actions = {
                    "retry_motion_from_current_state",
                    "retry_after_scene_repair",
                    "replan_from_stopped_state",
                    "retry_runtime_step",
                }
                if (
                    offer_wire.get("status") == "retry_ready"
                    and primary_action in automatic_actions
                ):
                    result = self.apply_recovery_action(
                        {
                            "request_id": f"run-{uuid.uuid4().hex}",
                            "offer_id": offer_wire["offerId"],
                            "offer_revision": offer_wire["offerRevision"],
                            "action_id": primary_action,
                            "reason": "operator_run_primary_recovery",
                        },
                        principal_id=principal_id,
                    )
                    if not result.get("accepted"):
                        error = result.get("error") or {}
                        raise RuntimeError(
                            str(
                                error.get("message")
                                or "Recovery action was rejected"
                            )
                        )
                    return result
                raise RuntimeError(
                    "Run cannot choose the current recovery action. "
                    f"status={offer_wire.get('status')}, "
                    f"reason={offer_wire.get('reasonCode')}"
                )

        with self._run_transition_lock:
            prepare_fresh = False
            start_from_idle = False
            resume_from_stopped = False
            resume_motion_stop = False
            on_run_from_idle: Optional[Callable[[], None]] = None
            run_transition_started = False
            run_committed = False

            def finish_run_transition(committed: bool) -> None:
                pending_stop: Optional[str] = None
                with self._lock:
                    if self._run_transition_in_progress:
                        pending_stop = self._finish_run_transition_locked(
                            committed=committed
                        )
                if pending_stop == "full_stop":
                    self.full_stop()
                elif pending_stop == "motion_stop":
                    self.stop()

            try:
                with self._lock:
                    if self._state in {CycleState.IDLE, CycleState.STOPPED}:
                        self._run_transition_in_progress = True
                        self._pending_run_stop = None
                        run_transition_started = True

                with self._lock:
                    current_state = self._state
                    motion_stop_state = self._motion_stop_state
                    if current_state == CycleState.RUNNING:
                        if motion_stop_state == "held":
                            resume_motion_stop = True
                        elif motion_stop_state == "stopping":
                            raise RuntimeError("Cannot run: motion stop is still in progress")
                        else:
                            return
                    elif current_state == CycleState.IDLE:
                        if self._tick_thread is not None and self._tick_thread.is_alive():
                            raise RuntimeError(
                                "Cannot start new cycle: previous tick thread is still alive"
                            )
                        if self._tree is None:
                            prepare_fresh = True
                        elif self._tree_is_terminal():
                            return
                        start_from_idle = True
                        self._program_run_id = (
                            f"run-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}"
                            f"-{uuid.uuid4().hex[:8]}"
                        )
                        on_run_from_idle = self._on_run_from_idle
                    elif current_state == CycleState.STOPPED:
                        resume_from_stopped = True
                    else:
                        return

                if resume_motion_stop:
                    kernel_status = self._kernel_status_with_timeout(
                        "run_resume_motion_stop_status"
                    )
                    if kernel_status.state not in {
                        ExecutionState.PAUSED,
                        ExecutionState.IDLE,
                        ExecutionState.COMPLETED,
                    }:
                        raise RuntimeError(
                            "Motion Stop cannot resume without a current recovery "
                            f"offer (kernel state={kernel_status.state.value})"
                        )
                    if kernel_status.state == ExecutionState.PAUSED:
                        resume = self._kernel_control_command(
                            "run_resume_after_motion_stop",
                            lambda epoch: self._kernel.resume(expected_epoch=epoch),
                        )
                        if not resume.accepted:
                            raise RuntimeError(
                                "kernel.resume rejected: "
                                f"{resume.error}"
                            )
                    clear_motion_stop_flag = False
                    with self._lock:
                        if (
                            self._state != CycleState.RUNNING
                            or self._motion_stop_state != "held"
                        ):
                            return
                        self._motion_stop_state = "idle"
                        clear_motion_stop_flag = True
                        self._error = None
                        self._error_code = None
                        self._error_details = None
                    if clear_motion_stop_flag:
                        self._set_motion_stop_active_flag(
                            False,
                            client_name="cycle_manager_resume_motion_stop",
                        )
                    return

                if resume_from_stopped:
                    with self._lock:
                        if self._state != CycleState.STOPPED:
                            return

                    kernel_status = self._kernel_status_with_timeout(
                        "run_resume_status"
                    )
                    if kernel_status.state not in {
                        ExecutionState.PAUSED,
                        ExecutionState.IDLE,
                        ExecutionState.COMPLETED,
                    }:
                        raise RuntimeError(
                            "Stopped cycle cannot resume without a current recovery "
                            f"offer (kernel state={kernel_status.state.value})"
                        )

                    if kernel_status.state == ExecutionState.PAUSED:
                        resume = self._kernel_control_command(
                            "run_resume",
                            lambda epoch: self._kernel.resume(expected_epoch=epoch),
                        )
                        if not resume.accepted:
                            raise RuntimeError(
                                "kernel.resume rejected: "
                                f"{resume.error}"
                            )

                    bb = py_trees.blackboard.Client(name="cycle_manager_resume")
                    bb.register_key(
                        key="stop_requested",
                        access=py_trees.common.Access.WRITE,
                    )
                    bb.stop_requested = False
                    self._set_motion_stop_active_flag(
                        False,
                        client_name="cycle_manager_resume",
                    )

                    with self._lock:
                        if self._state != CycleState.STOPPED:
                            return
                        self._pending_delegated_recovery = None
                        self._motion_stop_state = "idle"
                        self._tick_pause_event.set()
                        if self._tick_thread is None or not self._tick_thread.is_alive():
                            if self._tree is None:
                                raise RuntimeError("Cannot resume stopped cycle without a tree")
                            self._start_tick_thread(self._tick_loop)
                        self._error = None
                        self._error_code = None
                        self._error_details = None
                        self._state = CycleState.RUNNING
                        run_committed = True
                    return

                if start_from_idle:
                    if prepare_fresh:
                        self._prepare_fresh_cycle()
                    else:
                        self._write_bb_keys()

                    if on_run_from_idle:
                        on_run_from_idle()

                    with self._lock:
                        if self._state != CycleState.IDLE:
                            return
                        self._start_tick_thread(self._tick_loop)
                        self._error = None
                        self._error_code = None
                        self._error_details = None
                        self._state = CycleState.RUNNING
                        run_committed = True
            finally:
                if run_transition_started:
                    finish_run_transition(run_committed)

    def stop(self) -> None:
        """Stop active robot motion while keeping the behavior tree ticking."""
        with self._lock:
            if self._state != CycleState.RUNNING:
                if (
                    self._run_transition_in_progress
                    and self._state in {CycleState.IDLE, CycleState.STOPPED}
                ):
                    self._record_pending_run_stop_locked("motion_stop")
                return
            if self._motion_stop_state in {"stopping", "held"}:
                return
            self._motion_stop_state = "stopping"

        self._set_motion_stop_active_flag(
            True,
            client_name="cycle_manager_motion_stop",
        )

        try:
            kernel_status = self._call_with_timeout(
                "motion_stop_initial_status",
                self._kernel.get_status,
                _KERNEL_CONTROL_CALL_TIMEOUT_S,
            )
            stoppable = {
                ExecutionState.EXECUTING,
                ExecutionState.PLANNING,
                ExecutionState.VALIDATING,
                ExecutionState.INTERRUPTED,
                ExecutionState.PAUSED,
                ExecutionState.STOPPING,
            }
            if kernel_status.state in stoppable:
                self._call_with_timeout(
                    "motion_stop_stop",
                    self._kernel.stop,
                    _KERNEL_CONTROL_CALL_TIMEOUT_S,
                )
            kernel_state = self._kernel_state_with_timeout("motion_stop_post_status")
        except TimeoutError:
            with self._lock:
                if self._motion_stop_state == "stopping":
                    self._start_motion_stop_watcher_locked()
                    return
            return
        except Exception:
            with self._lock:
                self._motion_stop_state = "idle"
            self._set_motion_stop_active_flag(
                False,
                client_name="cycle_manager_motion_stop_failed",
            )
            raise

        with self._lock:
            if self._motion_stop_state != "stopping":
                return
            if self._kernel_stop_terminal(kernel_state):
                start_watcher = False
            else:
                self._start_motion_stop_watcher_locked()
                start_watcher = True
        if not start_watcher:
            self._complete_motion_stop_from_kernel_state(kernel_state)

    def full_stop(self) -> None:
        """
        Stop program execution at the cycle layer — the Full Stop action.

        Full Stop preserves the old whole-cycle quiesce behavior. If RUNNING:
        set stop_requested=True on BB, suspend the tick thread, and route the
        in-flight motion through the kernel controlled stop. State stays
        STOPPING until the kernel reaches a terminal stop state, then -> STOPPED.
        If STOPPED or IDLE: no-op.
        """
        with self._lock:
            if self._state != CycleState.RUNNING:
                if (
                    self._run_transition_in_progress
                    and self._state in {CycleState.IDLE, CycleState.STOPPED}
                ):
                    self._record_pending_run_stop_locked("full_stop")
                return  # Not running, no-op
            self._state = CycleState.STOPPING
            self._motion_stop_state = "idle"

        self._set_motion_stop_active_flag(
            False,
            client_name="cycle_manager_full_stop",
        )

        # Full Stop is the industrial controlled stop, not a resumable
        # pause: `stop_if_active` invokes `kernel.stop()` for any active state,
        # which the C++ kernel routes through the operator_stop_profile
        # controlled-stop path (industrial default soft_on_path: a self-computed
        # jerk-continuous preempting stop trajectory; legacy hard stop when no
        # profile is set). This also subsumes the planning-phase stop (a stop
        # during PLANNING/VALIDATING stops preparation) with no prep->executing
        # slip, since kernel.stop() handles whatever state the kernel is in.
        # Motion soft-fail uses the cooperative motion-stop hold path so
        # continue-policy branches can keep ticking.
        try:
            self._quiesce_runtime(
                client_name="cycle_manager_stop",
                kernel_action="stop_if_active",
            )
        except TimeoutError:
            self._request_kernel_stop_best_effort("full_stop_quiesce_timeout_stop")
            with self._lock:
                if self._state == CycleState.STOPPING:
                    self._start_stop_settle_watcher_locked()
            return
        except Exception:
            with self._lock:
                if self._state == CycleState.STOPPING:
                    self._state = CycleState.RUNNING
            raise

        try:
            kernel_state = self._kernel_state_with_timeout("full_stop_post_status")
        except Exception:
            logger.exception("Failed to read kernel state after operator stop")
            kernel_state = ExecutionState.STOPPING

        with self._lock:
            if self._state != CycleState.STOPPING:
                return
            if self._kernel_stop_terminal(kernel_state):
                self._state = CycleState.STOPPED
            else:
                self._start_stop_settle_watcher_locked()

    def shutdown(self, timeout_s: float = 5.0) -> bool:
        """Quiesce the cycle for process shutdown.

        Ordered and independently guarded: full_stop() routes any in-flight
        motion through the kernel controlled stop; a direct kernel stop then
        covers the surfaces full_stop() does not own (direct-control
        interventions, held motion stops); finally the tick thread is
        signalled and joined within the budget. The pause event is left
        untouched on purpose — setting it would let a held tree tick once
        more between the loop-top stop check and the tick.

        Returns True when the tick thread exited within the budget.
        """
        deadline = time.monotonic() + timeout_s
        try:
            self.full_stop()
        except Exception:  # noqa: BLE001 — shutdown steps are independent
            logger.exception("Shutdown: cycle full_stop failed")
        try:
            self._quiesce_runtime(
                client_name="cycle_manager_shutdown",
                kernel_action="stop_if_active",
            )
        except Exception:  # noqa: BLE001 — shutdown steps are independent
            logger.exception("Shutdown: direct kernel stop failed")
        with self._lock:
            thread = self._tick_thread
            self._tick_stop_event.set()
            self._tick_pause_event.set()
        joined = True
        if thread is not None and thread.is_alive():
            thread.join(timeout=max(0.0, deadline - time.monotonic()))
            joined = not thread.is_alive()
            if not joined:
                logger.error(
                    "Shutdown: tick thread did not exit within %.1fs", timeout_s
                )
        return joined

    def _await_zombie_thread_exit(self, thread_to_join) -> None:
        thread_to_join.join()
        with self._lock:
            if self._tick_cleanup_thread is threading.current_thread():
                self._tick_cleanup_thread = None
            if self._tick_thread is thread_to_join:
                self._tick_thread = None
    
    def reset(self, *, request_id: Optional[str] = None) -> None:
        """Serialize reset against Run, recovery, and intervention admission."""
        with self._run_transition_lock:
            self._reset_impl(request_id=request_id)

    def _reset_impl(self, *, request_id: Optional[str] = None) -> None:
        """
        Reset program to initial state.
        
        If RUNNING: full_stop() first.
        Rebuilds the software/runtime scene state for the currently-loaded
        program without teleporting the robot.
        """
        with self._lock:
            if self._state == CycleState.STOPPING:
                raise RuntimeError("Cannot reset while cycle is stopping")
            if self._motion_stop_state == "stopping":
                raise RuntimeError("Cannot reset while motion stop is in progress")
            need_stop = (self._state == CycleState.RUNNING)

        if need_stop:
            self._run_reset_phase(
                "full_stop",
                self.full_stop,
                request_id=request_id,
                fail_closed=False,
            )

        thread_to_join = None
        with self._lock:
            # Stop tick thread if running
            if self._tick_thread is not None and self._tick_thread.is_alive():
                self._tick_stop_event.set()
                self._tick_pause_event.set()
                thread_to_join = self._tick_thread

        if thread_to_join is not None:
            tick_join_started = time.monotonic()
            self._emit_reset_phase_event(
                phase="tick_join",
                status="started",
                request_id=request_id,
            )
            if not self._tick_exit_event.wait(timeout=0.025):
                tick_join_duration_s = time.monotonic() - tick_join_started
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
                self._emit_reset_phase_event(
                    phase="tick_join",
                    status="failed",
                    request_id=request_id,
                    duration_s=tick_join_duration_s,
                    error="tick thread did not terminate",
                    error_code="tick_thread_zombie",
                )
                return
            with self._lock:
                if self._tick_thread is thread_to_join:
                    self._tick_thread = None
            self._emit_reset_phase_event(
                phase="tick_join",
                status="completed",
                request_id=request_id,
                duration_s=time.monotonic() - tick_join_started,
            )

        kernel_status = self._run_reset_phase(
            "kernel_status",
            self._kernel.get_status,
            request_id=request_id,
        )
        if kernel_status.state != ExecutionState.IDLE:
            kernel_reset_started = time.monotonic()
            reset_result = self._run_reset_phase(
                "kernel_reset",
                lambda: self._kernel_control_command(
                    "kernel_reset",
                    lambda epoch: self._kernel.reset(expected_epoch=epoch),
                    timeout_s=_RESET_PHASE_DEADLINE_S,
                ),
                request_id=request_id,
                emit_completed=False,
                bounded_by_callee=True,
            )
            kernel_reset_duration_s = time.monotonic() - kernel_reset_started
            if not reset_result.accepted:
                reason_code = (
                    reset_result.reason_code
                    or "kernel_reset_refused"
                )
                error_message = (
                    reset_result.error
                    or "Kernel reset was refused"
                )
                with self._lock:
                    self._error = error_message
                    self._error_code = reason_code
                    self._error_details = {
                        "reason": reason_code,
                        "kernel_state": kernel_status.state.value,
                    }
                    self._state = CycleState.STOPPED
                    self._pending_delegated_recovery = None
                self._emit_reset_phase_event(
                    phase="kernel_reset",
                    status="failed",
                    request_id=request_id,
                    duration_s=kernel_reset_duration_s,
                    error=error_message,
                    error_code=reason_code,
                )
                return
            self._emit_reset_phase_event(
                phase="kernel_reset",
                status="completed",
                request_id=request_id,
                duration_s=kernel_reset_duration_s,
            )

        self._prepare_fresh_cycle(
            reset_request_id=request_id,
            record_reset_phases=True,
        )

        with self._lock:
            self._tick_stop_event.clear()
            self._tick_pause_event.set()
            self._error = None
            self._error_code = None
            self._error_details = None
            self._state = CycleState.IDLE
            self._pending_delegated_recovery = None
        self.refresh_recovery_offer()

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
                "kernel.set_runtime_velocity_scale failed; viewer runtime "
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
        if self._signal_provider_poll_countdown <= 0:
            self._signal_registry.poll_providers(now_ns=now_ns)
            self._signal_provider_poll_countdown = self._signal_provider_poll_ticks - 1
        else:
            self._signal_registry.expire(now_ns=now_ns)
            self._signal_provider_poll_countdown -= 1

        counter = self._signal_registry.change_counter(now_ns=now_ns)
        if counter == self._last_signal_registry_change_counter:
            return
        self._sync_signal_blackboard(now_ns)
        self._last_signal_registry_change_counter = (
            self._signal_registry.change_counter(now_ns=now_ns)
        )

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
            if not self._tick_pause_event.wait(timeout=_PAUSED_TICK_WAIT_S):
                continue  # Paused, loop again

            self._refresh_signal_registry()

            # Tick the tree
            start_time = time.monotonic()
            try:
                with self._tree_tick_lock:
                    self._tree.tick()
            except Exception:
                error_msg = traceback.format_exc()
                logger.error("Tick loop exception: %s", error_msg)
                self._tick_stop_event.set()
                # Containment: the tree's integrity is unknown after an
                # arbitrary exception, but an in-flight kernel execution is
                # real either way — motion must not outlive its supervisor.
                # Each step runs independently so one failure cannot skip
                # the next.
                try:
                    # Cascade terminate() through running nodes; a motion
                    # node cancels its owned kernel goal there.
                    self._tree.root.stop(py_trees.common.Status.INVALID)
                except Exception:
                    logger.exception("Tick containment: tree halt failed")
                try:
                    # Belt-and-suspenders: the tree halt above is best-effort
                    # inside the nodes, so stop the kernel directly as well.
                    self._quiesce_runtime(
                        client_name="cycle_manager_tick_exception",
                        kernel_action="stop_if_active",
                    )
                except Exception:
                    logger.exception("Tick containment: kernel stop failed")
                with self._lock:
                    self._error = error_msg
                    self._error_code = "tick_loop_exception"
                    self._error_details = None
                    # STOPPED (not IDLE) per spec/410: every runtime failure
                    # surfaces on the STOPPED + _error contract. The tree is
                    # dropped because its post-exception state cannot be
                    # trusted for a resume: Run is refused with an explicit
                    # reason and Reset rebuilds from the authored program.
                    self._state = CycleState.STOPPED
                    self._tree = None
                    self._tick_thread = None
                break
            
            # Drain native kernel events
            kernel_events = self._drain_kernel_events()
            if self._fail_held_motion_stop_closed_if_intervention_latched(kernel_events):
                continue

            # Collect node statuses atomically (F-6: thread-safe observer snapshot)
            statuses: dict[str, str] = {}
            _collect_statuses(self._tree.root, statuses)
            with self._lock:
                self._node_statuses = statuses

            runtime_faults = sorted(
                self._runtime_pause_registry.active_faults(),
                key=lambda item: (item.generation, item.fault_id),
            )
            if runtime_faults:
                fault = runtime_faults[0]
                self._enter_motion_paused_state(
                    fault.message,
                    runtime_reason_code=fault.reason_code,
                    runtime_details={
                        **fault.details,
                        "runtime_fault_id": fault.fault_id,
                        "runtime_fault_generation": fault.generation,
                        "runtime_fault_domain": fault.domain,
                        "runtime_fault_node_name": fault.node_name,
                        "runtime_fault_node_id": fault.node_id,
                    },
                )
                try:
                    self.refresh_recovery_offer()
                except Exception:  # noqa: BLE001 - fault remains fail-closed.
                    logger.exception("Recovery offer projection failed for runtime fault")
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
                            clear_motion_stop_flag = False
                            with self._lock:
                                self._error = failure_feedback
                                self._error_code = (
                                    tick_code or "recovery_exhausted"
                                )
                                self._error_details = tick_details
                                self._state = CycleState.STOPPED
                                self._tick_thread = None
                                clear_motion_stop_flag = (
                                    self._clear_motion_stop_after_tree_terminal_locked()
                                )
                            if clear_motion_stop_flag:
                                self._set_motion_stop_active_flag(
                                    False,
                                    client_name="cycle_manager_recovery_exhausted",
                                )
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
                        clear_motion_stop_flag = False
                        with self._lock:
                            self._error = failure_feedback
                            self._error_code = (
                                delegated_code or "delegated_recovery_requested"
                            )
                            self._error_details = delegated_details
                            self._pending_delegated_recovery = request
                            self._state = CycleState.STOPPED
                            self._tick_thread = None
                            clear_motion_stop_flag = (
                                self._clear_motion_stop_after_tree_terminal_locked()
                            )
                        if clear_motion_stop_flag:
                            self._set_motion_stop_active_flag(
                                False,
                                client_name="cycle_manager_delegate_stop",
                            )
                        self._notify_delegated_recovery_requested(request)
                        break
                terminal_code, terminal_details = (
                    (None, None)
                    if status != py_trees.common.Status.FAILURE
                    else self._capture_kernel_error_diagnostics()
                )
                clear_motion_stop_flag = False
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
                    clear_motion_stop_flag = (
                        self._clear_motion_stop_after_tree_terminal_locked()
                    )
                if clear_motion_stop_flag:
                    self._set_motion_stop_active_flag(
                        False,
                        client_name="cycle_manager_tree_terminal",
                    )
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
        self.refresh_recovery_offer()

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
        with self._run_transition_lock:
            with self._lock:
                if self._state not in (CycleState.IDLE, CycleState.STOPPED):
                    raise RuntimeError(
                        f"Cannot start intervention from state {self._state.value}"
                    )

                entry_state = self._state.value
                if (
                    entry_state == CycleState.IDLE.value
                    and (
                        session_kind != "manual_intervention"
                        or authority_source != "human"
                        or approval_mode != "human_approved"
                    )
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

            kernel_state = self._kernel_state_with_timeout("intervention_entry_status")

            with self._lock:
                if self._state.value != entry_state:
                    raise RuntimeError(
                        "Cannot start intervention: cycle state changed to "
                        f"{self._state.value}"
                    )
                if (
                    session_kind == "delegated_recovery"
                    and self._pending_delegated_recovery is not pending_request
                ):
                    raise RuntimeError(
                        "Cannot start intervention: delegated recovery request changed"
                    )
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
                    self._tick_pause_event.set()
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

            if not self._tick_pause_event.wait(timeout=_PAUSED_TICK_WAIT_S):
                continue

            start_time = time.monotonic()
            try:
                with self._tree_tick_lock:
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
            self._tick_pause_event.set()
            thread_to_join = self._tick_thread

        if thread_to_join is not None:
            thread_to_join.join(timeout=2.0)

        with self._lock:
            if thread_to_join is not None and thread_to_join.is_alive():
                logger.error("Intervention tick thread did not terminate — zombie detected")
                self._error = "intervention tick thread did not terminate"

    def _write_bb_keys(self) -> None:
        """Write stop_requested=False, velocity_scale, program_run_id to blackboard."""
        bb = py_trees.blackboard.Client(name="cycle_manager_init")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.register_key(key="motion_stop_active", access=py_trees.common.Access.WRITE)
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.register_key(key="program_run_id", access=py_trees.common.Access.WRITE)
        bb.stop_requested = False
        bb.motion_stop_active = False
        bb.velocity_scale = self._velocity_scale
        bb.program_run_id = self._program_run_id or ""
