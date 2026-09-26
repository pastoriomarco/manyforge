"""First bounded Program-Binding caller for addressed native execution."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
import math
from typing import Any, Literal

from .kernel_client import ActionSpan, KernelClient, KernelClientResponseError


class OrchestratorRunner:
    """Submit one source-joint goal without owning runtime or planner state.

    Program execution identity comes from the Applied Program binding this
    run was admitted with, never from the execution owner's status: the
    owner reports runtime identity only (execution_identity.v2, H1a-IF.2).
    """

    def __init__(
        self, kernel: KernelClient, applied_program_binding: Mapping[str, Any]
    ) -> None:
        if not isinstance(kernel, KernelClient):
            raise TypeError("kernel must be the bounded addressed KernelClient")
        if not isinstance(applied_program_binding, Mapping):
            raise TypeError("applied_program_binding must be a Program binding")
        for field in (
            "program_id",
            "program_revision",
            "program_binding_id",
            "graph_revision",
            "retained_bundle_id",
        ):
            value = applied_program_binding.get(field)
            if not isinstance(value, str) or not value:
                raise ValueError(f"applied Program binding has no {field}")
        targets = applied_program_binding.get("target_role_bindings")
        if not isinstance(targets, list):
            raise ValueError("applied Program binding has no target role bindings")
        self._kernel = kernel
        self._binding = applied_program_binding

    def _bound_capability(
        self, *, component_role_id: str, target_role_id: str, capability_id: str
    ) -> Mapping[str, Any]:
        """The Program-bound capability of one authored target role."""

        matches = [
            capability
            for target in self._binding["target_role_bindings"]
            if isinstance(target, Mapping)
            and target.get("target_role_id") == target_role_id
            and target.get("component_role_id") == component_role_id
            for capability in target.get("capability_bindings", [])
            if isinstance(capability, Mapping)
            and capability.get("capability_id") == capability_id
        ]
        if len(matches) != 1:
            raise ValueError(
                "target_role_id is not bound to this capability in the "
                "applied Program"
            )
        return matches[0]

    def _selected_status(self) -> tuple[dict[str, Any], dict[str, Any]]:
        status_envelope = self._kernel.identity()
        status = status_envelope.get("identity")
        if not isinstance(status, dict):
            raise KernelClientResponseError(
                "headless execution status has no selected-owner projection"
            )
        if any(
            not isinstance(status.get(field), str) or not status[field]
            for field in ("owner_epoch", "graph_revision", "retained_bundle_id")
        ):
            raise KernelClientResponseError(
                "headless execution status has an incomplete runtime identity"
            )
        # The run's binding executes only against the publication it was
        # compiled for; a changed owner product is refused, never rebound.
        if (
            status["graph_revision"] != self._binding["graph_revision"]
            or status["retained_bundle_id"] != self._binding["retained_bundle_id"]
        ):
            raise KernelClientResponseError(
                "execution owner product differs from the applied Program binding"
            )
        scene_revision = status.get("scene_revision")
        if (
            not isinstance(scene_revision, dict)
            or not isinstance(scene_revision.get("scene_id"), str)
            or not scene_revision["scene_id"]
            or type(scene_revision.get("revision_id")) is not int
            or scene_revision["revision_id"] < 1
            or not isinstance(scene_revision.get("content_hash"), str)
            or not scene_revision["content_hash"]
        ):
            raise KernelClientResponseError(
                "headless execution status has no valid scene revision"
            )
        return status, scene_revision

    def submit_joint_goal(
        self,
        *,
        request_id: str,
        program_run_id: str,
        component_role_id: str,
        target_role_id: str,
        node_instance_id: str,
        node_attempt_id: str,
        move_id: str,
        joint_names: Sequence[str],
        positions_rad: Sequence[float],
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        """Resolve selection from owner status and submit one portable goal."""

        self._bound_capability(
            component_role_id=component_role_id,
            target_role_id=target_role_id,
            capability_id="execution.trajectory.execute",
        )
        status, scene_revision = self._selected_status()
        span = ActionSpan(
            action_source="program_node",
            action_span_id=node_attempt_id,
            program_run_id=program_run_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
        )
        intent = {
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": status["owner_epoch"],
                "source_label": "manyforge_behavior",
                "correlation_id": correlation_id,
            },
            "program_id": self._binding["program_id"],
            "program_revision": self._binding["program_revision"],
            "program_binding_id": self._binding["program_binding_id"],
            "program_run_id": program_run_id,
            "expected_graph_revision": status["graph_revision"],
            "expected_retained_bundle_id": status["retained_bundle_id"],
            "expected_scene_revision": scene_revision,
            "participants": [
                {
                    "component_role_id": component_role_id,
                    "target_role_id": target_role_id,
                    "required_capability_id": "execution.trajectory.execute",
                    "action_span": span.as_request(),
                    "payload": {
                        "kind": "motion",
                        "move_id": move_id,
                        "base_frame_role_id": "base",
                        "tcp_frame_role_id": "tcp",
                        "tolerances": {
                            "joint_position_tol_rad": 0.001,
                            "translation_tol_m": 0.001,
                            "rotation_tol_rad": 0.001,
                        },
                        "speed_profile": {
                            "velocity_scale": 0.25,
                            "acceleration_scale": 0.25,
                            "max_tcp_linear_speed_mps": None,
                        },
                        "timeouts": {
                            "planning_timeout_s": 5.0,
                            "execution_timeout_s": 10.0,
                        },
                        "cache_mode": "default",
                        "planning_profile": "first_feasible",
                        "target_mode": "absolute",
                        "target_reference": None,
                        "offset_reference": None,
                        "motion": {
                            "kind": "joint_goal",
                            "joint_names": list(joint_names),
                            "positions_rad": list(positions_rad),
                        },
                        "contact_policy_id": None,
                        "per_joint_dynamics_limits": {},
                        "motion_interrupt_scopes": [],
                    },
                }
            ],
            "coordination_policy": "independent",
        }
        return self._kernel.execute(intent)

    def submit_gripper_position(
        self,
        *,
        request_id: str,
        program_run_id: str,
        component_role_id: str,
        target_role_id: str,
        node_instance_id: str,
        node_attempt_id: str,
        position_kind: Literal["angular_rad", "linear_m"],
        position: float,
        max_effort: float | None = None,
        command_duration_s: float = 0.0,
        completion_timeout_s: float = 5.0,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        """Submit one unit-tagged gripper target through the addressed owner."""

        if position_kind not in ("angular_rad", "linear_m"):
            raise ValueError("position_kind must be angular_rad or linear_m")
        if not math.isfinite(position):
            raise ValueError("position must be finite")
        if (
            max_effort is not None
            and (not math.isfinite(max_effort) or max_effort < 0.0)
        ):
            raise ValueError("max_effort must be finite and nonnegative")
        if not math.isfinite(command_duration_s) or command_duration_s < 0.0:
            raise ValueError("command_duration_s must be finite and nonnegative")
        if not math.isfinite(completion_timeout_s) or completion_timeout_s <= 0.0:
            raise ValueError("completion_timeout_s must be finite and positive")

        capability = self._bound_capability(
            component_role_id=component_role_id,
            target_role_id=target_role_id,
            capability_id="tool.gripper.command",
        )
        command_schema = capability.get("command_schema")
        schema_id = (
            command_schema.get("command_schema_id")
            if isinstance(command_schema, Mapping)
            else None
        )
        schema_revision = (
            command_schema.get("command_schema_revision")
            if isinstance(command_schema, Mapping)
            else None
        )
        if not isinstance(schema_id, str) or not schema_id:
            raise ValueError("applied Program binding has no gripper command schema")
        if not isinstance(schema_revision, str) or not schema_revision:
            raise ValueError(
                "applied Program binding has no gripper command schema revision"
            )
        status, scene_revision = self._selected_status()

        if position_kind == "angular_rad":
            position_value: dict[str, Any] = {
                "kind": "angular_rad",
                "value_rad": position,
            }
            effort_value = (
                None
                if max_effort is None
                else {
                    "kind": "angular_newton_meter",
                    "value_nm": max_effort,
                }
            )
        else:
            position_value = {"kind": "linear_m", "value_m": position}
            effort_value = (
                None
                if max_effort is None
                else {"kind": "linear_newton", "value_n": max_effort}
            )

        span = ActionSpan(
            action_source="program_node",
            action_span_id=node_attempt_id,
            program_run_id=program_run_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
        )
        intent = {
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": status["owner_epoch"],
                "source_label": "manyforge_behavior",
                "correlation_id": correlation_id,
            },
            "program_id": self._binding["program_id"],
            "program_revision": self._binding["program_revision"],
            "program_binding_id": self._binding["program_binding_id"],
            "program_run_id": program_run_id,
            "expected_graph_revision": status["graph_revision"],
            "expected_retained_bundle_id": status["retained_bundle_id"],
            "expected_scene_revision": scene_revision,
            "participants": [
                {
                    "component_role_id": component_role_id,
                    "target_role_id": target_role_id,
                    "required_capability_id": "tool.gripper.command",
                    "action_span": span.as_request(),
                    "payload": {
                        "kind": "capability_command",
                        "command_schema_id": schema_id,
                        "command_schema_revision": schema_revision,
                        "command_value": {
                            "kind": "gripper_position",
                            "position": position_value,
                            "max_effort": effort_value,
                        },
                        "completion_timeout_s": completion_timeout_s,
                        "caller_options": [
                            {
                                "option_id": "command_duration_s",
                                "value_s": command_duration_s,
                            }
                        ],
                    },
                }
            ],
            "coordination_policy": "independent",
        }
        return self._kernel.execute(intent)

    def operation(self, operation_id: str) -> dict[str, Any]:
        return self._kernel.operation(operation_id)

    def _request_context(
        self,
        *,
        request_id: str,
        correlation_id: str | None,
    ) -> dict[str, str | None]:
        status = self._kernel.identity().get("identity")
        owner_epoch = status.get("owner_epoch") if isinstance(status, dict) else None
        if not isinstance(owner_epoch, str) or not owner_epoch:
            raise KernelClientResponseError(
                "headless execution status has no owner epoch"
            )
        return {
            "request_id": request_id,
            "request_owner_epoch": owner_epoch,
            "source_label": "manyforge_behavior",
            "correlation_id": correlation_id,
        }

    def _control_operation(
        self,
        control: str,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        reason: str | None,
        correlation_id: str | None,
    ) -> dict[str, Any]:
        request = {
            "request_context": self._request_context(
                request_id=request_id,
                correlation_id=correlation_id,
            ),
            "operation_id": operation_id,
            "expected_operation_revision": expected_operation_revision,
            "reason": reason,
        }
        callback = {
            "pause": self._kernel.pause,
            "resume": self._kernel.resume,
            "reset": self._kernel.reset,
            "acknowledge_completed": self._kernel.acknowledge_completed,
        }.get(control)
        if callback is None:
            raise ValueError("operation control is unsupported")
        return callback(operation_id, request)

    def pause_operation(
        self,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        reason: str | None = None,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        return self._control_operation(
            "pause",
            request_id=request_id,
            operation_id=operation_id,
            expected_operation_revision=expected_operation_revision,
            reason=reason,
            correlation_id=correlation_id,
        )

    def resume_operation(
        self,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        reason: str | None = None,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        return self._control_operation(
            "resume",
            request_id=request_id,
            operation_id=operation_id,
            expected_operation_revision=expected_operation_revision,
            reason=reason,
            correlation_id=correlation_id,
        )

    def reset_operation(
        self,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        reason: str | None = None,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        return self._control_operation(
            "reset",
            request_id=request_id,
            operation_id=operation_id,
            expected_operation_revision=expected_operation_revision,
            reason=reason,
            correlation_id=correlation_id,
        )

    def acknowledge_completed_operation(
        self,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        reason: str | None = None,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        return self._control_operation(
            "acknowledge_completed",
            request_id=request_id,
            operation_id=operation_id,
            expected_operation_revision=expected_operation_revision,
            reason=reason,
            correlation_id=correlation_id,
        )

    def stop_operation(
        self,
        *,
        request_id: str,
        operation_id: str,
        expected_operation_revision: int,
        expected_stop_generation: int,
        reason: str | None = None,
        correlation_id: str | None = None,
    ) -> dict[str, Any]:
        """Submit one scoped Stop; owner projections settle its outcome."""

        request = {
            "request_context": self._request_context(
                request_id=request_id,
                correlation_id=correlation_id,
            ),
            "operations": [
                {
                    "operation_id": operation_id,
                    "expected_operation_revision": expected_operation_revision,
                    "expected_stop_generation": expected_stop_generation,
                }
            ],
            "reason": reason,
        }
        return self._kernel.stop(request)


__all__ = ["OrchestratorRunner"]
