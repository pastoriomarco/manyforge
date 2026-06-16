"""Runtime status routes.

This surface is intentionally deployment-agnostic.  Composer reports the
core/kernel/native-viewer state, while optional bringup supervisors can
contribute external component status through a narrow HTTP endpoint.  The
frontend renders the merged contract without knowing whether the deployment is
Composer-only, Isaac commanded simulation, or a real ROS-controlled robot.
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
    source_mode = str(
        config.get("execution_state_authority_profile")
        or config.get("execution_state_source_mode")
        or ""
    )
    if source_mode == "commanded_sim":
        return (
            "commanded_sim",
            "Isaac commanded simulation",
            "backend_authored",
            config,
        )
    if adapter_id.startswith("execution.ros2_control"):
        if source_mode == "observed_ros2":
            return (
                "observed_ros2",
                "ROS controller observed authority",
                "controller_observed",
                config,
            )
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


def _native_viewer_detail(state: ComposerState) -> str:
    _snapshot, snapshot_meta = state.visual_state_snapshot()
    snapshot_meta = snapshot_meta or {}
    sequence_number = snapshot_meta.get("sequence_number") if snapshot_meta else None
    if snapshot_meta.get("publisher_running"):
        if sequence_number is not None:
            return f"visual snapshot publisher running, frame {sequence_number}"
        return "visual snapshot publisher running"
    if snapshot_meta.get("last_error"):
        return f"visual snapshot publisher last error: {snapshot_meta['last_error']}"
    broker = getattr(state, "snapshot_broker", None)
    if broker is not None:
        try:
            return (
                "native snapshot bridge connected"
                if broker.has_clients()
                else "native snapshot bridge ready"
            )
        except Exception:
            return "native snapshot bridge ready"
    return "native scene state route ready"


def build_runtime_status_payload(state: ComposerState) -> dict[str, Any]:
    now_ns = time.monotonic_ns()
    heartbeat = build_infra_heartbeat_payload(state)
    profile_kind, profile_label, authority, _execution_config = _execution_profile(state)
    program_loaded = state.program is not None
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
            "native_viewer",
            "Native viewer",
            "viewer",
            "ready" if program_loaded and kernel_available else (
                "waiting" if program_loaded else "disabled"
            ),
            required_for=["render"],
            detail=(
                _native_viewer_detail(state)
                if program_loaded
                else "Starts after a program/deployment is loaded"
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

    stop_cap = state.stop_capability_status
    stop_modes_unsupported = list(stop_cap.get("unsupported") or [])
    supported_stop_modes = stop_cap.get("supported_stop_modes")
    hard_stop_level = stop_cap.get("hard_stop_level")
    derivative_proof_level = stop_cap.get("derivative_proof_level")
    scaling_source_visibility = stop_cap.get("scaling_source_visibility")
    scaling_source_probe_status = stop_cap.get("scaling_source_probe_status")
    stop_capability_unsupported = bool(stop_modes_unsupported)
    stop_capability_query_failed = bool(stop_cap.get("capability_query_failed"))
    advertised = ", ".join(supported_stop_modes) if supported_stop_modes else "none"
    unsupported_desc = ", ".join(
        f"{u.get('profile')}={u.get('stop_mode')}" for u in stop_modes_unsupported
    )
    # The component renders whenever a real backend was queried (success: modes
    # known; failure: capability_query_failed) for a program-loaded deployment.
    if program_loaded and (supported_stop_modes is not None or stop_capability_query_failed):
        if stop_capability_query_failed:
            stop_detail = (
                "Execution backend could not report its supported stop modes; the "
                f"configured stop profile(s) ({unsupported_desc}) cannot be verified."
            )
        elif stop_capability_unsupported:
            stop_detail = (
                f"Configured stop profile unsupported by backend: {unsupported_desc}. "
                f"Backend advertises: {advertised}."
            )
        else:
            evidence = []
            if hard_stop_level:
                evidence.append(f"hard stop: {hard_stop_level}")
            if derivative_proof_level:
                evidence.append(f"derivative proof: {derivative_proof_level}")
            if scaling_source_visibility:
                evidence.append(f"scaling: {scaling_source_visibility}")
            suffix = f" ({'; '.join(evidence)})" if evidence else ""
            stop_detail = f"Supported stop modes: {advertised}.{suffix}"
        components.append(
            _component(
                "stop_capability",
                "Stop capability",
                "safety",
                "fault" if stop_capability_unsupported else "ready",
                required_for=["execute"],
                detail=stop_detail,
            )
        )

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

    can_render = bool(program_loaded and kernel_available)
    can_plan = bool(program_loaded and kernel_available)
    can_execute = bool(program_loaded and kernel_available)
    if profile_kind == "commanded_sim":
        can_execute = can_execute and external_ready
    if kernel_fault:
        can_execute = False
    if stop_capability_unsupported:
        # Fail closed: never let a deployment whose configured stop profile is
        # not advertised by the backend reach an armed/executing state.
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
    elif stop_capability_unsupported:
        readiness = "degraded"
        summary_label = "Degraded"
        if stop_capability_query_failed:
            user_message = (
                "Execution disabled: the execution backend could not report its "
                f"supported stop modes, so the configured stop profile(s) "
                f"({unsupported_desc}) cannot be verified. Reload once the backend "
                "is healthy."
            )
        else:
            user_message = (
                "Execution disabled: the deployment's configured stop profile uses a "
                f"stop mode the execution backend does not support ({unsupported_desc}). "
                f"Backend advertises: {advertised}. Choose a supported stop profile in "
                "the Deployment tab."
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
        "supportedStopModes": supported_stop_modes,
        "configuredStopModes": stop_cap.get("configured_stop_modes") or {},
        "configuredStopDecelerationTimes": stop_cap.get("configured_stop_deceleration_times") or {},
        "stopCapabilityProfileId": stop_cap.get("capability_profile_id"),
        "hardStopLevel": hard_stop_level,
        "derivativeProofLevel": derivative_proof_level,
        "scalingSourceVisibility": scaling_source_visibility,
        "scalingSourceProbeStatus": scaling_source_probe_status,
        "stopProfileUnsupportedReasons": stop_modes_unsupported,
        "backendUnsupportedStopReasons": stop_cap.get("unsupported_reasons") or {},
        "stopProfileUnsupported": stop_capability_unsupported,
        "stopCapabilityQueryFailed": stop_capability_query_failed,
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
