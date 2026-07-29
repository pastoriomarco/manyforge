from __future__ import annotations

import ast
from pathlib import Path


_OBSERVER = (
    Path(__file__).resolve().parents[1]
    / 'manyforge_xarm_adapter'
    / 'read_only_observer.py'
)


def test_observer_source_owns_no_command_capable_ros_entity() -> None:
    tree = ast.parse(_OBSERVER.read_text(encoding='utf-8'))
    called_attributes = {
        node.func.attr
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
    }

    assert 'create_client' not in called_attributes
    assert 'create_service' not in called_attributes
    assert 'create_subscription' in called_attributes
    assert 'create_publisher' in called_attributes
    assert 'ActionClient' not in _OBSERVER.read_text(encoding='utf-8')
