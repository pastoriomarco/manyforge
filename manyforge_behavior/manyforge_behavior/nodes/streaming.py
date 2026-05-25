"""Streaming-session orchestration BT leaves (ADR-0016).

The leaf in this module is **lifecycle-only**: it opens a streaming session
on the kernel at setup, returns RUNNING for the whole drag/teleop window,
and ends the session at terminate. Per-target updates do NOT flow through
the BT — they're pushed directly into the kernel by the Composer WS handler
(or any other live-input surface) via `kernel.push_streaming_target(...)`.
This honors ADR-0005's kernel-ownership rule: the BT orchestrates session
lifetime; the kernel owns the streaming target register and execution
semantics.

Typical mounting:
- A long-running intervention is started via
  `cycle_manager.execute_intervention(tree_dict={"id":"track_target_action", ...})`
  when the operator grabs the gizmo (or first slider press).
- The intervention's BT contains just this leaf at the root.
- The leaf calls `kernel.begin_streaming_session(...)` in setup; if accepted
  it stores the session_id on the blackboard under "streaming_session_id"
  so the WS handler can read it and push targets.
- On terminate (cycle stop / reset / explicit end), the leaf calls
  `kernel.end_streaming_session(session_id, soft_stop_strategy=...)`.
"""

from __future__ import annotations

import time
from typing import Any, Optional

import py_trees

from manyforge_behavior.kernel_client import (
    ExecutionState,
    KernelClient,
    StreamingSessionConfig,
)


