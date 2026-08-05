"""Read-only projection of the immutable build-owned node environment.

This is a future Composer client route, not a discovery owner. It deliberately
contains no filesystem roots, source text, runtime availability, reload path,
or Python implementation factory.
"""

from __future__ import annotations

from fastapi import APIRouter, HTTPException

from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .state import ComposerState


def _environment_payload(state: ComposerState) -> dict:
    environment = state.node_environment
    manifest = environment.manifest
    return {
        "environmentDigest": environment.environment_digest,
        "catalog": {
            "apiVersion": manifest.catalog_api_version,
            "catalogId": manifest.catalog_id,
            "contentVersion": manifest.catalog_content_version,
            "catalogDigest": manifest.catalog_digest,
        },
        "executableRegistry": [
            row.to_dict() for row in manifest.executable_registry
        ],
        "executablePackages": [
            row.to_dict() for row in manifest.executable_packages
        ],
        "sourceCoverage": [
            row.to_dict() for row in manifest.source_coverage
        ],
    }


def create_nodes_router(state: ComposerState) -> APIRouter:
    """Create an immutable-identity reader for a later shell composition."""
    router = APIRouter()

    @router.get("/environment")
    def get_node_environment() -> dict:
        if state.headless_client is not None:
            try:
                active = read_active_node_environment(state)
            except ActiveNodeEnvironmentProjectionError as error:
                raise HTTPException(
                    status_code=error.status_code,
                    detail={"code": error.code, "message": str(error)},
                ) from error
            payload = active.payload
            manifest = active.manifest
            return {
                "environmentDigest": manifest.environment_digest,
                "catalog": {
                    "apiVersion": manifest.catalog_api_version,
                    "catalogId": manifest.catalog_id,
                    "contentVersion": manifest.catalog_content_version,
                    "catalogDigest": manifest.catalog_digest,
                },
                "executableRegistry": [
                    row.to_dict() for row in manifest.executable_registry
                ],
                "executablePackages": [
                    row.to_dict() for row in manifest.executable_packages
                ],
                "sourceCoverage": [
                    row.to_dict() for row in manifest.source_coverage
                ],
                "availability": payload["availability"],
                "active": {
                    "retainedBundleId": payload["retained_bundle_id"],
                    "graphRevision": payload["graph_revision"],
                },
                "source": payload["source"],
            }
        return _environment_payload(state)

    return router
