"""Runtime status routes.

This surface is intentionally deployment-agnostic.  Composer reports the
core/kernel/HMI state, while optional bringup supervisors can contribute
external component status through a narrow HTTP endpoint.  The frontend renders
the merged contract without knowing whether the deployment is Composer-only,
Isaac commanded simulation, or a real ROS-controlled robot.
"""
from __future__ import annotations

import time
from typing import Any

from fastapi import APIRouter, Body, HTTPException

from .routes_infra import build_infra_heartbeat_payload
from .state import ComposerState

_KERNEL_HEALTHY_STATUSES = {"ok", "degraded"}
_KERNEL_FAULT_STATUSES = {"error", "intervention_required"}


def _component(
    component_id: str,
    label: str,
    kind: str,
    status: str,
    *,
    required_for: list[str] | None = None,
    detail: str = "",
) -> dict[str, Any]:
    return {
        "id": component_id,
        "label": label,
        "kind": kind,
        "status": status,
        "requiredFor": list(required_for or []),
        "detail": detail,
    }


def _execution_profile(state: ComposerState) -> tuple[str, str, str, dict[str, Any]]:
    deployment = state.deployment
    if deployment is None:
        return (
            "composer_only",
            "Composer only",
            "backend_authored",
            {},
        )
    execution = deployment.adapters.get("execution")
    if execution is None:
        return (
            "composer_only",
            "Composer only",
            "backend_authored",
            {},
        )
    config = dict(execution.config or {})
    adapter_id = execution.adapter_id
    source_mode = str(config.get("execution_state_source_mode", "") or "")
    if source_mode == "commanded_sim":
        return (
            "commanded_sim",
            "Isaac commanded simulation",
            "backend_authored",
            config,
        )
    if adapter_id.startswith("execution.ros2_control"):
        return (
            "real_robot",
            "ROS controller / real robot",
            "controller_observed",
            config,
        )
    return (
        "composer_only",
        "Composer/native preview",
        "backend_authored",
        config,
    )


