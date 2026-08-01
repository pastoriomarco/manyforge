"""Wire-neutral JSON-Schema projection helpers for authoring clients.

The old assistant tool schemas are not retained.  These helpers operate only
on caller-supplied schema values and do not mint schema identity, tool
availability, runtime capability, or validation authority.
"""

from __future__ import annotations

import copy
from typing import Any


def strip_schema_descriptions(
    value: Any,
    *,
    in_properties_map: bool = False,
) -> Any:
    """Deep-copy a schema value while removing prose annotations.

    A field literally named ``description`` inside ``properties`` or a
    definition map remains structural; only schema-object annotation keys are
    removed.
    """

    if isinstance(value, dict):
        if in_properties_map:
            return {
                property_name: strip_schema_descriptions(property_schema)
                for property_name, property_schema in value.items()
            }
        result: dict[str, Any] = {}
        for key, item in value.items():
            if key == "description":
                continue
            result[key] = strip_schema_descriptions(
                item,
                in_properties_map=key in {"properties", "$defs", "definitions"},
            )
        return result
    if isinstance(value, list):
        return [strip_schema_descriptions(item) for item in value]
    return value


def with_description(
    schema: dict[str, Any],
    description: str | None,
) -> dict[str, Any]:
    """Add a description to a copy only when one is not already present."""

    if description and "description" not in schema:
        projected = copy.deepcopy(schema)
        projected["description"] = description
        return projected
    return schema


def with_replaced_description(
    schema: dict[str, Any],
    description: str,
) -> dict[str, Any]:
    """Return a deep copy with one replacement description."""

    projected = copy.deepcopy(schema)
    projected["description"] = description
    return projected


def property_schema(
    schema: dict[str, Any],
    property_name: str,
) -> dict[str, Any] | None:
    """Return a detached property schema when it is a mapping."""

    properties = schema.get("properties")
    if not isinstance(properties, dict):
        return None
    projected = properties.get(property_name)
    if not isinstance(projected, dict):
        return None
    return copy.deepcopy(projected)


def projected_property_or_fallback(
    projected_schema: dict[str, Any],
    property_name: str,
    fallback_schema: dict[str, Any],
) -> dict[str, Any]:
    """Return a detached projected property or detached fallback."""

    return property_schema(projected_schema, property_name) or copy.deepcopy(
        fallback_schema
    )


__all__ = [
    "projected_property_or_fallback",
    "property_schema",
    "strip_schema_descriptions",
    "with_description",
    "with_replaced_description",
]
