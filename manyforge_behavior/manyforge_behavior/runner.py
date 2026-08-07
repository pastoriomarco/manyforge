"""First bounded Program-Binding caller for addressed native execution."""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

from .kernel_client import ActionSpan, KernelClient, KernelClientResponseError


class OrchestratorRunner:
    """Submit one source-joint goal without owning runtime or planner state."""

    def __init__(self, kernel: KernelClient) -> None:
        if not isinstance(kernel, KernelClient):
            raise TypeError("kernel must be the bounded addressed KernelClient")
        self._kernel = kernel

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

        status_envelope = self._kernel.status()
        status = status_envelope.get("status")
        if not isinstance(status, dict):
            raise KernelClientResponseError(
                "headless execution status has no selected-owner projection"
            )
        required_text = (
            "owner_epoch",
            "program_id",
            "program_revision",
            "program_binding_id",
            "graph_revision",
            "retained_bundle_id",
            "motion_target_role_id",
        )
        if any(
            not isinstance(status.get(field), str) or not status[field]
            for field in required_text
        ):
            raise KernelClientResponseError(
                "headless execution status has an incomplete selected-owner identity"
            )
        if status.get("motion_target_role_id") != target_role_id:
            raise ValueError("target_role_id is not the selected motion target")
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
            "program_id": status["program_id"],
            "program_revision": status["program_revision"],
            "program_binding_id": status["program_binding_id"],
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

    def operation(self, operation_id: str) -> dict[str, Any]:
        return self._kernel.operation(operation_id)


__all__ = ["OrchestratorRunner"]
