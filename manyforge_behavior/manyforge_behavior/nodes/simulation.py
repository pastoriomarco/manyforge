"""Simulator-control behavior nodes for ManyForge orchestration."""

from __future__ import annotations

from concurrent.futures import CancelledError, Future, ThreadPoolExecutor
import threading
from typing import Any

import py_trees

from manyforge_behavior.action_span import ActionSpanMixin
from manyforge_behavior.parameters import resolve_blackboard_value
from manyforge_behavior.runtime_pause import RuntimePauseMixin, RuntimePauseRegistry


_SIMULATION_CONTROL_EXECUTOR: ThreadPoolExecutor | None = None
_SIMULATION_CONTROL_EXECUTOR_LOCK = threading.Lock()


def _get_simulation_control_executor() -> ThreadPoolExecutor:
    global _SIMULATION_CONTROL_EXECUTOR
    with _SIMULATION_CONTROL_EXECUTOR_LOCK:
        if _SIMULATION_CONTROL_EXECUTOR is None:
            _SIMULATION_CONTROL_EXECUTOR = ThreadPoolExecutor(
                max_workers=1,
                thread_name_prefix="manyforge_sim_control",
            )
        return _SIMULATION_CONTROL_EXECUTOR


class _SimulationControlNode(
    RuntimePauseMixin,
    ActionSpanMixin,
    py_trees.behaviour.Behaviour,
):
    """Base node that delegates simulator control to Composer."""

    operation: str = ""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        runtime_pause_registry = kwargs.pop("runtime_pause_registry", None)
        self._params = kwargs
        self._reason = str(kwargs.get("reason", "") or name)
        self._runtime_failure_policy = str(
            kwargs.get("runtime_failure_policy", "pause_for_operator")
        )
        self._future: Future | None = None
        self._configure_runtime_pause(
            registry=(
                runtime_pause_registry
                if isinstance(runtime_pause_registry, RuntimePauseRegistry)
                else None
            ),
            node_id=str(kwargs.get("node_id", name)),
            domain="simulation",
        )

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="simulation_control_callback",
            access=py_trees.common.Access.READ,
        )

    def initialise(self) -> None:
        if self._future is not None and not self._future.done():
            self._future.cancel()
        self._reset_action_span()
        self._future = None

    def _runtime_failure(
        self,
        *,
        reason_code: str,
        message: str,
        details: dict[str, Any] | None = None,
    ) -> py_trees.common.Status:
        self.feedback_message = message
        if self._runtime_failure_policy == "condition_failure":
            return py_trees.common.Status.FAILURE
        if self._runtime_failure_policy != "pause_for_operator":
            self.feedback_message = (
                "invalid runtime_failure_policy: "
                f"{self._runtime_failure_policy!r}"
            )
            return py_trees.common.Status.FAILURE
        self._raise_runtime_pause(
            reason_code=reason_code,
            message=message,
            details={"operation": self.operation, **(details or {})},
        )
        return py_trees.common.Status.RUNNING

    def _callback(self) -> Any:
        try:
            callback = self.blackboard.simulation_control_callback
        except (KeyError, AttributeError):
            callback = None
        return callback if callable(callback) else None

    def _operation_payload(self) -> dict[str, Any]:
        return {}

    def update(self) -> py_trees.common.Status:
        if self._runtime_pause_fault is not None:
            if self._consume_runtime_resume() is None:
                self.feedback_message = self._runtime_pause_fault.message
                return py_trees.common.Status.RUNNING

        if self._future is None:
            callback = self._callback()
            if callback is None:
                self.feedback_message = "simulation control callback is not configured"
                return py_trees.common.Status.FAILURE
            try:
                payload = {
                    "operation": self.operation,
                    "reason": self._reason,
                    "source": "behavior_node",
                    "allow_running": True,
                    "action_span": self._current_action_span(self.blackboard),
                    **self._operation_payload(),
                }
            except Exception as exc:
                self.feedback_message = str(exc)
                return py_trees.common.Status.FAILURE
            self._future = _get_simulation_control_executor().submit(
                callback,
                **payload,
            )
            self.feedback_message = "simulation control in progress"
            return py_trees.common.Status.RUNNING

        if not self._future.done():
            return py_trees.common.Status.RUNNING

        try:
            if self._future.cancelled():
                return self._runtime_failure(
                    reason_code="simulation_control_cancelled",
                    message="simulation control cancelled",
                )
            result = self._future.result()
        except CancelledError:
            return self._runtime_failure(
                reason_code="simulation_control_cancelled",
                message="simulation control cancelled",
            )
        except Exception as exc:
            return self._runtime_failure(
                reason_code="simulation_control_callback_failed",
                message=str(exc),
                details={"exception_type": type(exc).__name__},
            )
        finally:
            self._future = None

        if isinstance(result, dict) and result.get("success") is False:
            return self._runtime_failure(
                reason_code="simulation_control_rejected",
                message=str(result.get("error") or "simulation control failed"),
                details={
                    "result_code": str(result.get("code") or ""),
                },
            )
        return py_trees.common.Status.SUCCESS

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if (
            self._future is not None
            and not self._future.done()
            and new_status != py_trees.common.Status.RUNNING
        ):
            self._future.cancel()
            self._future = None
        if new_status != py_trees.common.Status.RUNNING:
            self._clear_runtime_pause()


class ResetSimulation(_SimulationControlNode):
    """Reset the connected simulator through the Composer-authorized callback."""

    operation = "reset"


class SetSimulationState(_SimulationControlNode):
    """Set simulator lifecycle state through ``SetSimulationState``."""

    operation = "set_state"

    def _operation_payload(self) -> dict[str, Any]:
        state = resolve_blackboard_value(self.blackboard, self._params.get("state"))
        return {"state": state}


class StepSimulation(_SimulationControlNode):
    """Step a paused simulator through ``StepSimulation``."""

    operation = "step"

    def _operation_payload(self) -> dict[str, Any]:
        steps = resolve_blackboard_value(self.blackboard, self._params.get("steps", 1))
        return {"steps": steps}


class SetEntityState(_SimulationControlNode):
    """Set a simulator entity pose through ``SetEntityState``."""

    operation = "set_entity_state"

    def _operation_payload(self) -> dict[str, Any]:
        entity = resolve_blackboard_value(self.blackboard, self._params.get("entity", ""))
        pose = resolve_blackboard_value(self.blackboard, self._params.get("pose", {}))
        return {"entity": entity, "pose": pose}
