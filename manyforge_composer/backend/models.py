"""Current thin-client DTOs for the structural Composer shell.

The former monolith mixed Program, scene, runtime, lifecycle, Stop, viewer, and
assistant contracts owned by different migration gates. Those v1 models do not
return as compatibility DTOs. Gate-specific clients add focused models here (or
in their bounded module) only after the corresponding headless contract is
frozen. Current models describe immutable catalog/environment reads and typed
unavailability only.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field


_STRICT_ALIAS_CONFIG = ConfigDict(
    populate_by_name=True,
    extra="forbid",
    allow_inf_nan=False,
)


class UnavailableDetail(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    code: str = Field(min_length=1, max_length=128)
    message: str = Field(min_length=1, max_length=2048)
    restoration_gates: list[str] = Field(
        default_factory=list,
        alias="restorationGates",
        max_length=16,
    )


class UnavailableResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    detail: UnavailableDetail


class CatalogParameterResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    id: str
    required: bool
    schema_: dict[str, Any] = Field(alias="schema")
    schema_hash: str = Field(alias="schemaHash")
    has_default: bool = Field(alias="hasDefault")
    default: Any | None = None


class CatalogCapabilityResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    capability_id: str = Field(alias="capabilityId")
    scope: str
    target_parameter: str | None = Field(default=None, alias="targetParameter")
    component_parameter: str | None = Field(
        default=None,
        alias="componentParameter",
    )
    when_parameter: str | None = Field(default=None, alias="whenParameter")
    when_equals: Any | None = Field(default=None, alias="whenEquals")


class CatalogExecutionResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    blocking: bool
    cancellable: bool
    owner_api: str = Field(alias="ownerApi")


class CatalogEntryResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    id: str
    name: str
    category: str
    description: str
    stability: str
    kind: Literal["composite", "decorator", "leaf"]
    children_min: int = Field(alias="childrenMin", ge=0)
    children_max: int | Literal["unbounded"] = Field(alias="childrenMax")
    parameters: list[CatalogParameterResponse] = Field(default_factory=list)
    required_capabilities: list[CatalogCapabilityResponse] = Field(
        default_factory=list,
        alias="requiredCapabilities",
    )
    execution: CatalogExecutionResponse
    runners: dict[str, str] = Field(default_factory=dict)
    tags: list[str] = Field(default_factory=list)
    min_program_profile: str = Field(alias="minProgramProfile")
    blackboard: dict[str, Any]
    owner_context: dict[str, Any] | None = Field(
        default=None,
        alias="ownerContext",
    )
    outcomes: dict[str, str] = Field(default_factory=dict)
    event_family: str = Field(alias="eventFamily")


class CatalogResponse(BaseModel):
    model_config = _STRICT_ALIAS_CONFIG

    api_version: str = Field(alias="apiVersion")
    content_version: str = Field(alias="contentVersion")
    catalog_id: str = Field(alias="catalogId")
    entries: list[CatalogEntryResponse]
    catalog_digest: str = Field(alias="catalogDigest")
    environment_digest: str = Field(alias="environmentDigest")
