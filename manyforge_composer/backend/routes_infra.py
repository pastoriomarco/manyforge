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

    # HMI / Viser
    if state.hmi_server is not None:
        components["hmi"] = {
            "status": "healthy",
            "port": state.hmi_public_port,
        }
    else:
        components["hmi"] = {
            "status": "unavailable",
            "port": None,
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
        "status": "ok",
        "components": components,
        "timestamp": now,
    }


def create_infra_router(state: ComposerState) -> APIRouter:
    """Create the infrastructure status router."""
    router = APIRouter()

    @router.get("/status")
    def infra_status():
        """Get infrastructure health status for all components."""
        return build_infra_status_payload(state)

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
