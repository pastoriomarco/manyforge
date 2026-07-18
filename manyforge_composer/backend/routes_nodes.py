"""Effective node-environment discovery and reload routes."""

from __future__ import annotations

from fastapi import APIRouter, HTTPException

from manyforge_behavior.node_environment import NodeEnvironmentBuildError

from .state import ComposerState


def _report_payload(state: ComposerState) -> dict:
    report = state.latest_node_discovery_report
    diagnostics = [
        {
            "path": item.path,
            "nodeId": item.node_id,
            "version": item.version,
            "status": item.status,
            "errorCode": item.error_code,
            "messages": [item.message] if item.message else [],
        }
        for item in report.diagnostics
    ]
    return {
        "status": (
            "ok" if report.activated else "conflict" if report.conflict else "rejected"
        ),
        "attemptedAtNs": report.attempted_at_ns,
        "roots": list(report.roots),
        "strict": report.strict,
        "previousCatalogHash": report.previous_catalog_hash,
        "effectiveCatalogHash": report.effective_catalog_hash,
        "activeEffectiveCatalogHash": state.node_environment.effective_catalog_hash,
        "activeGeneration": state.node_environment.generation,
        "activated": report.activated,
        "conflict": report.conflict,
        "summary": {
            "accepted": report.accepted_count,
            "rejected": report.rejected_count,
            "added": report.added_count,
            "changed": report.changed_count,
            "removed": report.removed_count,
            "unchanged": report.unchanged_count,
        },
        "results": diagnostics,
    }


def create_nodes_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("/discovered")
    def discovered_nodes() -> dict:
        environment = state.node_environment
        return {
            "effectiveCatalogHash": environment.effective_catalog_hash,
            "generation": environment.generation,
            "sources": [
                {
                    "nodeId": source.node_id,
                    "kind": source.kind,
                    "path": source.path,
                    "contentHash": source.content_hash,
                    "version": source.version,
                }
                for source in environment.sources
            ],
        }

    @router.get("/diagnostics")
    def node_diagnostics() -> dict:
        return _report_payload(state)

    @router.get("/{node_id}/source")
    def node_source(node_id: str) -> dict:
        descriptor = state.node_environment.descriptors.get(node_id)
        if descriptor is None:
            raise HTTPException(
                status_code=404,
                detail=f"external node '{node_id}' was not discovered",
            )
        return {
            "nodeId": node_id,
            "contentHash": descriptor.content_hash,
            "descriptor": descriptor.descriptor_text,
            "source": descriptor.source_text,
        }

    @router.post("/reload")
    def reload_nodes() -> dict:
        try:
            state.reload_node_environment()
        except NodeEnvironmentBuildError as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        except RuntimeError as exc:
            status = 409 if (
                "execution_not_idle" in str(exc)
                or "artifacts_changed" in str(exc)
                or "run_admission_in_progress" in str(exc)
                or "node_environment_reload_in_progress" in str(exc)
            ) else 503
            raise HTTPException(status_code=status, detail=str(exc)) from exc
        return _report_payload(state)

    return router
