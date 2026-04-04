"""CycleManager — runtime lifecycle controller for continuous robot programs."""

from __future__ import annotations

import threading
import time
from enum import Enum
from typing import Callable, Optional

import py_trees

from manyforge_behavior.kernel_client import ExecutionState, KernelClient, SceneEditRequest
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

        # Set initial pause event to set (not paused)
        self._tick_pause_event.set()
    
    @property
    def state(self) -> CycleState:
        """Current lifecycle state."""
        return self._state
    
    @property
    def velocity_scale(self) -> float:
        """Current velocity scale (0.0 to 1.0)."""
        return self._velocity_scale
    
    def get_node_statuses(self) -> dict[str, str]:
        """Walk the py_trees tree and collect per-node status.
        
        Returns:
            Dict mapping node instance name to status string.
            Status values: "SUCCESS", "FAILURE", "RUNNING", "INVALID"
        """
        if self._tree is None:
            return {}
        
        result: dict[str, str] = {}
        _collect_statuses(self._tree.root, result)
        return result
    
    def run(self) -> None:
        """
        Start or resume program execution.
        
        If IDLE: runner.load(), runner.build_tree(), runner.apply_scene(),
                 write BB keys, start tick thread. State → RUNNING.
        If STOPPED: clear stop_requested on BB, resume tick thread. State → RUNNING.
                    If kernel is PAUSED, call kernel.resume().
        If RUNNING: no-op.
        """
        with self._lock:
            if self._state == CycleState.RUNNING:
                return  # Already running, no-op
            
            if self._state == CycleState.IDLE:
                # First run — build and start everything
                self._runner.load()
                self._tree = self._runner.build_tree()
                self._runner.apply_scene()
                self._write_bb_keys()

                # Notify listener (e.g. Viser scene refresh)
                if self._on_run_from_idle:
                    self._on_run_from_idle()

                # Start tick thread
                self._tick_stop_event.clear()
                self._tick_pause_event.set()
                self._tick_thread = threading.Thread(target=self._tick_loop, daemon=True)
                self._tick_thread.start()
                
            elif self._state == CycleState.STOPPED:
                # Resuming from stopped state
                # Clear stop flag on blackboard
                bb = py_trees.blackboard.Client(name="cycle_manager_resume")
                bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
                bb.stop_requested = False
                
                # Resume kernel if paused
                kernel_status = self._kernel.get_status()
                if kernel_status.state == ExecutionState.PAUSED:
                    self._kernel.resume()
                
                # Resume tick thread
                self._tick_pause_event.set()
            
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
        If STOPPED or RUNNING: stop tick thread, kernel.reset(),
                               runner.build_tree(), runner.apply_scene(),
                               write BB keys. State → IDLE.
        If IDLE: no-op.
        """
        with self._lock:
            if self._state == CycleState.IDLE:
                return  # Already idle, no-op
            
            # If running, stop first
            if self._state == CycleState.RUNNING:
                # We need to stop without holding the lock to avoid deadlock
                # since stop() also acquires the lock
                pass  # Will handle below
            
            # Release lock temporarily to call stop()
        if self._state == CycleState.RUNNING:
            self.stop()
        
        with self._lock:
            # Stop tick thread if running
            if self._tick_thread is not None and self._tick_thread.is_alive():
                self._tick_stop_event.set()
                self._tick_thread.join(timeout=1.0)
                self._tick_thread = None
            
            # Reset kernel
            self._kernel.reset()

            # Clear scene objects from the previous run (C++ kernel reset
            # doesn't clear the planning scene, so apply_scene() would fail
            # with "already exists" on re-add).
            # Also remove "_attached" variants left by AttachObjectToLink.
            if self._runner.program:
                for res in self._runner.program.scene:
                    for suffix in ("", "_attached"):
                        self._kernel.scene_edit(SceneEditRequest(
                            operation="remove",
                            object_id=res.object_id + suffix))

            # Clear tree — run() from IDLE will rebuild fresh
            self._tree = None

            # Reset events
            self._tick_stop_event.clear()
            self._tick_pause_event.set()

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
            self._tree.tick()
            
            # Check if tree completed
            status = self._tree.root.status
            if status in (py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE):
                # Program completed — stop
                self._tick_stop_event.set()
                break
            
            # Sleep remainder of period
            elapsed = time.monotonic() - start_time
            sleep_time = period_s - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)
    
    def _write_bb_keys(self) -> None:
        """Write stop_requested=False and velocity_scale to blackboard."""
        bb = py_trees.blackboard.Client(name="cycle_manager_init")
        bb.register_key(key="stop_requested", access=py_trees.common.Access.WRITE)
        bb.register_key(key="velocity_scale", access=py_trees.common.Access.WRITE)
        bb.stop_requested = False
        bb.velocity_scale = self._velocity_scale
