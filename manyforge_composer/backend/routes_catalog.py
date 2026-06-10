"""Catalog routes - /api/catalog/*"""
from __future__ import annotations

from fastapi import APIRouter, HTTPException
from .models import (
    CatalogResponse,
    CatalogEntryResponse,
    CatalogParameterResponse,
    CatalogEnumOptionResponse,
    CatalogVisibleWhenResponse,
)
from .state import ComposerState


def _parameter_response(param, *, catalog=None) -> dict:
    """Convert a catalog parameter to an API response dictionary."""
    visible_when = None
    if param.visible_when is not None:
        visible_when = CatalogVisibleWhenResponse(
            param=param.visible_when.param,
            equals=param.visible_when.equals,
        )
    schema = None
    schema_hash = None
    if catalog is not None and getattr(param, "schema", None) is not None:
        schema = catalog.schema_registry.to_json_schema(param.schema)
        schema_hash = catalog.schema_registry.schema_hash(param.schema)
    return CatalogParameterResponse(
        name=param.name,
        type=param.type,
        required=param.required,
        default=param.default,
        enumValues=param.enum_values,
        enumOptions=[
            CatalogEnumOptionResponse(
                value=option.value,
                label=option.label,
                description=option.description,
                profilesMin=option.profiles_min,
                requiredCapabilities=option.required_capabilities,
            )
            for option in param.enum_options
        ],
        constraints=param.constraints,
        parameterizable=param.parameterizable,
        profilesMin=param.profiles_min,
        visibleWhen=visible_when,
        uiEditor=param.ui_editor,
        blackboardRole=param.blackboard_role,
        blackboardContract=param.blackboard_contract,
        blackboardValueType=param.blackboard_value_type,
        schema=schema,
        schemaHash=schema_hash,
        description=param.description,
    ).model_dump(by_alias=True)


def create_catalog_router(state: ComposerState) -> APIRouter:
    """Create the catalog router with the given state."""
    router = APIRouter()
    
    @router.get("", response_model=CatalogResponse, response_model_by_alias=True)
    def get_catalog() -> dict:
        """Get all catalog entries."""
        entries = []
        for entry in state.catalog.entries:
            params = [
                _parameter_response(param, catalog=state.catalog)
                for param in entry.parameters
            ]
            entries.append(CatalogEntryResponse(
                id=entry.id,
                name=entry.name,
                category=entry.category,
                description=entry.description,
                stability=entry.stability,
                kind=entry.kind,
                childrenMin=entry.children_min,
                childrenMax=entry.children_max,
                parameters=params,
                bbReaderParams=entry.bb_reader_params,
                bbWriterParams=entry.bb_writer_params,
                requiredCapabilities=entry.required_capabilities,
                profilesMin=entry.profiles_min,
                interruptible=entry.interruptible,
                defaultInterruptScope=entry.default_interrupt_scope,
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
        
        params = [
            _parameter_response(param, catalog=state.catalog)
            for param in entry.parameters
        ]
        
        return CatalogEntryResponse(
            id=entry.id,
            name=entry.name,
            category=entry.category,
            description=entry.description,
            stability=entry.stability,
            kind=entry.kind,
            childrenMin=entry.children_min,
            childrenMax=entry.children_max,
            parameters=params,
            bbReaderParams=entry.bb_reader_params,
            bbWriterParams=entry.bb_writer_params,
            requiredCapabilities=entry.required_capabilities,
            profilesMin=entry.profiles_min,
            interruptible=entry.interruptible,
            defaultInterruptScope=entry.default_interrupt_scope,
        ).model_dump(by_alias=True)
    
    return router
