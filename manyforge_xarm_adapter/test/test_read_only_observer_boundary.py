"""Source boundary for the removed duplicate read-only observer owner."""

from __future__ import annotations

import ast
from pathlib import Path

from manyforge_xarm_adapter import read_only_observer as subject


HISTORICAL_TEST_BLOB = "5a80ce7fcef5cd1416a1c7e96e37cb0cc4af0070"
HISTORICAL_CASE = "test_observer_source_owns_no_command_capable_ros_entity"

_OBSERVER = (
    Path(__file__).resolve().parents[1]
    / "manyforge_xarm_adapter"
    / "read_only_observer.py"
)


def test_observer_source_is_a_pre_import_gate_not_a_dormant_ros_owner() -> None:
    source = _OBSERVER.read_text(encoding="utf-8")
    tree = ast.parse(source)
    imported_modules = {
        alias.name
        for node in ast.walk(tree)
        if isinstance(node, ast.Import)
        for alias in node.names
    } | {
        node.module or ""
        for node in ast.walk(tree)
        if isinstance(node, ast.ImportFrom)
    }

    assert not any(
        name.startswith(
            ("rclpy", "diagnostic_msgs", "sensor_msgs", "xarm_msgs")
        )
        for name in imported_modules
    )
    assert "class ReadOnlyObserver" not in source
    assert "create_subscription" not in source
    assert "create_publisher" not in source


def test_boundary_case_and_source_remain_pinned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "9d51f0b9910e2051f247726bef6796ef3ddc9da3"
    )
    assert HISTORICAL_TEST_BLOB == (
        "5a80ce7fcef5cd1416a1c7e96e37cb0cc4af0070"
    )
    assert HISTORICAL_CASE.endswith("command_capable_ros_entity")
