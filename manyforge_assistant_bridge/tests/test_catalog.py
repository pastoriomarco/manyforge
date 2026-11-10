from __future__ import annotations

import pytest

from manyforge_assistant_bridge.catalog import (
    AssistantCatalogError,
    _validate_model_request_contract,
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


_CONTRACT = {
    "default_timeout_s": 30,
    "max_output_tokens": 512,
    "supported_image_transports": [],
    "supports_image_input": False,
    "supports_json_object": True,
    "supports_response_tools": True,
    "tool_call_family": "openai_chat_tools",
}


def test_model_sampling_is_optional_and_kept() -> None:
    assert "sampling" not in _validate_model_request_contract(dict(_CONTRACT))
    sampling = {"temperature": 0, "top_k": 20, "top_p": 1}
    assert _validate_model_request_contract(
        {**_CONTRACT, "sampling": sampling})["sampling"] == sampling


@pytest.mark.parametrize(
    "sampling",
    [{}, {"min_p": 0.1}, {"temperature": 2.5}, {"temperature": -0.1},
     {"temperature": True}, {"top_p": 0}, {"top_p": 1.5}, {"top_k": 0},
     {"top_k": 1.5}, [0.5]],
)
def test_model_sampling_out_of_range_is_refused(sampling) -> None:
    with pytest.raises(AssistantCatalogError) as error:
        _validate_model_request_contract({**_CONTRACT, "sampling": sampling})
    assert error.value.code == "assistant_projection_invalid"
