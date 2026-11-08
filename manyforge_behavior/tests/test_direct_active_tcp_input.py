"""A node snapshot keeps Active TCP values keyed by their exact target."""

from pathlib import Path

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueStore,
)
from manyforge_behavior.program import Program


_ROOT = Path(__file__).parents[2]
_CATALOG = NodeCatalog(Path(__file__).parents[1] / "resources/node_catalog.yaml")


def test_two_robot_current_tcp_inputs_keep_their_direct_keys() -> None:
    program = Program(_ROOT / "examples/pick_and_place_minimal.program.yaml", _CATALOG)
    program = program.with_active_tcp_binding(
        {
            "target_bindings": [
                {
                    "target": {"component_id": "robot_a", "command_target_id": "arm"},
                    "robot_tcps": [],
                },
                {
                    "target": {"component_id": "robot_b", "command_target_id": "arm"},
                    "robot_tcps": [],
                },
            ]
        }
    )
    store = ProgramValueStore(program)
    entry_a = {
        "target": {"component_id": "robot_a", "command_target_id": "arm"},
        "frame": {"component_id": "tool_a", "frame_id": "tcp"},
    }
    entry_b = {
        "target": {"component_id": "robot_b", "command_target_id": "arm"},
        "frame": {"component_id": "tool_b", "frame_id": "tcp"},
    }
    assert store.write_active_tcp("robot_a", "arm", entry_a)
    assert store.write_active_tcp("robot_b", "arm", entry_b)

    resolver = NodeAttemptInputResolver(
        program=program, catalog=_CATALOG, value_store=store
    )
    common = {
        "node_id": "timer",
        "node_instance_id": "timer",
        "parameters": {"duration_s": 1.0},
        "value_sources": {},
    }
    key_a = "active_tcp/robot_a/arm"
    key_b = "active_tcp/robot_b/arm"
    snapshot_a = resolver.snapshot(**common, derived_active_tcp_key=key_a)
    snapshot_b = resolver.snapshot(**common, derived_active_tcp_key=key_b)

    assert snapshot_a.blackboard[key_a] == entry_a
    assert key_b not in snapshot_a.blackboard
    assert snapshot_a.blackboard["active_tcp"] == entry_a
    assert snapshot_b.blackboard[key_b] == entry_b
    assert key_a not in snapshot_b.blackboard
    assert snapshot_b.blackboard["active_tcp"] == entry_b
