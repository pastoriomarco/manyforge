from __future__ import annotations

import pytest

from manyforge_assistant_bridge.catalog import (
    AssistantCatalogError,
    resolve_catalog,
)
from manyforge_composer.backend.assistant_catalog_narrowing import (
    resolve_assistant_catalog,
)


def _mode() -> dict:
    return {
        "mode": "composer-assistant",
        "grant_policy": {
            "approval_mode": None,
            "review_policy": "required",
            "session_kind": None,
        },
        "nodes": [{"node_id": "sequence"}],
        "skills": [],
        "tools": [
            {
                "tool_id": "scene_draft_update_object",
                "description": "Update one object",
                "effect": "composer_draft_mutating",
                "input_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "orientation": {
                            "type": "object",
                            "additionalProperties": False,
                            "oneOf": [
                                {"required": ["xyzw"]},
                                {"required": ["rpy_deg"]},
                            ],
                            "properties": {
                                "xyzw": {"type": "array"},
                                "rpy_deg": {"type": "array"},
                            },
                        },
                        "rpy_delta_deg": {"type": "array"},
                    },
                },
                "input_schema_hash": "sha256:" + "0" * 64,
            },
            {
                "tool_id": "status_read",
                "description": "Read status",
                "effect": "read_only",
                "input_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {},
                },
                "input_schema_hash": "sha256:" + "1" * 64,
            },
        ],
    }


def test_request_and_field_policy_can_only_narrow() -> None:
    resolved = resolve_catalog(
        _mode(),
        requested_tools=["scene_draft_update_object"],
        field_policy={"allowedOrientationFields": ["rpy_deg"]},
    )
    assert [row["tool_id"] for row in resolved.tools] == [
        "scene_draft_update_object"
    ]
    schema = resolved.tools[0]["input_schema"]
    orientation = schema["properties"]["orientation"]
    assert set(orientation["properties"]) == {"rpy_deg"}
    assert orientation["oneOf"] == [{"required": ["rpy_deg"]}]
    assert "rpy_delta_deg" not in schema["properties"]

    composer = resolve_assistant_catalog(
        _mode(),
        requested_tools=["scene_draft_update_object"],
        field_policy={"allowedOrientationFields": ["rpy_deg"]},
    )
    assert composer == resolved.to_wire()


@pytest.mark.parametrize(
    ("requested", "policy"),
    [
        (["runtime.stop"], {}),
        (["unknown_tool"], {}),
        (["status_read", "status_read"], {}),
        ([], {"orientationFields": ["rpy_deg"]}),
        ([], {"allowedOrientationFields": ["unknown"]}),
    ],
)
def test_narrowing_rejects_aliases_widening_and_noncanonical_inputs(
    requested, policy
) -> None:
    with pytest.raises(AssistantCatalogError):
        resolve_catalog(_mode(), requested_tools=requested, field_policy=policy)
