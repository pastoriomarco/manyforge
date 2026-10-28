#!/usr/bin/env python3
"""Generate the model-free component configuration schemas.

The I/O device wrapper is derived from the shared-host row definitions: the
ROS runtime descriptor is the single authored row source and the projection
adds component-level cardinality only. The pose-source component schema is
authored here; the Deployment compiler joins it with the object links into
the unchanged `pose_sources` unit row.
"""

from __future__ import annotations

import argparse
import copy
from pathlib import Path

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    load_strict_json_yaml_bytes,
)
from generate_maintained_deployment_occurrences import semantic_revision


SOURCE = "manyforge_deployment/resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml"
VALUE_DIRECTORY = "manyforge_deployment/resources/closed_schema/value"
# schema id -> (root definition id, {selected field: max_items or None}).
WRAPPERS = {
    "manyforge.value_schema.runtime.io_provider_execution.v1": (
        "io_provider_component_configuration",
        {"io_signals": None, "io_provider_devices": 1},
    ),
}
POSE_SOURCE_SCHEMA_ID = "manyforge.value_schema.runtime.pose_source.v2"


def _scalar(scalar_type: str, **constraints: object) -> dict:
    return {"kind": "scalar", "scalar_type": scalar_type,
            "constraints": [{"kind": kind, "value": value}
                            for kind, value in constraints.items()]}


def _text(min_length: int, max_length: int) -> dict:
    if min_length == 0:
        return _scalar("string", max_length=max_length)
    return _scalar("string", min_length=min_length, max_length=max_length)


def _field(field_id: str, value_schema: dict, *, optional: bool = False) -> dict:
    return {"field_id": field_id,
            "presence": "optional" if optional else "required",
            "default": {"kind": "absent"},
            "value_schema": value_schema}


def _object(*fields: dict) -> dict:
    return {"kind": "object", "fields": list(fields)}


def _numbers(count: int) -> dict:
    return {"kind": "array", "min_items": count, "max_items": count,
            "collection": {"kind": "ordered"}, "item_schema": _scalar("number")}


def _local_offset() -> list[dict]:
    """Optional local offset; absent means zero / identity."""
    return [_field("local_position_xyz", _numbers(3), optional=True),
            _field("local_orientation_xyzw", _numbers(4), optional=True)]


def pose_source_schema() -> dict:
    """Authored `pose_source_v0` component: source-level facts only."""
    root = _object(
        _field("stream_budget_ns", _scalar("integer", minimum=1, maximum=60_000_000_000)),
        _field("simulation", _object(
            _field("poll_period_ms", _scalar("integer", minimum=20, maximum=60000)),
            _field("source_coordinate_frame_id", _text(0, 1024)),
            _field("target_frame_id", _text(1, 1024)),
            _field("relative_references", {
                "kind": "array", "min_items": 1, "max_items": 128,
                "collection": {
                    "kind": "keyed_record_set",
                    "key_paths": [["controllable_component_id"],
                                  ["command_target_id"]],
                },
                "item_schema": _object(
                    _field("controllable_component_id",
                           _scalar("string", string_format="portable_id")),
                    _field("command_target_id", _text(1, 256)),
                    _field("gripper_link", _text(1, 256), optional=True),
                    _field("entity", _text(1, 1024)),
                    _field("link", _text(1, 256)),
                    _field("source_header_frame_id", _text(1, 1024)),
                ),
            }, optional=True),
            # Compatibility input only. Activation folds this legacy
            # source-wide row into the per-target map before compilation.
            _field("relative_reference", _object(
                _field("entity", _text(1, 1024)),
                _field("link", _text(1, 256)),
                _field("source_header_frame_id", _text(1, 1024)),
            ), optional=True),
        ), optional=True),
        _field("detection3d", _object(
            _field("topic", _text(2, 256)),
            _field("target_frame_id", _text(1, 1024)),
            _field("fallback_frame_id", _text(0, 1024)),
        ), optional=True),
        _field("robot_overlay", _object(
            _field("robot_component_id",
                   _scalar("string", string_format="portable_id")),
            _field("joint_state", _object(
                _field("topic", _text(2, 256)),
                _field("required_joint_names", {
                    "kind": "array", "min_items": 1, "max_items": 128,
                    "collection": {"kind": "ordered"},
                    "item_schema": _text(1, 1024)}),
            ), optional=True),
            _field("tcp_entity", _object(
                _field("entity", _text(1, 1024)),
                _field("source_header_frame_id", _text(0, 1024)),
                *_local_offset(),
            ), optional=True),
        ), optional=True),
    )
    return _finish(POSE_SOURCE_SCHEMA_ID, "pose_source_component_configuration",
                   [{"definition_id": "pose_source_component_configuration",
                     "value_schema": root}])


