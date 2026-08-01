"""Focused tests for owner-neutral authoring-schema projection."""

from __future__ import annotations

from manyforge_composer.backend.schema_projection import (
    projected_property_or_fallback,
    property_schema,
    strip_schema_descriptions,
    with_description,
    with_replaced_description,
)


def test_description_stripping_preserves_fields_named_description() -> None:
    schema = {
        "description": "root prose",
        "properties": {
            "description": {
                "type": "string",
                "description": "field prose",
            },
            "nested": {
                "type": "object",
                "description": "nested prose",
                "properties": {
                    "value": {"type": "number", "description": "value prose"},
                },
            },
        },
    }

    projected = strip_schema_descriptions(schema)

    assert projected == {
        "properties": {
            "description": {"type": "string"},
            "nested": {
                "type": "object",
                "properties": {"value": {"type": "number"}},
            },
        },
    }
    assert schema["description"] == "root prose"


def test_description_and_property_projection_never_mutate_inputs() -> None:
    schema = {
        "description": "original",
        "properties": {"value": {"type": "number"}},
    }
    fallback = {"type": "string"}

    assert with_description(schema, "ignored") is schema
    replacement = with_replaced_description(schema, "replacement")
    selected = property_schema(schema, "value")
    fallback_selected = projected_property_or_fallback(
        schema,
        "missing",
        fallback,
    )

    assert replacement["description"] == "replacement"
    assert selected == {"type": "number"}
    assert fallback_selected == fallback
    assert replacement is not schema
    assert selected is not schema["properties"]["value"]
    assert fallback_selected is not fallback
