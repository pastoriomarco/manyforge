"""Product check for the Isaac Program simulator site configuration."""

from __future__ import annotations

from pathlib import Path

from manyforge_ros.program_simulation_port import (
    load_simulation_program_site_config,
)


_ROOT = Path(__file__).resolve().parents[2]
_CONFIG = _ROOT / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"


def test_isaac_site_config_holds_only_machine_facts() -> None:
    # The entity mapping and cell frame come from the Deployment object links.
    config = load_simulation_program_site_config(_CONFIG)

    assert config.service_prefix == "auto"
    assert config.service_timeout_s == 10.0
