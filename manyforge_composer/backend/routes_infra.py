"""Infrastructure status routes — /api/infra/*"""
from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter
from fastapi.concurrency import run_in_threadpool

from .state import ComposerState
from .models import ModelEndpointCatalogResponse

logger = logging.getLogger(__name__)


def _probe_openai_compatible_models(base_url: str | None) -> tuple[list[str], str | None]:
    if not base_url:
        return [], "No base_url configured"
    try:
        import httpx
    except Exception as exc:  # pragma: no cover - dependency issue
        return [], f"httpx unavailable: {exc}"
    models_url = f"{base_url.rstrip('/')}/models"
    with httpx.Client(timeout=5.0) as client:
        response = client.get(models_url)
        response.raise_for_status()
        payload = response.json()
    data = payload.get("data", [])
    model_ids = [
        item.get("id")
        for item in data
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    ]
    return sorted(set(model_ids)), None


def _model_endpoint_capabilities_payload(endpoint) -> dict[str, Any]:
    capabilities = endpoint.capabilities
    return {
        "supportsResponseTools": capabilities.supports_response_tools,
        "supportsJsonObject": capabilities.supports_json_object,
        "supportsImageInput": capabilities.supports_image_input,
        "supportedImageTransports": list(capabilities.supported_image_transports),
        "toolCallFamily": capabilities.tool_call_family,
        "latencyBudgetMs": capabilities.latency_budget_ms,
    }


def build_infra_status_payload(state: ComposerState) -> dict[str, Any]:
    """Build infrastructure health status."""
    now = time.time()
    heartbeat = build_infra_heartbeat_payload(state)

    components: dict[str, Any] = {}

    # ManyForge Composer
    components["composer"] = {
        "status": "healthy",
        "catalog_version": state.catalog.version if state.catalog else None,
        "program_loaded": state.program is not None,
        "program_name": state.program.name if state.program else None,
        "cycle_state": (
            state.cycle_manager.state.value if state.cycle_manager else "none"
        ),
    }

    # Kernel
    if state.kernel is not None:
        try:
            snapshot = state.kernel.get_execution_snapshot()
            components["kernel"] = {
                "status": "healthy",
                "type": type(state.kernel).__name__,
                "timestamp_ns": snapshot.timestamp_ns,
            }
        except Exception as e:
            components["kernel"] = {
                "status": "error",
                "error": str(e),
            }
    else:
        components["kernel"] = {"status": "unavailable"}

    # MCP HTTP
    components["mcp_http"] = {
        "status": "enabled" if hasattr(state, "_mcp_http_enabled") else "unknown",
    }

    visual_state, visual_meta = state.visual_state_snapshot()
    visual_meta = visual_meta or {}
    broker = getattr(state, "snapshot_broker", None)
    broker_has_clients = False
    if broker is not None:
        try:
            broker_has_clients = bool(broker.has_clients())
        except Exception:
            broker_has_clients = False
    components["native_viewer"] = {
        "status": "healthy" if state.program is not None and state.kernel is not None else "waiting",
        "visual_state_cached": visual_state is not None,
        "visual_state_publisher_running": bool(visual_meta.get("publisher_running", False)),
        "visual_state_sequence_number": visual_meta.get("sequence_number"),
        "snapshot_bridge_ready": broker is not None,
        "snapshot_bridge_client_connected": broker_has_clients,
    }

    # Deployment
    if state.deployment is not None:
        components["deployment"] = {
            "status": "loaded",
            "source": state.deployment_source_path,
            "adapter_count": len(state.deployment.adapters),
        }
    else:
        components["deployment"] = {"status": "none"}

    return {
        "status": heartbeat["status"],
        "components": components,
        "heartbeat": heartbeat,
        "timestamp": now,
        "timestamp_ns": heartbeat["timestamp_ns"],
        "uptime_s": heartbeat["uptime_s"],
    }


