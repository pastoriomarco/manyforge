#!/usr/bin/env python3
"""Derive the model-free component wrappers from the shared-host row definitions.

The ROS runtime descriptor is the single authored row source. Each projection
(I/O device, pose source) adds component-level cardinality only; it creates no
runtime unit or process.
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
    "manyforge.value_schema.runtime.pose_source_execution.v1": (
        "pose_source_component_configuration",
        {"pose_sources": 1},
    ),
}


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
    result = normalize_schema({
        "api_version": "manyforge.closed_schema.v1",
        "schema_id": schema_id,
        "schema_revision": "sha256:" + "0" * 64,
        "schema_purpose": "value",
        "root_definition_id": root_id,
        "definitions": [{"definition_id": root_id,
                         "value_schema": {"kind": "object", "fields": selected}},
                        *nested],
    })
    result["schema_revision"] = semantic_revision(
        result, domain="schema-revision", api_version="manyforge.closed_schema.v1",
        revision_field="schema_revision")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace-root", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    source_path = args.workspace_root / SOURCE
    source = load_strict_json_yaml_bytes(source_path.read_bytes(), source=str(source_path))
    for schema_id in WRAPPERS:
        payload = canonical_json_bytes(derive_schema(source, schema_id)) + b"\n"
        target = args.workspace_root / VALUE_DIRECTORY / f"{schema_id}.yaml"
        if args.update:
            target.write_bytes(payload)
        elif not target.is_file() or target.read_bytes() != payload:
            parser.error(f"{schema_id} is stale; run the normal --update")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
