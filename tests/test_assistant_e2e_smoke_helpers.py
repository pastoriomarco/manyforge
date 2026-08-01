"""Unit coverage for assistant E2E smoke semantic validators."""

from __future__ import annotations

import copy
import importlib.util
import sys
from pathlib import Path
from types import ModuleType

import pytest


def _load_smoke_module() -> ModuleType:
    script_path = Path(__file__).resolve().parents[1] / "scripts" / "smoke-assistant-e2e.py"
    spec = importlib.util.spec_from_file_location("manyforge_assistant_e2e_smoke", script_path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


SMOKE = _load_smoke_module()


def test_golden_scene_resource_set_validator_accepts_exact_match() -> None:
    expected = SMOKE._golden_scene_resources("unit")

    assert (
        SMOKE._scene_resource_set_match_errors(
            copy.deepcopy(expected),
            expected,
            label="arguments.sceneResources",
            require_exact=True,
        )
        == []
    )


def test_golden_scene_resource_set_validator_rejects_extra_and_missing_objects() -> None:
    expected = SMOKE._golden_scene_resources("unit")
    extra = copy.deepcopy(expected)
    extra.append(
        {
            "objectId": "unexpected",
            "shape": {"type": "sphere", "sphere_radius_m": 0.01},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.0, 0.0, 0.01],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        }
    )

    extra_errors = SMOKE._scene_resource_set_match_errors(
        extra,
        expected,
        label="arguments.sceneResources",
        require_exact=True,
    )
    missing_errors = SMOKE._scene_resource_set_match_errors(
        extra[:1],
        expected,
        label="arguments.sceneResources",
        require_exact=True,
    )

    assert any("unexpected values" in error for error in extra_errors)
    assert any("missing expected values" in error for error in missing_errors)


def test_golden_scene_resource_validator_checks_orientation() -> None:
    expected = SMOKE._golden_scene_resources("unit")
    actual = copy.deepcopy(expected[0])
    actual["pose"]["orientation_rpy_deg"] = [0.0, 0.0, 90.0]

    errors = SMOKE._scene_resource_match_errors(
        actual,
        expected[0],
        label="arguments.sceneResources[golden_box_unit]",
    )

    assert any("orientation_rpy_deg" in error for error in errors)


def test_tool_call_scene_resources_accepts_camel_and_snake_case_arguments() -> None:
    resources = SMOKE._golden_scene_resources("unit")

    assert SMOKE._tool_call_scene_resources({"arguments": {"sceneResources": resources}}) == resources
    assert SMOKE._tool_call_scene_resources({"arguments": {"scene_resources": resources}}) == resources


def test_golden_tree_insert_validator_checks_parent_index_and_node() -> None:
    expected = {
        "parentName": "pick_and_place",
        "index": 0,
        "node": {
            "id": "remove_collision_object",
            "name": "golden_tree_remove_unit",
            "params": {"object_id": "golden_tree_object_unit"},
        },
    }
    actual = copy.deepcopy(expected)

    assert SMOKE._tree_insert_argument_errors(actual, expected, label="arguments") == []

    actual["node"]["params"]["object_id"] = "wrong_object"
    errors = SMOKE._tree_insert_argument_errors(actual, expected, label="arguments")

    assert any(".node.params" in error for error in errors)


def test_golden_parameter_validator_checks_result_metadata() -> None:
    expected = {
        "name": "golden_param_unit",
        "type": "string",
        "default": "golden_object",
        "description": "Assistant golden E2E parameter.",
    }
    result = {
        "draftMutation": "program_parameters_upsert",
        "names": ["golden_param_unit"],
        "parameters": [copy.deepcopy(expected)],
    }

    assert SMOKE._program_parameter_result_errors(result, expected, label="result") == []

    result["parameters"][0]["default"] = "wrong"
    errors = SMOKE._program_parameter_result_errors(result, expected, label="result")

    assert any(".default" in error for error in errors)


def test_golden_blackboard_validator_checks_namespace_and_key() -> None:
    namespace = {
        "id": "golden_ns_unit",
        "contract": "selection_context_v1",
        "prefix": "/golden/unit",
        "description": "Assistant golden E2E namespace.",
        "fields": [{"name": "object_id", "type": "string"}],
    }
    key = {
        "id": "golden_key_unit",
        "type": "bool",
        "key": "/golden/unit/ready",
        "description": "Assistant golden E2E ready flag.",
    }
    result = {
        "blackboard": {
            "namespaces": [
                {
                    **copy.deepcopy(namespace),
                    "fields": [{"name": "object_id", "type": "string", "description": ""}],
                }
            ],
            "keys": [copy.deepcopy(key)],
        }
    }

    assert SMOKE._blackboard_result_errors(result, namespace, key, label="result") == []

    result["blackboard"]["keys"][0]["type"] = "string"
    errors = SMOKE._blackboard_result_errors(result, namespace, key, label="result")

    assert any(".type" in error for error in errors)


