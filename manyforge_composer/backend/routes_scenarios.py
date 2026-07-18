"""Artifact-owned scenario discovery routes."""

from __future__ import annotations

from fastapi import APIRouter, HTTPException

from manyforge_behavior.scenarios import ScenarioDiscoveryError, discover_scenarios

from .state import ComposerState


def create_scenarios_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get("")
    def list_scenarios() -> dict:
        try:
            catalog = discover_scenarios(
                state.scenario_roots,
                node_environment=state.node_environment,
            )
        except (OSError, ScenarioDiscoveryError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc
        return {
            "scenarios": [
                {
                    "name": scenario.name,
                    "lane": scenario.lane,
                    "assistant": scenario.assistant,
                    "programPath": str(scenario.program_path),
                    "deploymentPath": str(scenario.deployment_path),
                    "description": scenario.description,
                    "aliases": list(scenario.aliases),
                }
                for scenario in catalog.scenarios
            ]
        }

    return router
