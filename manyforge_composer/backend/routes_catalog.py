"""Read-only built-in catalog routes."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from fastapi import APIRouter, HTTPException

from .models import CatalogEntryResponse, CatalogResponse
from .node_environment_client import (
    ActiveNodeEnvironmentProjectionError,
    read_active_node_environment,
)
from .state import ComposerState


def _json_response_value(value: Any) -> Any:
    """Thaw immutable catalog values at the HTTP serialization boundary."""

    if isinstance(value, Mapping):
        return {key: _json_response_value(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_json_response_value(nested) for nested in value]
    return value


def _parameter_response(param: Any, *, catalog: Any) -> dict[str, Any]:
    response = {
        "id": param.id,
        "required": param.required,
        "schema": catalog.schema_registry.to_json_schema(param.schema),
        "schemaHash": catalog.schema_registry.schema_hash(param.schema),
        "hasDefault": param.has_default,
    }
    if param.has_default:
        response["default"] = _json_response_value(param.default)
    return response


def _capability_response(requirement: Any) -> dict[str, Any]:
    response = {
        "capabilityId": requirement.capability_id,
        "scope": requirement.scope,
        "effectful": requirement.effectful,
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
                _blackboard_response(binding) for binding in entry.blackboard_reads
            ],
            "writes": [
                _blackboard_response(binding) for binding in entry.blackboard_writes
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

    def active_catalog() -> tuple[Any, str]:
        if state.headless_client is None:
            return (
                state.node_environment.catalog,
                state.node_environment.environment_digest,
            )
        try:
            projection = read_active_node_environment(state)
        except ActiveNodeEnvironmentProjectionError as error:
            raise HTTPException(
                status_code=error.status_code,
                detail={"code": error.code, "message": str(error)},
            ) from error
        return projection.catalog, projection.manifest.environment_digest

    @router.get(
        "",
        response_model=CatalogResponse,
        response_model_by_alias=True,
        response_model_exclude_unset=True,
    )
    def get_catalog() -> dict[str, Any]:
        catalog, environment_digest = active_catalog()
        return {
            "apiVersion": catalog.api_version,
            "contentVersion": catalog.content_version,
            "catalogId": catalog.catalog_id,
            "entries": [
                _entry_response(entry, catalog=catalog) for entry in catalog.entries
            ],
            "catalogDigest": catalog.catalog_digest,
            "environmentDigest": environment_digest,
        }

    @router.get(
        "/{entry_id}",
        response_model=CatalogEntryResponse,
        response_model_by_alias=True,
        response_model_exclude_unset=True,
    )
    def get_catalog_entry(entry_id: str) -> dict[str, Any]:
        catalog, _environment_digest = active_catalog()
        try:
            entry = catalog.get(entry_id)
        except KeyError as exc:
            raise HTTPException(
                status_code=404,
                detail=f"Entry '{entry_id}' not found",
            ) from exc
        return _entry_response(entry, catalog=catalog)

    return router