def test_profile_selection_rejects_duplicates_and_unbounded_input() -> None:
    with pytest.raises(SMOKE.SmokeFailure, match="duplicates"):
        SMOKE._selected_profiles(["bridge-default", "bridge-default"])
    with pytest.raises(SMOKE.SmokeFailure, match="64-item bound"):
        SMOKE._selected_profiles(["bridge-default"] * 65)
    with pytest.raises(SMOKE.SmokeFailure, match="256-character bound"):
        SMOKE._selected_profiles(["x" * 257])


def test_parameter_and_blackboard_validators_reject_duplicate_ids() -> None:
    expected_parameter = {
        "name": "golden_param_unit",
        "type": "string",
        "default": "golden_object",
        "description": "Assistant golden E2E parameter.",
    }
    parameter_result = {
        "draftMutation": "program_parameters_upsert",
        "names": ["golden_param_unit"],
        "parameters": [
            copy.deepcopy(expected_parameter),
            copy.deepcopy(expected_parameter),
        ],
    }
    assert any(
        "duplicate name" in error
        for error in SMOKE._program_parameter_result_errors(
            parameter_result, expected_parameter, label="result"
        )
    )

    namespace = {
        "id": "golden_ns_unit",
        "contract": "selection_context_v1",
        "prefix": "/golden/unit",
        "description": "namespace",
        "fields": [],
    }
    key = {
        "id": "golden_key_unit",
        "type": "bool",
        "key": "/golden/unit/ready",
        "description": "key",
    }
    blackboard_result = {
        "blackboard": {
            "namespaces": [copy.deepcopy(namespace), copy.deepcopy(namespace)],
            "keys": [copy.deepcopy(key), copy.deepcopy(key)],
        }
    }
    errors = SMOKE._blackboard_result_errors(
        blackboard_result, namespace, key, label="result"
    )
    assert any("namespaces contains duplicate id" in error for error in errors)
    assert any("keys contains duplicate id" in error for error in errors)


@pytest.mark.parametrize(
    ("radius", "tolerance"),
    [
        (True, 1e-6),
        (float("nan"), 1e-6),
        (0.025, True),
        (0.025, float("nan")),
    ],
)
def test_scene_validator_rejects_coercible_or_nonfinite_numbers(
    radius, tolerance
) -> None:
    expected = SMOKE._golden_scene_resources("unit")[1]
    actual = copy.deepcopy(expected)
    actual["shape"]["sphere_radius_m"] = radius

    errors = SMOKE._scene_resource_match_errors(
        actual,
        expected,
        label="scene[golden_sphere_unit]",
        tolerance=tolerance,
    )

    assert errors


def test_scene_and_string_set_validators_reject_duplicates_and_bounds() -> None:
    expected = SMOKE._golden_scene_resources("unit")
    duplicate = [copy.deepcopy(expected[0]), copy.deepcopy(expected[0])]
    duplicate_errors = SMOKE._scene_resource_set_match_errors(
        duplicate,
        expected,
        label="scene",
        require_exact=True,
    )
    assert any("duplicate objectIds" in error for error in duplicate_errors)

    oversized_errors = SMOKE._scene_resource_set_match_errors(
        [{} for _ in range(SMOKE.MAX_VALIDATION_ITEMS + 1)],
        expected,
        label="scene",
        require_exact=True,
    )
    assert any("4096-item bound" in error for error in oversized_errors)
    assert SMOKE._exact_string_set_errors(
        ["same", "same"], ["same"], label="names"
    ) == ["names contains duplicate values"]


def test_float_list_comparison_rejects_bool_nan_and_wrong_vector_shape() -> None:
    assert not SMOKE._float_lists_close([True], [1.0], tolerance=1e-6)
    assert not SMOKE._float_lists_close(
        [float("nan")], [float("nan")], tolerance=1e-6
    )
    assert not SMOKE._float_lists_close(
        [1.0, 2.0],
        [1.0, 2.0],
        tolerance=1e-6,
        expected_length=3,
    )
