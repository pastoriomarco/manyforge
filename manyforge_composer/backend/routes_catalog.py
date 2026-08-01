"""Read-only built-in catalog routes."""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException

from .models import CatalogEntryResponse, CatalogResponse
from .state import ComposerState


def _parameter_response(param: Any, *, catalog: Any) -> dict[str, Any]:
    response = {
        "id": param.id,
        "required": param.required,
        "schema": catalog.schema_registry.to_json_schema(param.schema),
        "schemaHash": catalog.schema_registry.schema_hash(param.schema),
        "hasDefault": param.has_default,
    }
    if param.has_default:
        response["default"] = param.default
    return response


def _capability_response(requirement: Any) -> dict[str, Any]:
    response = {
        "capabilityId": requirement.capability_id,
        "scope": requirement.scope,
    }
    for key, value in (
        ("targetParameter", requirement.target_parameter),
        ("componentParameter", requirement.component_parameter),
        ("whenParameter", requirement.when_parameter),
    ):
        if value is not None:
            response[key] = value
    if requirement.when_parameter is not None:
        response["whenEquals"] = requirement.when_equals
    return response


def _blackboard_response(binding: Any) -> dict[str, Any]:
    response = {
        "keyParameter": binding.key_parameter,
        "valueType": binding.value_type,
    }
    if binding.missing_key is not None:
        response["missingKey"] = binding.missing_key
    return response


def _entry_response(entry: Any, *, catalog: Any) -> dict[str, Any]:
    return {
        "id": entry.id,
        "name": entry.name,
        "category": entry.category,
        "description": entry.description,
        "stability": entry.stability,
        "kind": entry.kind,
        "childrenMin": entry.children_min,
        "childrenMax": entry.children_max,
        "parameters": [
            _parameter_response(parameter, catalog=catalog)
            for parameter in entry.parameters
        ],
        "requiredCapabilities": [
            _capability_response(requirement)
            for requirement in entry.required_capabilities
        ],
        "execution": {
            "blocking": entry.execution.blocking,
            "cancellable": entry.execution.cancellable,
            "ownerApi": entry.execution.owner_api,
        },
        "runners": dict(entry.runner_implementation_ids),
        "tags": list(entry.tags),
        "minProgramProfile": entry.min_program_profile,
        "blackboard": {
            "reads": [
                _blackboard_response(binding)
                for binding in entry.blackboard_reads
            ],
            "writes": [
                _blackboard_response(binding)
                for binding in entry.blackboard_writes
            ],
        },
        "ownerContext": (
            {
                "kind": entry.owner_context.kind,
                "required": entry.owner_context.required,
            }
            if entry.owner_context is not None
            else None
        ),
        "outcomes": dict(entry.outcomes),
        "eventFamily": entry.event_family,
    }


def create_catalog_router(state: ComposerState) -> APIRouter:
    router = APIRouter()

    @router.get(
        "",
        response_model=CatalogResponse,
        response_model_by_alias=True,
        response_model_exclude_unset=True,
    )
    def get_catalog() -> dict[str, Any]:
        environment = state.node_environment
        catalog = environment.catalog
        return {
            "apiVersion": catalog.api_version,
            "contentVersion": catalog.content_version,
            "catalogId": catalog.catalog_id,
            "entries": [
                _entry_response(entry, catalog=catalog)
                for entry in catalog.entries
            ],
            "catalogDigest": catalog.catalog_digest,
            "environmentDigest": environment.environment_digest,
        }

    @router.get(
        "/{entry_id}",
        response_model=CatalogEntryResponse,
        response_model_by_alias=True,
        response_model_exclude_unset=True,
    )
    def get_catalog_entry(entry_id: str) -> dict[str, Any]:
        catalog = state.node_environment.catalog
        try:
            entry = catalog.get(entry_id)
        except KeyError as exc:
            raise HTTPException(
                status_code=404,
                detail=f"Entry '{entry_id}' not found",
            ) from exc
        return _entry_response(entry, catalog=catalog)

    return router
