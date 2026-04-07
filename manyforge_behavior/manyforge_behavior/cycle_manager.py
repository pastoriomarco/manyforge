"""CycleManager — runtime lifecycle controller for continuous robot programs."""

from __future__ import annotations

import logging
import threading
import time
import traceback
from enum import Enum
from typing import Callable, Optional

import py_trees

logger = logging.getLogger(__name__)

from manyforge_behavior.kernel_client import (
    ExecutionSnapshot,
    ExecutionState,
    KernelClient,
    ObjectState,
    SceneEditRequest,
)
from manyforge_behavior.runner import OrchestratorRunner


class CycleState(Enum):
    """Lifecycle states for the CycleManager."""
    IDLE = "idle"
    RUNNING = "running"
    STOPPED = "stopped"


def _collect_statuses(node, result):
    """Recursively collect node statuses from a py_trees node and its children."""
    result[node.name] = node.status.value if node.status else "INVALID"
    if hasattr(node, 'children'):
        for child in node.children:
            _collect_statuses(child, result)


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
        self._tick_stop_event = threading.Event()  # signals tick thread to exit
        self._tick_pause_event = threading.Event()  # cleared = paused, set = running
        self._velocity_scale = 1.0
        self._lock = threading.Lock()
        self._on_run_from_idle: Optional[Callable[[], None]] = None
        self._error: Optional[str] = None
        self._node_statuses: dict[str, str] = {}

        # Set initial pause event to set (not paused)
        self._tick_pause_event.set()

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

        self._clear_runtime_scene()
        self._tree = self._runner.build_tree()
        self._runner.apply_scene()
        self._write_bb_keys()
        with self._lock:
            self._node_statuses = {}
    
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
        """Last error from the tick loop, or None."""
        return self._error

    def get_execution_snapshot(self) -> ExecutionSnapshot:
        """Get the latest observer-safe execution snapshot from the kernel."""
        return self._kernel.get_execution_snapshot()

    def get_object_pose(self, object_id: str, base_frame: str) -> Optional[dict]:
        """Get the current backend-authoritative object pose."""
        return self._kernel.get_object_pose(object_id, base_frame)

    def get_object_state(self, object_id: str) -> ObjectState:
        """Get the current backend-authoritative object state."""
        return self._kernel.get_object_state(object_id)

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
    
    def run(self) -> None:
        """
        Start or resume program execution.
        
        If IDLE: runner.load(), runner.build_tree(), runner.apply_scene(),
                 write BB keys, start tick thread. If the previous tree
                 already reached SUCCESS/FAILURE, a fresh tree/scene is
                 prepared before starting. State → RUNNING.
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

            if self._state == CycleState.IDLE:
                # Guard: refuse to start if a zombie tick thread is still alive
                if self._tick_thread is not None and self._tick_thread.is_alive():
                    raise RuntimeError("Cannot start new cycle: previous tick thread is still alive")
                prepare_fresh = self._tree is None or self._tree_is_terminal()
                start_from_idle = True
                on_run_from_idle = self._on_run_from_idle

            elif self._state == CycleState.STOPPED:
                # Resuming from stopped state
                bb = py_trees.blackboard.Client(name="cycle_manager_resume")
                bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
                bb.stop_requested = False

                kernel_status = self._kernel.get_status()
                if kernel_status.state == ExecutionState.PAUSED:
                    self._kernel.resume()

                self._tick_pause_event.set()
                self._state = CycleState.RUNNING
                return

        if start_from_idle:
            if prepare_fresh:
                self._prepare_fresh_cycle()
            else:
                self._write_bb_keys()

            if on_run_from_idle:
                on_run_from_idle()

            with self._lock:
                self._tick_stop_event.clear()
                self._tick_pause_event.set()
                self._tick_thread = threading.Thread(target=self._tick_loop, daemon=True)
                self._tick_thread.start()
                self._state = CycleState.RUNNING
    
    def stop(self) -> None:
        """
        Stop program execution.
        
        If RUNNING: set stop_requested=True on BB, suspend tick thread.
                    If kernel state is EXECUTING, call kernel.pause().
                    State → STOPPED.
        If STOPPED or IDLE: no-op.
        """
        with self._lock:
            if self._state != CycleState.RUNNING:
                return  # Not running, no-op
            
            # Set stop flag on blackboard
            bb = py_trees.blackboard.Client(name="cycle_manager_stop")
            bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
            bb.stop_requested = True
            
            # Suspend tick thread
            self._tick_pause_event.clear()
            
            # Pause kernel if executing
            kernel_status = self._kernel.get_status()
            if kernel_status.state == ExecutionState.EXECUTING:
                self._kernel.pause()
            
            self._state = CycleState.STOPPED
    
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
            thread_to_join.join(timeout=1.0)

        with self._lock:
            if thread_to_join is not None:
                if thread_to_join.is_alive():
                    logger.error("Tick thread did not terminate within timeout — zombie thread detected")
                    self._error = "tick thread did not terminate"
                    self._state = CycleState.IDLE
                    return
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
            self._state = CycleState.IDLE
    
    def set_velocity_scale(self, scale: float) -> None:
        """Set velocity_scale on blackboard. Clamped to [0.0, 1.0]."""
        scale = max(0.0, min(1.0, scale))
        self._velocity_scale = scale
        
        bb = py_trees.blackboard.Client(name="cycle_manager_vs")
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.velocity_scale = scale
    
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
            
            # Collect node statuses atomically (F-6: thread-safe observer snapshot)
            statuses: dict[str, str] = {}
            _collect_statuses(self._tree.root, statuses)
            with self._lock:
                self._node_statuses = statuses

            # Check if tree completed
            status = self._tree.root.status
            if status in (py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE):
                # Program completed — stop
                self._tick_stop_event.set()
                with self._lock:
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
    
    def _write_bb_keys(self) -> None:
        """Write stop_requested=False and velocity_scale to blackboard."""
        bb = py_trees.blackboard.Client(name="cycle_manager_init")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.stop_requested = False
        bb.velocity_scale = self._velocity_scale
