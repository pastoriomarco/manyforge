from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest


_LAUNCH_FILE = (
    Path(__file__).resolve().parents[1]
    / "launch"
    / "isaac_topic_based_control.launch.py"
)


def _load_launch_module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_isaac_topic_based_control_launch_test",
        _LAUNCH_FILE,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_controller_manager_cpu_affinity_parser_accepts_ranges_and_deduplicates():
    module = _load_launch_module()

    assert module._parse_cpu_affinity_launch_value(
        "7, 9-11,10",
        name="controller_manager_cpu_affinity",
    ) == [7, 9, 10, 11]
    assert module._parse_cpu_affinity_launch_value(
        "",
        name="controller_manager_cpu_affinity",
    ) == []


@pytest.mark.parametrize(
    "value",
    ["3-1", "1,,2", "x", "1-2-3", "-1", "0-2147483647"],
)
def test_controller_manager_cpu_affinity_parser_rejects_malformed_values(value):
    module = _load_launch_module()

    with pytest.raises(ValueError):
        module._parse_cpu_affinity_launch_value(
            value,
            name="controller_manager_cpu_affinity",
        )


def test_controller_manager_priority_and_lock_parsers_are_bounded():
    module = _load_launch_module()

    assert module._parse_bool_launch_value("true", name="lock_memory") is True
    assert module._parse_bool_launch_value("OFF", name="lock_memory") is False
    assert (
        module._parse_bounded_int_launch_value(
            "50",
            name="thread_priority",
            minimum=1,
            maximum=99,
        )
        == 50
    )
    with pytest.raises(ValueError):
        module._parse_bool_launch_value("sometimes", name="lock_memory")
    with pytest.raises(ValueError):
        module._parse_bounded_int_launch_value(
            "100",
            name="thread_priority",
            minimum=1,
            maximum=99,
        )
