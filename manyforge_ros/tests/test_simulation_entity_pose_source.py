"""Entity poses are a generic pose source keyed by entity name.

The per-scene mappings are Scenario data: each Isaac Scenario's
``policies.scene_pose_sources[].source_key.simulation_entity`` names a simulator entity
and the pose source component that reads it. The observer turns an entity row
into a binding keyed by that entity name.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from manyforge_ros.isaac_pose_observer import load_isaac_pose_observer_site_config

_ROOT = Path(__file__).resolve().parents[2]
_SCENARIOS = sorted((_ROOT / "examples").glob("sim_*.scenario"))


def _site_document(entities: list[dict]) -> dict:
    return {
        "api_version": "manyforge.isaac_pose_observer_site.v1",
        "cell_id": "cell",
        "detection3d_sources": [],
        "joint_state": None,
        "simulation_entities": entities,
        "simulation_poll_period_ms": 250,
        "source_id": "scene_objects",
    }


def _entity(entity: str, object_id: str) -> dict:
    return {
        "eligible": True,
        "entity": entity,
        "frame_kind": "cell_frame",
        "lifetime_ns": 2_000_000_000,
        "local_orientation_xyzw": [0, 0, 0, 1],
        "local_position_xyz": [0, 0, 0],
        "object_id": object_id,
        "policy_id": "scene",
        "source_coordinate_frame_id": "world",
        "source_header_frame_id": entity.rsplit("/", 1)[-1],
        "target_frame_id": "world",
    }


def test_isaac_scenarios_exist() -> None:
    assert _SCENARIOS


@pytest.mark.parametrize("scenario", _SCENARIOS, ids=lambda path: path.name)
def test_scenario_entity_rows_name_an_entity_of_a_simulation_pose_source(
    scenario: Path,
) -> None:
    deployment = json.loads((scenario / "deployment.yaml").read_text(encoding="utf-8"))
    sources = {
        component["component_id"]: component["configuration"]
        for component in deployment["components"]
        if component.get("configuration", {}).get("runtime_kind") == "pose_source"
    }
    assert sources, "an Isaac Scenario carries its pose source component"
    for row in deployment["policies"].get("scene_pose_sources", []):
        key = row["source_key"].get("simulation_entity")
        if key is None:
            continue
        assert key["entity"].startswith("/"), row
        configuration = sources[row["source_id"]]["runtime_configuration"]
        assert "simulation" in configuration, row


def test_observer_keys_entity_bindings_by_entity_name(tmp_path: Path) -> None:
    path = tmp_path / "pose_source.json"
    path.write_text(
        json.dumps(
            _site_document(
                [_entity("/World/box_a", "box_a"), _entity("/World/box_b", "box_b")]
            )
        ),
        encoding="utf-8",
    )
    config = load_isaac_pose_observer_site_config(path)
    by_entity = {
        binding.request.entity: binding.object_id
        for binding in config.simulation_entities
    }
    assert by_entity == {"/World/box_a": "box_a", "/World/box_b": "box_b"}
