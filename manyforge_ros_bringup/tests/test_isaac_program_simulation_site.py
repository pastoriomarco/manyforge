"""Product checks for the five retained Isaac Program simulator bindings."""

from __future__ import annotations

from pathlib import Path

from manyforge_ros.program_simulation_port import (
    load_simulation_program_site_config,
)


_ROOT = Path(__file__).resolve().parents[2]
_CONFIG = _ROOT / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"


def test_all_registered_isaac_scenarios_have_exact_simulator_site_bindings() -> None:
    config = load_simulation_program_site_config(_CONFIG)

    assert config.service_prefix == "auto"
    assert config.service_timeout_s == 10.0
    assert tuple(row.scenario_id for row in config.scenarios) == (
        "isaac-lite6-pick-place",
        "isaac-lite6-trocar-foundationpose",
        "isaac-ur10e-light-scene",
        "isaac-ur10e-light-scene-assistant",
        "isaac-ur10e-light-scene-collision",
    )
    expected_ur_entities = {
        "isaac_cheese_box": "/mac_n_cheese_centered",
        "isaac_small_klt": "/World/small_KLT",
        "isaac_small_klt_01": "/World/small_KLT_01",
        "isaac_soup_can": "/World/soup_can",
        "isaac_table_low_327": "/background/table_low_327",
    }
    for scenario_id in (
        "isaac-ur10e-light-scene",
        "isaac-ur10e-light-scene-assistant",
        "isaac-ur10e-light-scene-collision",
    ):
        row = config.scenario(scenario_id)
        assert row.cell_frame_id == "universe"
        assert dict(row.scene_entities) == expected_ur_entities


def test_lite6_site_bindings_do_not_alias_retained_ur10_entities() -> None:
    config = load_simulation_program_site_config(_CONFIG)

    for scenario_id in (
        "isaac-lite6-pick-place",
        "isaac-lite6-trocar-foundationpose",
    ):
        row = config.scenario(scenario_id)
        assert row.cell_frame_id == "link_base"
        assert dict(row.scene_entities) == {}
