"""Catalog routes - /api/catalog/*"""
from __future__ import annotations

from fastapi import APIRouter, HTTPException
from .models import CatalogResponse, CatalogEntryResponse, CatalogParameterResponse
from .state import ComposerState


def create_catalog_router(state: ComposerState) -> APIRouter:
    """Create the catalog router with the given state."""
    router = APIRouter()
    
    @router.get("/", response_model=CatalogResponse, response_model_by_alias=True)
    def get_catalog() -> dict:
        """Get all catalog entries."""
        entries = []
        for entry in state.catalog.entries:
            params = []
            for param in entry.parameters:
                params.append(CatalogParameterResponse(
                    name=param.name,
                    type=param.type,
                    required=param.required,
                    default=param.default,
                    enumValues=param.enum_values,
                    constraints=param.constraints,
                    parameterizable=param.parameterizable,
                    description=param.description,
                ).model_dump(by_alias=True))
            entries.append(CatalogEntryResponse(
                id=entry.id,
                name=entry.name,
                category=entry.category,
                description=entry.description,
                kind=entry.kind,
                childrenMin=entry.children_min,
                childrenMax=entry.children_max,
                parameters=params,
                requiredCapabilities=entry.required_capabilities,
            ).model_dump(by_alias=True))
        return {
            "version": state.catalog.version,
            "entries": entries,
        }
    
    @router.get("/{entry_id}", response_model=CatalogEntryResponse, response_model_by_alias=True)
    def get_catalog_entry(entry_id: str) -> dict:
        """Get a single catalog entry by ID."""
        try:
            entry = state.catalog.get(entry_id)
        except KeyError:
            raise HTTPException(status_code=404, detail=f"Entry '{entry_id}' not found")
        
        params = []
        for param in entry.parameters:
            params.append(CatalogParameterResponse(
                name=param.name,
                type=param.type,
                required=param.required,
                default=param.default,
                enumValues=param.enum_values,
                constraints=param.constraints,
                parameterizable=param.parameterizable,
                description=param.description,
            ).model_dump(by_alias=True))
        
        return CatalogEntryResponse(
            id=entry.id,
            name=entry.name,
            category=entry.category,
            description=entry.description,
            kind=entry.kind,
            childrenMin=entry.children_min,
            childrenMax=entry.children_max,
            parameters=params,
            requiredCapabilities=entry.required_capabilities,
        ).model_dump(by_alias=True)
    
    return router
