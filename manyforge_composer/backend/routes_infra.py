"""Infrastructure status routes — /api/infra/*"""
from __future__ import annotations

import logging
import time
from typing import Any

from fastapi import APIRouter

from .state import ComposerState

logger = logging.getLogger(__name__)


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

    return router
