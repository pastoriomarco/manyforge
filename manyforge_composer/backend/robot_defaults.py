"""Generated from manyforge_core/resources/robot_defaults.tsv by manyforge_core/tools/generate_robot_defaults.py; do not edit."""

from typing import Any

ROBOT_RUNTIME_DEFAULTS: dict[str, Any] = {
    "execution_tick_hz": 100,
    "collision_monitor_period_ms": 50.0,
    "planner_rng_seed": 12345,
}

ROBOT_OPTIONAL_DEFAULTS: dict[str, Any] = {
    "collision_clearance_m": 0.001,
    "collision_check_step_rad": 0.05,
    "collision_check_fresh_plans": True,
    "planner_search": "rrt_connect",
    "planner_optimize": "first",
    "planner_n": 4,
    "planner_max_nodes": 5000,
    "cartesian_timing": "constant_speed",
    "motion_timing": "smooth",
}

REAL_ROBOT_OPTIONAL_DEFAULTS: dict[str, Any] = {
    "stop_lead_in_anchor": "commanded",
    "controller_tcp_offset_sync": True,
}

COMPONENT_DEFAULTS: dict[str, Any] = {
    "lifecycle_command_timeout_s": 10.0,
}
