from __future__ import annotations

import importlib.util
import math
from pathlib import Path
from types import SimpleNamespace

from launch import LaunchContext
from launch.actions import DeclareLaunchArgument, EmitEvent, OpaqueFunction
from launch_ros.actions import Node
import pytest


_PACKAGE_ROOT = Path(__file__).resolve().parents[1]
_LAUNCH_FILE = _PACKAGE_ROOT / "launch" / "isaac_topic_based_control.launch.py"


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


def _launch_context(module, *, profile: str = "ur10e") -> LaunchContext:
    context = LaunchContext()
    for entity in module.generate_launch_description().entities:
        if isinstance(entity, DeclareLaunchArgument):
            entity.execute(context)
    context.launch_configurations["isaac_profile"] = profile
    return context


def _node_executable(action: object) -> str | None:
    return getattr(action, "_Node__node_executable", None)


def test_launch_declares_exact_world_and_has_no_retired_semantic_owner() -> None:
    module = _load_launch_module()
    description = module.generate_launch_description()
    declarations = [
        action
        for action in description.entities
        if isinstance(action, DeclareLaunchArgument)
    ]

    assert module.DEFAULT_WORLD_URI == (
        "file:/home/tndlux/workspaces/isaac_ros-dev/src/"
        "isaac_sim_custom_examples/Scenario/IsaacManipulator/UniversalRobots/"
        "isaac_manipulator_scene_light.usda"
    )
    assert module.DEFAULT_WORLD_NAME == "isaac_manipulator_scene_light"
    assert {action.name for action in declarations} >= {
        "isaac_profile",
        "world_uri",
        "expected_world_name",
        "additional_required_topics",
        "command_gate_owner_node",
        "command_gate_owner_namespace",
    }
    assert sum(isinstance(action, OpaqueFunction) for action in description.entities) == 1

    source = _LAUNCH_FILE.read_text(encoding="utf-8")
    for retired_wire in (
        "isaac_demo_supervisor",
        "supervisor_enable_topic",
        "composer_url",
        "deployment_path",
        "program_path",
    ):
        assert retired_wire not in source


@pytest.mark.parametrize(
    ("profile", "raw_gripper_topic", "starts_action_bridge"),
    [
        ("ur10e", "/isaac_gripper_state", True),
        ("lite6", "/isaac_joint_commands_gripper", False),
    ],
)
def test_profile_selection_preserves_distinct_gripper_transports(
    profile: str,
    raw_gripper_topic: str,
    starts_action_bridge: bool,
) -> None:
    module = _load_launch_module()

    selected = module._profile_configuration(profile)
    assert selected["raw_gripper_topic"] == raw_gripper_topic
    assert (
        module._resolve_start_gripper_action_bridge("auto", isaac_profile=profile)
        is starts_action_bridge
    )

    if profile == "lite6":
        with pytest.raises(ValueError, match="direct-effort"):
            module._resolve_start_gripper_action_bridge(
                "true", isaac_profile=profile
            )


def test_launch_builds_barrier_first_and_releases_ur_stack_only_on_success(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare,
        "find",
        lambda _self, _name: str(_PACKAGE_ROOT),
    )
    actions = module._launch_setup(_launch_context(module, profile="ur10e"))

    assert len(actions) == 2
    barrier, registration = actions
    assert isinstance(barrier, Node)
    assert _node_executable(barrier) == "isaac_scene_load_barrier"
    event_handler = getattr(
        registration, "_RegisterEventHandler__event_handler"
    )
    assert getattr(event_handler, "_OnActionEventBase__action_matcher") is barrier
    callback = getattr(event_handler, "_OnActionEventBase__on_event")

    released = callback(SimpleNamespace(returncode=0), LaunchContext())
    assert [_node_executable(action) for action in released[:4]] == [
        "isaac_topic_command_gate",
        "robot_state_publisher",
        "static_transform_publisher",
        "isaac_gripper_action_bridge",
    ]
    assert len(released) == 7

    with pytest.raises(RuntimeError, match="return code 2"):
        callback(SimpleNamespace(returncode=2), LaunchContext())


