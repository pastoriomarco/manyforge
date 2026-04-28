from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest


def _load_query_module():
    repo_root = Path(__file__).resolve().parents[1]
    query_path = (
        repo_root
        / "exts"
        / "manyforge.isaac.collider_service"
        / "manyforge"
        / "isaac"
        / "collider_service"
        / "query.py"
    )
    spec = importlib.util.spec_from_file_location("isaac_collider_service_query", query_path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


query = _load_query_module()


def test_parse_query_accepts_versioned_parameterized_request() -> None:
    parsed = query.parse_query(
        json.dumps(
            {
                "version": "manyforge.isaac.collider_query.v0.1",
                "request_id": "req-1",
                "include_subtrees": ["/World"],
                "exclude_subtrees": ["/World/robot"],
                "probe_prims": ["/World/robot"],
                "include_hidden": False,
                "include_cooked_convex": True,
                "include_cooked_samples": False,
                "max_colliders": 200,
                "purpose": ["default", "proxy"],
                "output_profile": "llm_scene_authoring",
            }
        )
    )

    assert parsed.request_id == "req-1"
    assert parsed.include_subtrees == ["/World"]
    assert parsed.exclude_subtrees == ["/World/robot"]
    assert parsed.probe_prims == ["/World/robot"]
    assert parsed.include_cooked_convex is True
    assert parsed.output_profile == "llm_scene_authoring"


@pytest.mark.parametrize(
    "payload,error_fragment",
    [
        ({"include_hidden": "false"}, "include_hidden must be a boolean"),
        ({"include_subtrees": ["/World", 42]}, "Expected list of strings"),
        ({"max_colliders": 0}, "max_colliders must be > 0"),
        ({"output_profile": "yaml_export"}, "Unsupported output_profile"),
        ({"purpose": ["physics"]}, "Invalid purpose"),
        ({"version": "v0"}, "Unsupported query version"),
    ],
)
def test_parse_query_rejects_unsafe_coercions(
    payload: dict[str, object],
    error_fragment: str,
) -> None:
    with pytest.raises(ValueError, match=error_fragment):
        query.parse_query(json.dumps(payload))


def test_request_id_from_text_is_best_effort() -> None:
    assert query.request_id_from_text('{"request_id":"abc"}') == "abc"
    assert query.request_id_from_text("not-json") is None
