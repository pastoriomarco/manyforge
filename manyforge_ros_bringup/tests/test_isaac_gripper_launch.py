"""Merge 6: the Isaac tool (gripper-only) component's own launch."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

from launch import LaunchContext
from launch.actions import DeclareLaunchArgument
from launch_ros.utilities import evaluate_parameters
import pytest


_PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(_PACKAGE_ROOT) not in sys.path:  # the launch files import manyforge_ros_bringup
    sys.path.insert(0, str(_PACKAGE_ROOT))
_LAUNCH_FILE = _PACKAGE_ROOT / "launch" / "isaac_gripper.launch.py"
_NAMESPACE = "/manyforge/ur10e_gripper"
_BRIDGE_FILE = (
    _PACKAGE_ROOT.parent
    / "manyforge_isaac_adapter" / "lane" / "gripper_bridges" / "robotiq_2f_140.yaml"
)


def _load_launch_module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_isaac_gripper_launch_test", _LAUNCH_FILE
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _node_executable(action: object) -> str | None:
    return getattr(action, "_Node__node_executable", None)


def _context(module, tmp_path: Path, **values: str) -> LaunchContext:
    description = tmp_path / "robot_description.urdf"
    description.write_text('<robot name="tool"><link name="tool_mount"/></robot>', encoding="utf-8")
    context = LaunchContext()
    context.launch_configurations.update(
        robot_description_file=str(description), runtime_namespace=_NAMESPACE
    )
    for entity in module.generate_launch_description().entities:
        if isinstance(entity, DeclareLaunchArgument):
            entity.execute(context)
    context.launch_configurations.update(values)
    return context


def _bridge_values() -> dict[str, str]:
    # The host's arguments for the Robotiq transport (the tool's lanes.isaac row).
    return {
        "gripper_bridge_parameters": str(_BRIDGE_FILE),
        "gripper_action_name": "robotiq_gripper_controller/gripper_cmd",
        "gripper_command_topic": "/isaac_gripper_state",
        "gripper_state_topic": "/isaac_joint_states",
        "gripper_state_freshness_timeout_s": "0.75",
    }


def test_direct_transport_publishes_only_the_tool_description(tmp_path: Path) -> None:
    module = _load_launch_module()
    context = _context(module, tmp_path, frame_prefix="tool/")
    actions = module._launch_setup(context)

    assert [_node_executable(action) for action in actions] == ["robot_state_publisher"]
    assert evaluate_parameters(context, actions[0]._Node__parameters)[0] == {
        "robot_description": '<robot name="tool"><link name="tool_mount"/></robot>',
        "frame_prefix": "tool/",
    }


def test_action_transport_runs_the_bridge_in_the_tool_namespace(tmp_path: Path) -> None:
    module = _load_launch_module()
    context = _context(module, tmp_path, **_bridge_values())
    actions = module._launch_setup(context)

    assert [_node_executable(action) for action in actions] == [
        "robot_state_publisher",
        "isaac_gripper_action_bridge",
    ]
    bridge = actions[1]
    parameters = evaluate_parameters(context, bridge._Node__parameters)
    assert Path(str(parameters[0])) == _BRIDGE_FILE
    assert parameters[-1] == {
        "action_name": _NAMESPACE + "/robotiq_gripper_controller/gripper_cmd",
        "command_topic": "/isaac_gripper_state",
        "state_topic": "/isaac_joint_states",
        "readiness_topic": _NAMESPACE + "/manyforge/isaac_gripper_bridge/ready",
        "state_freshness_timeout_s": 0.75,
    }
    assert getattr(bridge, "_ExecuteLocal__on_exit") is not None


def test_missing_description_or_bridge_budget_refuses(tmp_path: Path) -> None:
    module = _load_launch_module()
    missing = _context(module, tmp_path)
    missing.launch_configurations["robot_description_file"] = str(tmp_path / "absent")
    with pytest.raises(ValueError, match="robot_description_file"):
        module._launch_setup(missing)

    values = _bridge_values()
    values["gripper_state_freshness_timeout_s"] = ""
    with pytest.raises(ValueError):
        module._launch_setup(_context(module, tmp_path, **values))

    relative = _bridge_values()
    relative["gripper_bridge_parameters"] = "robotiq_2f_140.yaml"
    with pytest.raises(ValueError, match="absolute"):
        module._launch_setup(_context(module, tmp_path, **relative))


def test_owned_nodes_disable_external_library_log_files() -> None:
    import ast

    nodes = [
        node
        for node in ast.walk(ast.parse(_LAUNCH_FILE.read_text(encoding="utf-8")))
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "Node"
    ]
    assert len(nodes) == 2
    for node in nodes:
        arguments = next(
            keyword.value for keyword in node.keywords if keyword.arg == "ros_arguments"
        )
        assert ast.literal_eval(arguments) == ["--disable-external-lib-logs"]
