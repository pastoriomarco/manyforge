"""The catalog can describe the closed v3 component reference objects."""

import pytest

from manyforge_behavior.catalog_schema import (
    CatalogSchemaError,
    parse_schema_registry,
    validate_value,
)
from manyforge_behavior.interrupt_condition import check_condition, condition_signals


@pytest.mark.parametrize(
    ("semantic_type", "member", "value"),
    [
        ("target_reference", "command_target_id", "2_ARM"),
        ("signal_reference", "signal_name", "CI0"),
        ("frame_reference", "frame_id", "TCP"),
        ("collision_body_reference", "collision_body_id", "arm_collision"),
    ],
)
def test_reference_object_shape_and_partial_draft(semantic_type, member, value):
    registry = parse_schema_registry(
        {
            "reference": {
                "type": "object",
                "semantic_type": semantic_type,
                "properties": {
                    "component_id": {"type": "string"},
                    member: {"type": "string"},
                },
                "required": ["component_id", member],
                "additional_properties": False,
            }
        }
    )
    schema = registry.types["reference"]
    projected = registry.to_json_schema(schema)
    assert projected["additionalProperties"] is False
    assert set(projected["required"]) == {"component_id", member}
    assert not validate_value(schema, {"component_id": "robot", member: value}, registry=registry)
    assert validate_value(schema, {"component_id": "robot"}, registry=registry)
    assert not validate_value(
        schema, {"component_id": "robot"}, registry=registry, allow_partial=True
    )
    assert validate_value(
        schema, {"component_id": "robot", member: value, "extra": "x"}, registry=registry
    )
    assert validate_value(schema, {"component_id": "robot", member: ""}, registry=registry)


def test_reference_semantics_reject_incorrect_catalog_schema():
    with pytest.raises(CatalogSchemaError, match="closed object"):
        parse_schema_registry(
            {"bad": {"type": "string", "semantic_type": "signal_reference"}}
        )


def test_signal_name_preserves_existing_nonportable_text_and_bounds():
    registry = parse_schema_registry(
        {
            "signal": {
                "type": "object",
                "semantic_type": "signal_reference",
                "properties": {
                    "component_id": {"type": "string"},
                    "signal_name": {"type": "string"},
                },
                "required": ["component_id", "signal_name"],
            }
        }
    )
    schema = registry.types["signal"]
    for name in ("TO0", "carried_object_lost.gripper", "é" * 256):
        assert not validate_value(schema, {"component_id": "io", "signal_name": name}, registry=registry)
    for name in ("", "x\x00y", "é" * 257, "\ud800"):
        assert validate_value(schema, {"component_id": "io", "signal_name": name}, registry=registry)


def test_direct_interrupt_condition_uses_signal_reference_without_roles():
    registry = parse_schema_registry({
        "condition": {"type": "component_interrupt_condition"},
        "legacy": {"type": "interrupt_condition"},
    })
    condition = {"compare": {
        "operand": {"signal": {"component_id": "robot_io", "signal_name": "CI0"}},
        "op": "==",
        "value": True,
    }}
    assert not validate_value(registry.types["condition"], condition, registry=registry)
    assert validate_value(registry.types["legacy"], condition, registry=registry)
    check_condition(condition, key_types={}, target_resource_roles={}, direct_references=True)
    assert condition_signals(condition, direct_references=True) == {("robot_io", "CI0")}
    condition["compare"]["operand"]["signal"]["component_id"] = ""
    assert validate_value(registry.types["condition"], condition, registry=registry)


def test_tcp_selector_catalog_condition_enforces_direct_variant_shapes():
    from pathlib import Path
    from manyforge_behavior.catalog import NodeCatalog

    root = Path(__file__).resolve().parents[2]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    schema = catalog.schema_registry.types["tcp_selector"]
    target = {"component_id": "robot", "command_target_id": "arm"}
    frame = {"component_id": "tool", "frame_id": "TCP"}
    valid = [{"kind": "current"}, {"kind": "default_of_target", "target": target},
             {"kind": "entry", "target": target, "frame": frame}]
    invalid = [{"kind": "current", "target": target}, {"kind": "default_of_target"},
               {"kind": "entry", "frame": frame}, {"kind": "default_of_target", "target": target, "frame": frame}]
    assert all(not validate_value(schema, value, registry=catalog.schema_registry) for value in valid)
    assert all(validate_value(schema, value, registry=catalog.schema_registry) for value in invalid)
    projected = catalog.schema_registry.to_json_schema(schema)
    assert any(rule.get("if", {}).get("properties", {}).get("kind", {}).get("enum") ==
               ["default_of_target", "entry"] for rule in projected["allOf"])


@pytest.mark.parametrize("choices", [[], ["entry", "entry"], ["unknown"]])
def test_enum_list_conditions_must_be_nonempty_declared_unique_values(choices):
    with pytest.raises(CatalogSchemaError, match="nonempty set of declared enum values"):
        parse_schema_registry({"choice": {"type": "object", "properties": {
            "kind": {"type": "enum", "values": ["current", "entry"]},
            "target": {"type": "string", "required_when": {"kind": choices}},
        }}})