class TrackTargetAction(py_trees.behaviour.Behaviour):
    """Open a kernel streaming session, hold it RUNNING until released.

    Catalog id: `track_target_action`
    Parameters (read from node params dict):
        group_id (str, required): robot group id this session drives
        joint_names (list[str], optional): joints in the session; empty =>
            all drivable joints
        input_mode (str, optional, default "cartesian"): "cartesian" | "joint"
        cartesian_reference_frame (str, optional, default "base")
        soft_stop_strategy (str, optional, default "spring_back"):
            "spring_back" | "velocity_scaling"
        soft_stop_deceleration_time_s (float, optional, default 0.4)
        anchor_negative_time_s (float, optional, default -0.010)
        target_time_from_start_min_s (float, optional, default 0.030)
        max_dispatch_rate_hz (float, optional, default 30.0)
        target_max_age_s (float, optional, default 0.200)
        velocity_scale (float, optional, default 1.0)
        principal_id (str, required for audit)
        authority_source (str, optional, default "human")
        trigger_reason (str, optional, default "operator_direct_control")
        max_session_duration_s (float, optional, default 0 = no limit):
            safety net to auto-end stalled sessions. Honored by the leaf's
            tick; not enforced by the kernel.

    Blackboard:
        Writes "streaming_session_id" with the kernel-assigned session id
            so Composer WS handlers / LLM-assistant callers can target
            push_streaming_target.

    Status semantics:
        SUCCESS only if `end_streaming_session` is invoked externally (e.g.
            via a separate stop signal or the WS close → cycle stop path).
        FAILURE if begin_streaming_session is rejected.
        RUNNING otherwise.
    """

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._params = kwargs
        self._kernel: Optional[KernelClient] = None
        self._session_id: int = 0
        self._begin_error: Optional[str] = None
        self._end_error: Optional[str] = None
        self._started_at: Optional[float] = None
        self._terminated_externally: bool = False

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="streaming_session_id",
            access=py_trees.common.Access.WRITE,
        )
        self.blackboard.register_key(
            key="streaming_session_active",
            access=py_trees.common.Access.WRITE,
        )

    def _resolve_kernel(self) -> Optional[KernelClient]:
        if self._kernel is not None:
            return self._kernel
        try:
            kernel = self.blackboard.kernel  # type: ignore[attr-defined]
        except (AttributeError, KeyError):
            kernel = None
        self._kernel = kernel
        return kernel

    def _build_config(self) -> StreamingSessionConfig:
        params = self._params
        return StreamingSessionConfig(
            group_id=str(params.get("group_id", "")),
            joint_names=list(params.get("joint_names", []) or []),
            input_mode=str(params.get("input_mode", "cartesian")),
            cartesian_reference_frame=str(
                params.get("cartesian_reference_frame", "base")
            ),
            tcp_frame=str(params.get("tcp_frame", "") or ""),
            soft_stop_strategy=str(
                params.get("soft_stop_strategy", "spring_back")
            ),
            soft_stop_deceleration_time_s=float(
                params.get("soft_stop_deceleration_time_s", 0.4)
            ),
            anchor_negative_time_s=float(
                params.get("anchor_negative_time_s", -0.010)
            ),
            target_time_from_start_min_s=float(
                params.get("target_time_from_start_min_s", 0.030)
            ),
            max_dispatch_rate_hz=float(
                params.get("max_dispatch_rate_hz", 30.0)
            ),
            target_max_age_s=float(params.get("target_max_age_s", 0.200)),
            velocity_scale=float(params.get("velocity_scale", 1.0)),
            principal_id=str(params.get("principal_id", "")),
            authority_source=str(params.get("authority_source", "human")),
            trigger_reason=str(
                params.get("trigger_reason", "operator_direct_control")
            ),
        )

    def initialise(self) -> None:
        self._begin_error = None
        self._end_error = None
        self._terminated_externally = False
        kernel = self._resolve_kernel()
        if kernel is None:
            self._begin_error = "kernel not bound on blackboard"
            return
        config = self._build_config()
        if not config.principal_id:
            self._begin_error = (
                "principal_id is required for streaming sessions "
                "(operator/LLM identity must be recorded for audit)"
            )
            return
        result = kernel.begin_streaming_session(config)
        if not result.accepted:
            self._begin_error = result.error or (
                "kernel rejected begin_streaming_session"
            )
            return
        self._session_id = int(result.session_id)
        self._started_at = time.monotonic()
        try:
            self.blackboard.streaming_session_id = self._session_id  # type: ignore[attr-defined]
            self.blackboard.streaming_session_active = True  # type: ignore[attr-defined]
        except (AttributeError, KeyError):
            pass

    def update(self) -> py_trees.common.Status:
        if self._begin_error is not None:
            self.feedback_message = self._begin_error
            return py_trees.common.Status.FAILURE
        if self._session_id == 0:
            self.feedback_message = "streaming session not initialised"
            return py_trees.common.Status.FAILURE

        kernel = self._resolve_kernel()
        if kernel is None:
            self.feedback_message = "kernel disappeared mid-session"
            return py_trees.common.Status.FAILURE

        # Auto-end if a max-duration safety net is configured. Useful for
        # LLM-driven sessions or as a defensive guard against stuck WS
        # handlers that fail to close cleanly.
        max_duration = float(self._params.get("max_session_duration_s", 0.0))
        if (
            max_duration > 0.0
            and self._started_at is not None
            and (time.monotonic() - self._started_at) > max_duration
        ):
            if self._end_session(reason="max_session_duration exceeded"):
                return py_trees.common.Status.SUCCESS
            self.feedback_message = self._end_error or (
                "failed to end streaming session"
            )
            return py_trees.common.Status.FAILURE

        # Reflect the kernel's actual state. If the kernel has left
        # kStreaming (operator hit Stop / Reset, or the backend faulted),
        # surface a terminal status.
        try:
            status = kernel.get_status()
        except Exception as exc:  # pragma: no cover — defensive
            self.feedback_message = f"kernel get_status failed: {exc}"
            return py_trees.common.Status.FAILURE
        if status.state == ExecutionState.STREAMING:
            return py_trees.common.Status.RUNNING
        if status.state in (
            ExecutionState.STOPPING,
            ExecutionState.STOPPED,
        ):
            # External stop completed — the kernel already ran the soft-stop
            # trajectory. Mark SUCCESS so the intervention cleanly closes.
            self._terminated_externally = True
            return py_trees.common.Status.SUCCESS
        if status.state in (
            ExecutionState.INTERRUPTED,
            ExecutionState.FAILED,
            ExecutionState.INTERVENTION_REQUIRED,
        ):
            self.feedback_message = (
                f"streaming session terminated in {status.state.value}"
            )
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING

    def terminate(self, new_status: py_trees.common.Status) -> None:
        # Only the leaf-owned end path. If the external stop already drove
        # the kernel out of kStreaming we don't double-end (kernel will
        # reject session_id mismatch); otherwise we honor the leaf's
        # responsibility to release the session.
        if self._session_id != 0 and not self._terminated_externally:
            self._end_session(
                reason=f"BT terminate (new_status={new_status.name})"
            )
        try:
            self.blackboard.streaming_session_active = False  # type: ignore[attr-defined]
        except (AttributeError, KeyError):
            pass

    def _end_session(self, reason: str) -> bool:
        kernel = self._resolve_kernel()
        if kernel is None or self._session_id == 0:
            return True
        strategy = str(
            self._params.get("soft_stop_strategy", "spring_back")
        )
        try:
            result = kernel.end_streaming_session(self._session_id, strategy)
        except Exception as exc:  # pragma: no cover — defensive
            self._end_error = (
                f"end_streaming_session raised during {reason}: {exc}"
            )
            self.feedback_message = self._end_error
            return False
        if not result.accepted:
            self._end_error = result.error or (
                f"kernel rejected end_streaming_session during {reason}"
            )
            self.feedback_message = self._end_error
            return False
        self._session_id = 0
        self._started_at = None
        self._end_error = None
        return True