def _finish(schema_id: str, root_id: str, definitions: list[dict]) -> dict:
    result = normalize_schema({
        "api_version": "manyforge.closed_schema.v1",
        "schema_id": schema_id,
        "schema_revision": "sha256:" + "0" * 64,
        "schema_purpose": "value",
        "root_definition_id": root_id,
        "definitions": definitions,
    })
    result["schema_revision"] = semantic_revision(
        result, domain="schema-revision", api_version="manyforge.closed_schema.v1",
        revision_field="schema_revision")
    return result


def normalize_schema(document: dict) -> dict:
    """Apply the existing closed dialect's set ordering before identity."""
    result = copy.deepcopy(document)

    def visit(value: dict) -> None:
        kind = value["kind"]
        if kind == "scalar":
            value["constraints"].sort(key=lambda row: row["kind"])
            for constraint in value["constraints"]:
                if constraint["kind"] == "enum":
                    constraint["values"].sort(key=canonical_json_bytes)
        elif kind == "object":
            value["fields"].sort(key=lambda row: row["field_id"])
            for field in value["fields"]:
                visit(field["value_schema"])
        elif kind == "array":
            visit(value["item_schema"])
        elif kind == "tagged_union":
            value["alternatives"].sort(key=lambda row: row["tag_value"])
            for alternative in value["alternatives"]:
                visit(alternative["value_schema"])

    result["definitions"].sort(key=lambda row: row["definition_id"])
    for definition in result["definitions"]:
        visit(definition["value_schema"])
    return result


def local_references(value: object) -> set[str]:
    if isinstance(value, dict):
        found = {value["definition_id"]} if value.get("kind") == "local_ref" else set()
        for item in value.values():
            found |= local_references(item)
        return found
    if isinstance(value, list):
        return set().union(*(local_references(item) for item in value))
    return set()


def derive_schema(source: dict, schema_id: str) -> dict:
    root_id, selection = WRAPPERS[schema_id]
    definitions = {row["definition_id"]: row for row in source["definitions"]}
    root = definitions[source["root_definition_id"]]["value_schema"]
    fields = {row["field_id"]: row for row in root["fields"]}
    selected = []
    for name, max_items in selection.items():
        field = copy.deepcopy(fields[name])
        field["presence"] = "required"
        field["default"] = {"kind": "absent"}
        field["value_schema"]["min_items"] = 1
        if max_items is not None:
            field["value_schema"]["max_items"] = max_items
        selected.append(field)
    # Local definitions are copied once from the authoritative row source.
    # The wrapper's semantic component check rejects foreign components.
    pending = local_references(selected)
    used: set[str] = set()
    while pending:
        key = pending.pop()
        if key not in used:
            used.add(key)
            pending |= local_references(definitions[key])
    nested = [copy.deepcopy(row) for key, row in definitions.items()
              if key != source["root_definition_id"] and key in used]
    return _finish(schema_id, root_id,
                   [{"definition_id": root_id,
                     "value_schema": {"kind": "object", "fields": selected}},
                    *nested])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source_path = args.workspace_root / SOURCE
    source = load_strict_json_yaml_bytes(source_path.read_bytes(), source=str(source_path))
    schemas = [derive_schema(source, schema_id) for schema_id in WRAPPERS]
    schemas.append(pose_source_schema())
    for schema in schemas:
        schema_id = schema["schema_id"]
        payload = canonical_json_bytes(schema) + b"\n"
        target = args.workspace_root / VALUE_DIRECTORY / f"{schema_id}.yaml"
        if args.update:
            target.write_bytes(payload)
        elif not target.is_file() or target.read_bytes() != payload:
            parser.error(f"{schema_id} is stale; run the normal --update")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