def build_infra_heartbeat_payload(state: ComposerState) -> dict[str, Any]:
    """Build a compact heartbeat/status payload for launchers and supervisors."""
    now_ns = time.monotonic_ns()
    started_at_ns = int(getattr(state, "started_at_ns", now_ns))
    uptime_s = max(0.0, (now_ns - started_at_ns) / 1e9)
    kernel_payload: dict[str, Any] = {"available": state.kernel is not None}
    top_status = "ok"

    if state.kernel is not None:
        try:
            status = state.kernel.get_status()
            snapshot = state.kernel.get_execution_snapshot()
            state_value = (
                status.state.value
                if hasattr(status.state, "value")
                else str(status.state)
            )
            error_details = getattr(snapshot, "error_details", {}) or {}
            requires_intervention = (
                state_value == "intervention_required"
                or str(error_details.get("requires_operator_intervention", "")).lower()
                == "true"
            )
            resync_required = (
                str(error_details.get("resync_required", "")).lower() == "true"
            )
            if requires_intervention:
                top_status = "intervention_required"
            elif getattr(snapshot, "error", None):
                top_status = "degraded"
            kernel_payload = {
                "available": True,
                "type": type(state.kernel).__name__,
                "state": state_value,
                "execution_id": int(getattr(status, "execution_id", 0) or 0),
                "active_move_id": getattr(status, "active_move_id", "") or "",
                "progress": float(getattr(status, "progress", 0.0) or 0.0),
                "snapshot_timestamp_ns": int(getattr(snapshot, "timestamp_ns", 0) or 0),
                "snapshot_age_ms": (
                    max(0.0, (now_ns - int(getattr(snapshot, "timestamp_ns", now_ns))) / 1e6)
                    if getattr(snapshot, "timestamp_ns", None)
                    else None
                ),
                "error": getattr(snapshot, "error", None),
                "error_code": getattr(snapshot, "error_code", None),
                "error_details": dict(error_details),
                "requires_operator_intervention": requires_intervention,
                "resync_required": resync_required,
            }
        except Exception as exc:
            top_status = "error"
            kernel_payload = {
                "available": True,
                "status": "error",
                "type": type(state.kernel).__name__,
                "error": str(exc),
            }

    archive_status = {}
    recorder = getattr(state, "event_recorder", None)
    if recorder is not None:
        try:
            archive_status = recorder.failure_archive_status()
            archive_degraded = (
                bool(archive_status.get("enabled"))
                and (
                    int(archive_status.get("dropped_archives") or 0) > 0
                    or not bool(archive_status.get("worker_alive", True))
                )
            )
            archive_status["status"] = "degraded" if archive_degraded else "healthy"
            if archive_degraded and top_status == "ok":
                top_status = "degraded"
        except Exception as exc:  # pragma: no cover - defensive status surface
            archive_status = {"enabled": False, "status": "error", "error": str(exc)}
            if top_status == "ok":
                top_status = "degraded"

    pipeline_status = {}
    if state.cycle_manager is not None:
        try:
            pipeline_status = state.cycle_manager.kernel_event_pipeline_status()
            if recorder is not None:
                pipeline_status.update(recorder.kernel_event_gap_status())
            pipeline_dropped = (
                int(pipeline_status.get("collector_dropped_total") or 0)
                + int(pipeline_status.get("manager_dropped_total") or 0)
                # A sequence-gap record without a drop-counter advance is
                # unexplained loss; it degrades the component just the same.
                + int(pipeline_status.get("gap_records_total") or 0)
            )
            pipeline_status["status"] = (
                "degraded" if pipeline_dropped > 0 else "healthy"
            )
            if pipeline_dropped > 0 and top_status == "ok":
                top_status = "degraded"
        except Exception as exc:  # pragma: no cover - defensive status surface
            pipeline_status = {"status": "error", "error": str(exc)}
            if top_status == "ok":
                top_status = "degraded"

    stop_latency = {}
    if recorder is not None:
        try:
            # Informational only: stop latency is a performance signal, not a
            # health fault, so it never degrades the deployment status.
            stop_latency = recorder.stop_latency_summary()
        except Exception as exc:  # pragma: no cover - defensive status surface
            stop_latency = {"error": str(exc)}

    trace_storage = {}
    if recorder is not None:
        try:
            trace_storage = recorder.trace_storage_status()
            storage_degraded = bool(trace_storage.get("enabled")) and (
                bool((trace_storage.get("last_prune") or {}).get("error"))
                or (
                    trace_storage.get("max_bytes") is not None
                    and int(trace_storage.get("total_bytes") or 0)
                    > int(trace_storage["max_bytes"])
                )
            )
            # `volatile` (e.g. /tmp) is reported on the component but does
            # not degrade the deployment: every dev/test lane runs on /tmp
            # and a permanently degraded heartbeat would drown real signals.
            trace_storage["status"] = (
                "degraded" if storage_degraded else "healthy"
            )
            if storage_degraded and top_status == "ok":
                top_status = "degraded"
        except Exception as exc:  # pragma: no cover - defensive status surface
            trace_storage = {"enabled": False, "status": "error", "error": str(exc)}
            if top_status == "ok":
                top_status = "degraded"

    return {
        "schema_version": "manyforge.infra_heartbeat.v1",
        "status": top_status,
        "timestamp": time.time(),
        "timestamp_ns": now_ns,
        "uptime_s": uptime_s,
        "cycle_state": (
            state.cycle_manager.state.value if state.cycle_manager else "none"
        ),
        "program_loaded": state.program is not None,
        "program_name": state.program.name if state.program else None,
        "deployment_source": state.deployment_source_path or None,
        "components": {
            "composer": {
                "status": "healthy",
                "catalog_version": state.catalog.version if state.catalog else None,
            },
            "kernel": kernel_payload,
            "failure_archive": archive_status,
            "kernel_event_pipeline": pipeline_status,
            "trace_storage": trace_storage,
            "stop_latency": stop_latency,
        },
    }