def test_launch_lite6_stack_omits_python_gripper_action_adapter(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare,
        "find",
        lambda _self, _name: str(_PACKAGE_ROOT),
    )
    actions = module._launch_setup(_launch_context(module, profile="lite6"))
    registration = actions[1]
    event_handler = getattr(
        registration, "_RegisterEventHandler__event_handler"
    )
    callback = getattr(event_handler, "_OnActionEventBase__on_event")
    released = callback(SimpleNamespace(returncode=0), LaunchContext())

    assert "isaac_gripper_action_bridge" not in {
        _node_executable(action) for action in released
    }
    assert len(released) == 6


def test_scene_barrier_return_code_is_exact_and_fail_closed() -> None:
    module = _load_launch_module()
    sentinel = object()

    assert module._after_scene_barrier(0, [sentinel]) == [sentinel]
    for returncode in (False, None, "0", 2, -9):
        with pytest.raises(RuntimeError, match="scene-load barrier failed"):
            module._after_scene_barrier(returncode, [sentinel])


def test_launch_name_topic_and_vector_parsers_are_bounded() -> None:
    module = _load_launch_module()

    assert module._absolute_ros_name(" /clock ", name="topic") == "/clock"
    assert module._relative_ros_name("owner", name="node") == "owner"
    assert module._absolute_namespace("/cell/", name="namespace") == "/cell"
    assert module._absolute_namespace("/", name="namespace") == "/"
    assert module._parse_topic_csv("/clock, /tf", name="topics") == [
        "/clock",
        "/tf",
    ]
    assert module._parse_vector3("0 -1.25 3e-2", name="xyz") == [
        "0",
        "-1.25",
        "0.029999999999999999",
    ]
    for invalid in ("clock", "/", "/a//b", "/has space"):
        with pytest.raises(ValueError):
            module._absolute_ros_name(invalid, name="topic")
    for invalid in ("", "/owner", "some/owner", "two words"):
        with pytest.raises(ValueError):
            module._relative_ros_name(invalid, name="node")
    for invalid in ("/clock,/clock", "/clock,relative", "/clock,"):
        with pytest.raises(ValueError):
            module._parse_topic_csv(invalid, name="topics")
    for invalid in ("0 1", "0 1 2 3", f"0 1 {math.inf}"):
        with pytest.raises(ValueError):
            module._parse_vector3(invalid, name="xyz")


def test_controller_manager_resource_parsers_remain_bounded() -> None:
    module = _load_launch_module()

    assert module._parse_cpu_affinity_launch_value(
        "7, 9-11,10",
        name="controller_manager_cpu_affinity",
    ) == [7, 9, 10, 11]
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
    for value in ("3-1", "1,,2", "x", "1-2-3", "-1", "0-2147483647"):
        with pytest.raises(ValueError):
            module._parse_cpu_affinity_launch_value(
                value,
                name="controller_manager_cpu_affinity",
            )


def test_pose_source_console_entrypoint_remains_fenced(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from manyforge_ros_bringup import pose_source_snapshot_publisher

    source = Path(pose_source_snapshot_publisher.__file__).read_text(
        encoding="utf-8"
    )
    assert "def _port_target_main" not in source
    assert "rclpy" not in source
    assert "manyforge_behavior.deployment" not in source
    assert pose_source_snapshot_publisher.main([]) == 78
    assert capsys.readouterr().err.strip() == (
        pose_source_snapshot_publisher.UNAVAILABLE_MESSAGE
    )


def test_demo_supervisor_entrypoint_has_no_backup_owner(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from manyforge_ros_bringup import isaac_demo_supervisor

    source = Path(isaac_demo_supervisor.__file__).read_text(encoding="utf-8")
    assert "class IsaacDemoSupervisor" not in source
    assert "rclpy" not in source
    assert "urllib" not in source
    assert isaac_demo_supervisor.main() == 78
    assert capsys.readouterr().err.strip() == isaac_demo_supervisor.UNAVAILABLE_MESSAGE