def build_runtime_status_payload(state: ComposerState) -> dict[str, Any]:
    now_ns = time.monotonic_ns()
    heartbeat = build_infra_heartbeat_payload(state)
    profile_kind, profile_label, authority, _execution_config = _execution_profile(state)
    program_loaded = state.program is not None
    hmi_ready = state.hmi_server is not None
    kernel_payload = heartbeat.get("components", {}).get("kernel", {})
    kernel_available = bool(kernel_payload.get("available"))
    kernel_status = str(heartbeat.get("status", "__missing__"))
    kernel_status_known = kernel_status in (
        _KERNEL_HEALTHY_STATUSES | _KERNEL_FAULT_STATUSES
    )
    kernel_fault = kernel_status in _KERNEL_FAULT_STATUSES or not kernel_status_known

    components: list[dict[str, Any]] = [
        _component(
            "composer",
            "Composer API",
            "composer",
            "ready",
            required_for=["render", "plan", "execute"],
            detail=f"Uptime {heartbeat.get('uptime_s', 0.0):.0f}s",
        ),
        _component(
            "program",
            "Program",
            "program",
            "ready" if program_loaded else "waiting",
            required_for=["render", "plan", "execute"],
            detail=state.program.name if state.program else "No program loaded",
        ),
        _component(
            "hmi",
            "HMI / Viser",
            "hmi",
            "ready" if hmi_ready else ("waiting" if program_loaded else "disabled"),
            required_for=["render"],
            detail=(
                f"Port {state.hmi_public_port}"
                if hmi_ready and state.hmi_public_port
                else (
                    "Starts after a program/deployment is loaded"
                    if not program_loaded
                    else "HMI server not started"
                )
            ),
        ),
        _component(
            "planner",
            "Planner / collision backend",
            "planner",
            "ready" if kernel_available and program_loaded else "waiting",
            required_for=["plan"],
            detail=str(kernel_payload.get("type", "Kernel unavailable")),
        ),
        _component(
            "execution_backend",
            "Execution backend",
            "execution",
            (
                "fault"
                if kernel_fault
                else "ready"
                if kernel_available and program_loaded
                else "waiting"
            ),
            required_for=["execute"],
            detail=str(kernel_payload.get("state", kernel_payload.get("error", "")) or ""),
        ),
    ]

    external_statuses = state.external_runtime_statuses(max_age_s=6.0)
    external_ready = True
    external_waiting = False
    external_fault = False
    external_user_message = ""
    for provider_id, provider_status in sorted(external_statuses.items()):
        provider_readiness = str(provider_status.get("readiness", "") or "")
        if provider_readiness in {"fault", "error"}:
            external_fault = True
        elif provider_readiness in {"waiting", "degraded", "offline"}:
            external_waiting = True
        if provider_status.get("canExecute") is False:
            external_ready = False
        message = str(provider_status.get("userMessage", "") or "")
        if message and not external_user_message:
            external_user_message = message
        raw_components = provider_status.get("components", [])
        if isinstance(raw_components, list):
            for raw_component in raw_components:
                if isinstance(raw_component, dict):
                    components.append(dict(raw_component))
        else:
            components.append(
                _component(
                    provider_id,
                    provider_id,
                    "external",
                    provider_readiness or "waiting",
                    detail="External provider status did not include components",
                )
            )

    if profile_kind == "commanded_sim" and "isaac_demo_supervisor" not in external_statuses:
        external_ready = False
        external_waiting = True
        external_user_message = "Waiting for Isaac demo supervisor status."
        components.append(
            _component(
                "isaac_demo_supervisor",
                "Isaac supervisor",
                "simulator",
                "waiting",
                required_for=["execute"],
                detail="No fresh supervisor status received",
            )
        )

    can_render = bool(program_loaded and hmi_ready)
    can_plan = bool(program_loaded and kernel_available)
    can_execute = bool(program_loaded and kernel_available)
    if profile_kind == "commanded_sim":
        can_execute = can_execute and external_ready
    if kernel_fault:
        can_execute = False

    if kernel_status == "intervention_required":
        readiness = "fault"
        summary_label = "Fault"
        user_message = "Operator intervention is required before execution can continue."
    elif kernel_fault or external_fault:
        readiness = "fault"
        summary_label = "Fault"
        user_message = (
            f"Kernel reported unknown status '{kernel_status}'."
            if not kernel_status_known
            else external_user_message or "A runtime component reported a fault."
        )
    elif external_waiting or (profile_kind == "commanded_sim" and not can_execute):
        readiness = "waiting"
        summary_label = "Waiting"
        user_message = (
            external_user_message
            or "Rendering/planning are available; execution is waiting for external readiness."
        )
    elif can_render and can_plan and can_execute:
        readiness = "ready"
        summary_label = "Ready"
        user_message = "Runtime is ready."
    elif program_loaded:
        readiness = "degraded"
        summary_label = "Degraded"
        user_message = "Program is loaded, but one or more runtime capabilities are unavailable."
    else:
        readiness = "waiting"
        summary_label = "Waiting"
        user_message = "Load a program/deployment to enable runtime capabilities."

    return {
        "schemaVersion": "manyforge.runtime_status.v1",
        "profileId": state.deployment.name if state.deployment else "composer_only",
        "profileKind": profile_kind,
        "profileLabel": profile_label,
        "authority": authority,
        "readiness": readiness,
        "summaryLabel": summary_label,
        "canRender": can_render,
        "canPlan": can_plan,
        "canExecute": can_execute,
        "userMessage": user_message,
        "components": components,
        "externalProviders": sorted(external_statuses.keys()),
        "timestampNs": now_ns,
    }


def create_runtime_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/status")
    def runtime_status() -> dict[str, Any]:
        """Return merged Composer/kernel/external runtime status."""
        return build_runtime_status_payload(state)

    @router.post("/external/{provider_id}")
    def update_external_runtime_status(
        provider_id: str,
        payload: dict[str, Any] = Body(...),
    ) -> dict[str, Any]:
        """Accept a bringup/provider runtime-status contribution."""
        try:
            state.update_external_runtime_status(provider_id, payload)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        return {"status": "ok", "providerId": provider_id}

    return router