def create_infra_router(state: ComposerState) -> APIRouter:
    """Create the infrastructure status router."""
    router = APIRouter()

    @router.get("/status")
    def infra_status():
        """Get infrastructure health status for all components."""
        return build_infra_status_payload(state)

    @router.get("/heartbeat")
    def infra_heartbeat():
        """Get a compact heartbeat/status payload for launchers and supervisors."""
        return build_infra_heartbeat_payload(state)

    @router.get(
        "/model-endpoints",
        response_model=ModelEndpointCatalogResponse,
        response_model_by_alias=True,
    )
    async def infra_model_endpoints():
        """Return configured model endpoints plus best-effort live /v1/models discovery."""
        endpoints: dict[str, Any] = {}
        deployment = state.deployment
        if deployment is not None:
            for alias, endpoint in deployment.model_endpoints.items():
                probe_status = "skipped"
                available_model_ids: list[str] = []
                error: str | None = None
                if endpoint.provider in {"openai", "local"}:
                    try:
                        available_model_ids, error = await run_in_threadpool(
                            _probe_openai_compatible_models,
                            endpoint.base_url,
                        )
                        probe_status = "ok" if error is None else "error"
                    except Exception as exc:  # pragma: no cover - defensive
                        probe_status = "error"
                        error = str(exc)
                endpoints[alias] = {
                    "alias": alias,
                    "provider": endpoint.provider,
                    "configuredModelId": endpoint.model_id,
                    "baseUrl": endpoint.base_url,
                    "capabilities": _model_endpoint_capabilities_payload(endpoint),
                    "probeStatus": probe_status,
                    "availableModelIds": available_model_ids,
                    "error": error,
                }
        return {
            "endpoints": endpoints,
            "timestamp": time.time(),
        }

    return router
