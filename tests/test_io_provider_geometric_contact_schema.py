"""The I/O runtime schemas accept a geometric-contact input's selection form (OD-356).

A geometric-contact input names either `target_scene_resource_id` or two
selections `a` and `b` (collision-rule selection kinds) with an optional
`trip_depth_m` and an optional `release_depth_m` below it (OD-360); the generated I/O provider schema inherits the shape from the
ros2_control runtime schema.
"""

from __future__ import annotations

import json
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
VALUE = ROOT / "manyforge_deployment/resources/closed_schema/value"


def _geometric_contact_fields(document: dict) -> dict[str, dict]:
    found: list[dict] = []

    def visit(value: object) -> None:
        if isinstance(value, dict):
            if value.get("tag_value") == "geometric_contact":
                found.append(value["value_schema"])
            for item in value.values():
                visit(item)
        elif isinstance(value, list):
            for item in value:
                visit(item)

    visit(document)
    assert len(found) == 1
    return {row["field_id"]: row for row in found[0]["fields"]}


def test_runtime_and_io_provider_schemas_take_selections_and_trip_and_release_depths() -> None:
    for path, load in (
        (VALUE / "manyforge.value_schema.runtime.ros2_control_execution.v1.yaml", yaml.safe_load),
        (VALUE / "manyforge.value_schema.runtime.io_provider_execution.v1.yaml", json.loads),
    ):
        document = load(path.read_text(encoding="utf-8"))
        fields = _geometric_contact_fields(document)
        assert fields["target_scene_resource_id"]["presence"] == "optional", path.name
        for side in ("a", "b"):
            schema = fields[side]["value_schema"]
            assert fields[side]["presence"] == "optional"
            assert schema["item_schema"] == {"kind": "local_ref",
                                             "definition_id": "geometric_contact_selection"}
        for depth in ("trip_depth_m", "release_depth_m"):
            assert fields[depth]["presence"] == "optional", path.name
            assert fields[depth]["value_schema"]["scalar_type"] == "number", path.name
            assert fields[depth]["value_schema"]["constraints"] == [
                {"kind": "exclusive_minimum", "value": 0}], path.name
        definitions = {row["definition_id"]: row for row in document["definitions"]}
        kinds = {row["tag_value"] for row in
                 definitions["geometric_contact_selection"]["value_schema"]["alternatives"]}
        # The kinds the input parser accepts; a Scene object is named by its id.
        assert kinds == {"component", "link", "attached_object", "scene_objects",
                         "scene_object", "collision_body"}, path.name
