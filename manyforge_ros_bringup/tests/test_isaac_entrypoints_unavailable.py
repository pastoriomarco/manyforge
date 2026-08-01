"""Source-level acceptance for pre-I9 ROS bringup entrypoint fences."""

from __future__ import annotations

import inspect

import pytest

from manyforge_ros_bringup import isaac_gripper_action_bridge
from manyforge_ros_bringup import isaac_topic_command_gate


@pytest.mark.parametrize(
    "module",
    [isaac_gripper_action_bridge, isaac_topic_command_gate],
)
def test_isaac_command_entrypoints_fail_before_ros_initialization(
    module: object,
    capsys: pytest.CaptureFixture[str],
) -> None:
    assert module.main() == 78
    assert capsys.readouterr().err.strip() == module.UNAVAILABLE_MESSAGE

    source = inspect.getsource(module.main)
    for forbidden in ("rclpy", "spin", "create_node", "publish"):
        assert forbidden not in source


def test_isaac_stop_topic_translation_uses_one_owner_fact() -> None:
    assert (
        isaac_topic_command_gate.stop_topic_value(owner_command_enabled=True)
        is False
    )
    assert (
        isaac_topic_command_gate.stop_topic_value(owner_command_enabled=False)
        is True
    )


@pytest.mark.parametrize("value", ["true", "false", 1, 0, None])
def test_isaac_stop_topic_translation_rejects_non_boolean_owner_facts(
    value: object,
) -> None:
    with pytest.raises(TypeError, match="must be an exact bool"):
        isaac_topic_command_gate.stop_topic_value(
            owner_command_enabled=value  # type: ignore[arg-type]
        )
