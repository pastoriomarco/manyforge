from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest


def _load_ros_contract_module():
    repo_root = Path(__file__).resolve().parents[1]
    contract_path = (
        repo_root
        / "exts"
        / "manyforge.isaac.collider_service"
        / "manyforge"
        / "isaac"
        / "collider_service"
        / "ros_contract.py"
    )
    spec = importlib.util.spec_from_file_location(
        "isaac_collider_service_ros_contract",
        contract_path,
    )
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ros_contract = _load_ros_contract_module()


def test_parse_query_accepts_versioned_parameterized_request() -> None:
    parsed = ros_contract.parse_query(
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
                "mesh_export_dir": "/tmp/manyforge-meshes",
                "mesh_source_preference": "collider",
            }
        )
    )

    assert parsed.request_id == "req-1"
    assert parsed.include_subtrees == ["/World"]
    assert parsed.exclude_subtrees == ["/World/robot"]
    assert parsed.probe_prims == ["/World/robot"]
    assert parsed.include_cooked_convex is True
    assert parsed.output_profile == "llm_scene_authoring"
    assert parsed.mesh_export_dir == "/tmp/manyforge-meshes"
    assert parsed.mesh_source_preference == "collider"


@pytest.mark.parametrize(
    "payload,error_fragment",
    [
        ({"include_hidden": "false"}, "include_hidden must be a boolean"),
        ({"include_subtrees": ["/World", 42]}, "Expected list of strings"),
        ({"max_colliders": 0}, "max_colliders must be > 0"),
        ({"output_profile": "yaml_export"}, "Unsupported output_profile"),
        ({"mesh_source_preference": "visual"}, "mesh_source_preference must be one of"),
        ({"purpose": ["physics"]}, "Invalid purpose"),
        ({"version": "v0"}, "Unsupported query version"),
    ],
)
def test_parse_query_rejects_unsafe_coercions(
    payload: dict[str, object],
    error_fragment: str,
) -> None:
    with pytest.raises(ValueError, match=error_fragment):
        ros_contract.parse_query(json.dumps(payload))


def test_request_id_from_text_is_best_effort() -> None:
    assert ros_contract.request_id_from_text('{"request_id":"abc"}') == "abc"
    assert ros_contract.request_id_from_text("not-json") is None


def test_parse_query_accepts_camel_case_mesh_export_dir() -> None:
    parsed = ros_contract.parse_query(
        '{"meshExportDir":"/tmp/exported-meshes","meshSourcePreference":"primitive"}'
    )

    assert parsed.mesh_export_dir == "/tmp/exported-meshes"
    assert parsed.mesh_source_preference == "primitive"


def test_parse_mesh_transfer_request_accepts_source_root_request() -> None:
    parsed = ros_contract.parse_mesh_transfer_request(
        json.dumps(
            {
                "version": "manyforge.isaac.mesh_transfer_request.v0.1",
                "request_id": "mesh-1",
                "source_root": "/World/soup_can",
                "mesh_export_dir": "/tmp/manyforge-meshes",
                "mesh_source_preference": "collider",
                "chunk_size": 65536,
            }
        )
    )

    assert parsed.request_id == "mesh-1"
    assert parsed.source_root == "/World/soup_can"
    assert parsed.mesh_resource_uri is None
    assert parsed.mesh_export_dir == "/tmp/manyforge-meshes"
    assert parsed.mesh_source_preference == "collider"
    assert parsed.chunk_size == 65536


def test_parse_mesh_transfer_request_accepts_file_uri_request() -> None:
    parsed = ros_contract.parse_mesh_transfer_request(
        '{"meshResourceUri":"file:///tmp/soup.obj","chunkSize":4096}'
    )

    assert parsed.mesh_resource_uri == "file:///tmp/soup.obj"
    assert parsed.source_root is None
    assert parsed.chunk_size == 4096


@pytest.mark.parametrize(
    "payload,error_fragment",
    [
        ({}, "exactly one"),
        (
            {"mesh_resource_uri": "file:///tmp/a.obj", "source_root": "/World/a"},
            "exactly one",
        ),
        ({"source_root": "/World/a", "chunk_size": 1024}, "chunk_size must be >="),
        (
            {"source_root": "/World/a", "chunk_size": 8 * 1024 * 1024},
            "chunk_size must be <=",
        ),
    ],
)
def test_parse_mesh_transfer_request_rejects_invalid_payloads(
    payload: dict[str, object],
    error_fragment: str,
) -> None:
    with pytest.raises(ValueError, match=error_fragment):
        ros_contract.parse_mesh_transfer_request(json.dumps(payload))
