from __future__ import annotations

import importlib.util
import math
from pathlib import Path
import sys

from launch import LaunchContext
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.utilities import perform_substitutions
from launch_ros.utilities import evaluate_parameters
import pytest
from rclpy.parameter import parameter_dict_from_yaml_file


_PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(_PACKAGE_ROOT) not in sys.path:  # the launch files import manyforge_ros_bringup
    sys.path.insert(0, str(_PACKAGE_ROOT))
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


@pytest.fixture(autouse=True)
def _supplied_artifacts(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # Action-wiring fixture only: no generated-lane validation or RSP graph.
    description = tmp_path / "supplied description.urdf"
    description.write_text(
        '<robot name="explicit_fixture" version="1.0">'
        '<link name="base"/><!-- preserve {literal_braces} -->'
        '</robot>', encoding="utf-8"
    )
    controllers = tmp_path / "supplied controllers.yaml"
    controllers.write_text("/**/controller_manager:\n  ros__parameters:\n    update_rate: 100\n", encoding="utf-8")
    monkeypatch.setitem(_SUPPLIED_ARTIFACTS, "robot_description_file", str(description))
    monkeypatch.setitem(_SUPPLIED_ARTIFACTS, "controllers_file", str(controllers))


_SUPPLIED_ARTIFACTS: dict[str, str] = {}


def _launch_context(module, *, profile: str = "ur10e") -> LaunchContext:
    context = LaunchContext()
    context.launch_configurations.update(_SUPPLIED_ARTIFACTS)
    for entity in module.generate_launch_description().entities:
        if isinstance(entity, DeclareLaunchArgument):
            entity.execute(context)
    context.launch_configurations["isaac_profile"] = profile
    return context


def _node_executable(action: object) -> str | None:
    return getattr(action, "_Node__node_executable", None)


def _node_namespace(action: object, context: LaunchContext) -> str:
    substitutions = getattr(action, "_Node__node_namespace")
    return "".join(
        value
        if isinstance(value, str)
        else perform_substitutions(context, [value])
        for value in substitutions
    )


def test_launch_declares_runtime_inputs_and_has_no_retired_semantic_owner() -> None:
    module = _load_launch_module()
    description = module.generate_launch_description()
    declarations = [
        action
        for action in description.entities
        if isinstance(action, DeclareLaunchArgument)
    ]

    assert (
        module.DEFAULT_RUNTIME_NAMESPACE
        == "/manyforge/isaac_ur10e_light_robot"
    )
    assert {action.name for action in declarations} >= {
        "isaac_profile",
        "robot_description_file",
        "controllers_file",
        "runtime_namespace",
        "manager_run_root",
        "pose_observer_site_config",
        "pointcloud_topic",
        "pointcloud_cell_id",
    }
    assert (
        sum(isinstance(action, OpaqueFunction) for action in description.entities)
        == 1
    )

    source = _LAUNCH_FILE.read_text(encoding="utf-8")
    assert "isaac_topic_command_gate" not in source
    assert "stop_joint_commands" not in source
    for retired_wire in (
        "isaac_demo_supervisor",
        "supervisor_enable_topic",
        "composer_url",
        "deployment_path",
        "program_path",
    ):
        assert retired_wire not in source


@pytest.mark.parametrize("name", ("robot_description_file", "controllers_file"))
def test_missing_artifact_refuses_before_runtime_action_construction(
    name: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(module.FindPackageShare, "find", lambda _self, _name: str(_PACKAGE_ROOT))
    context = _launch_context(module)
    context.launch_configurations[name] = str(tmp_path / "missing")
    monkeypatch.setattr(module, "Node", lambda **_kwargs: pytest.fail("runtime action constructed before artifact check"))
    with pytest.raises(ValueError, match=f"{name} must be a readable"):
        module._launch_setup(context)


def test_artifact_paths_accept_readable_relative_files(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(module.FindPackageShare, "find", lambda _self, _name: str(_PACKAGE_ROOT))
    context = _launch_context(module)
    parent = Path(_SUPPLIED_ARTIFACTS["robot_description_file"]).parent
    monkeypatch.chdir(parent)
    for name, path in _SUPPLIED_ARTIFACTS.items():
        context.launch_configurations[name] = Path(path).name
    assert len(module._launch_setup(context)) == 6


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


def test_launch_attaches_controller_stack_with_inactive_arm_spawner(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare,
        "find",
        lambda _self, _name: str(_PACKAGE_ROOT),
    )
    context = _launch_context(module, profile="ur10e")
    actions = module._launch_setup(context)

    assert len(actions) == 6
    assert [_node_executable(action) for action in actions[:3]] == [
        "robot_state_publisher",
        "static_transform_publisher",
        "isaac_gripper_action_bridge",
    ]
    assert all(
        _node_namespace(action, context) == module.DEFAULT_RUNTIME_NAMESPACE
        for action in actions[:3]
    )
    timed_nodes = [
        getattr(action, "_TimerAction__actions")[0]
        for action in actions[3:]
    ]
    assert all(
        _node_namespace(action, context) == module.DEFAULT_RUNTIME_NAMESPACE
        for action in timed_nodes
    )
    expected_manager = (
        module.DEFAULT_RUNTIME_NAMESPACE + "/controller_manager"
    )
    assert "--inactive" in getattr(timed_nodes[-1], "_Node__arguments")
    arm_arguments = getattr(timed_nodes[-1], "_Node__arguments")
    assert arm_arguments.count("--param-file") == 1
    assert arm_arguments[arm_arguments.index("--param-file") + 1] == (
        _SUPPLIED_ARTIFACTS["controllers_file"]
    )
    expected_description = Path(
        _SUPPLIED_ARTIFACTS["robot_description_file"]
    ).read_text(encoding="utf-8")
    assert evaluate_parameters(context, actions[0]._Node__parameters)[0] == {
        "robot_description": expected_description, "frame_prefix": "",
    }
    assert evaluate_parameters(context, timed_nodes[0]._Node__parameters)[0] == {
        "robot_description": expected_description,
    }
    assert evaluate_parameters(context, getattr(timed_nodes[0], "_Node__parameters"))[-1] == Path(_SUPPLIED_ARTIFACTS["controllers_file"])
    for spawner in timed_nodes[1:]:
        arguments = getattr(spawner, "_Node__arguments")
        assert arguments[arguments.index("--controller-manager") + 1] == (
            expected_manager
        )

@pytest.mark.parametrize("profile", ["ur10e", "lite6"])
@pytest.mark.parametrize("prefix", ["", "authored_component/"])
def test_explicit_prefix_changes_rsp_and_static_child_only(
    monkeypatch: pytest.MonkeyPatch, profile: str, prefix: str,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare, "find", lambda _self, _name: str(_PACKAGE_ROOT),
    )
    context = _launch_context(module, profile=profile)
    assert context.launch_configurations["frame_prefix"] == ""
    context.launch_configurations.update(
        frame_prefix=prefix, base_parent_frame="authored_global_world",
        base_child_frame="authored_model_root",
    )
    actions = module._launch_setup(context)
    rsp, static = actions[:2]
    description = Path(_SUPPLIED_ARTIFACTS["robot_description_file"]).read_text()
    assert evaluate_parameters(context, rsp._Node__parameters)[0] == {
        "robot_description": description, "frame_prefix": prefix,
    }
    arguments = [
        value if isinstance(value, str) else perform_substitutions(context, [value])
        for value in static._Node__arguments
    ]
    assert arguments[arguments.index("--frame-id") + 1] == "authored_global_world"
    assert arguments[arguments.index("--child-frame-id") + 1] == (
        prefix + "authored_model_root"
    )
    manager = next(
        action._TimerAction__actions[0] for action in actions
        if hasattr(action, "_TimerAction__actions")
        and _node_executable(action._TimerAction__actions[0]) == "ros2_control_node"
    )
    assert evaluate_parameters(context, manager._Node__parameters)[0] == {
        "robot_description": description,
    }
    assert evaluate_parameters(context, manager._Node__parameters)[-1] == (
        Path(_SUPPLIED_ARTIFACTS["controllers_file"])
    )
    assert _node_namespace(rsp, context) == module.DEFAULT_RUNTIME_NAMESPACE


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

    assert "isaac_gripper_action_bridge" not in {
        _node_executable(action) for action in actions
    }
    assert len(actions) == 5


def test_launch_includes_exact_pose_observer_with_controller_stack(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare,
        "find",
        lambda _self, _name: str(_PACKAGE_ROOT),
    )
    context = _launch_context(module, profile="ur10e")
    context.launch_configurations["pose_observer_site_config"] = (
        "/opt/manyforge/isaac-ur10e-light.pose-observer.json"
    )
    context.launch_configurations["manager_run_root"] = "/run/manyforge"
    actions = module._launch_setup(context)

    assert [_node_executable(action) for action in actions].count(
        "pose_source_snapshot_publisher"
    ) == 1
    observer = next(
        action
        for action in actions
        if _node_executable(action) == "pose_source_snapshot_publisher"
    )
    assert getattr(observer, "_ExecuteLocal__on_exit") is not None

    for missing_name in ("pose_observer_site_config", "manager_run_root"):
        invalid = _launch_context(module, profile="ur10e")
        invalid.launch_configurations[missing_name] = "/configured/value"
        with pytest.raises(ValueError, match="configured together"):
            module._launch_setup(invalid)


def test_launch_adds_demand_pointcloud_bridge_only_with_exact_cell_inputs(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    module = _load_launch_module()
    monkeypatch.setattr(
        module.FindPackageShare,
        "find",
        lambda _self, _name: str(_PACKAGE_ROOT),
    )
    context = _launch_context(module, profile="ur10e")
    context.launch_configurations["pointcloud_topic"] = "/isaac_cell_cloud"
    context.launch_configurations["pointcloud_cell_id"] = "isaac.cell.1"
    actions = module._launch_setup(context)

    bridge = next(
        action
        for action in actions
        if _node_executable(action) == "isaac_pointcloud_observer_bridge"
    )
    assert _node_namespace(bridge, context) == module.DEFAULT_RUNTIME_NAMESPACE
    assert getattr(bridge, "_ExecuteLocal__on_exit") is not None

    invalid = _launch_context(module, profile="ur10e")
    invalid.launch_configurations["pointcloud_topic"] = "/isaac_cell_cloud"
    with pytest.raises(ValueError, match="pointcloud_cell_id"):
        module._launch_setup(invalid)


def test_launch_name_topic_and_vector_parsers_are_bounded() -> None:
    module = _load_launch_module()

    assert module._absolute_ros_name(" /clock ", name="topic") == "/clock"
    assert module._relative_ros_name("owner", name="node") == "owner"
    assert module._absolute_namespace("/cell/", name="namespace") == "/cell"
    assert module._absolute_namespace("/", name="namespace") == "/"
    assert (
        module._instance_namespace(
            "/manyforge/robot_a", name="runtime_namespace"
        )
        == "/manyforge/robot_a"
    )
    assert (
        module._scoped_ros_name(
            "controller/action",
            runtime_namespace="/manyforge/robot_a",
            name="topic",
        )
        == "/manyforge/robot_a/controller/action"
    )
    assert module._parse_vector3("0 -1.25 3e-2", name="xyz") == [
        "0",
        "-1.25",
        "0.029999999999999999",
    ]
    for invalid in ("clock", "/", "/a//b", "/has space"):
        with pytest.raises(ValueError):
            module._absolute_ros_name(invalid, name="topic")
    for invalid in (
        "",
        "/owner",
        "some/owner",
        "two words",
        "9owner",
        "owner-name",
        "owner.name",
    ):
        with pytest.raises(ValueError):
            module._relative_ros_name(invalid, name="node")
    for invalid in (
        "/",
        "/cell/robot_a",
        "/manyforge/robot.a",
        "/manyforge/robot_a/arm",
    ):
        with pytest.raises(ValueError):
            module._instance_namespace(invalid, name="runtime_namespace")
    for invalid in ("/absolute", "controller/", "controller//action"):
        with pytest.raises(ValueError):
            module._scoped_ros_name(
                invalid,
                runtime_namespace="/manyforge/robot_a",
                name="topic",
            )
    assert module._optional_absolute_path("", name="path") == ""
    assert module._optional_absolute_path(" /run/manyforge ", name="path") == (
        "/run/manyforge"
    )
    for invalid in ("relative", "/" + ("x" * 4096), "/bad\x00path"):
        with pytest.raises(ValueError):
            module._optional_absolute_path(invalid, name="path")
    assert module._bounded_observer_identity(
        "isaac.cell/1",
        name="identity",
        maximum_bytes=256,
    ) == "isaac.cell/1"
    for invalid in ("", " bad", "bad value", "-bad"):
        with pytest.raises(ValueError):
            module._bounded_observer_identity(
                invalid,
                name="identity",
                maximum_bytes=256,
            )
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


@pytest.mark.parametrize("profile", ("ur10e", "lite6"))
def test_controller_parameters_follow_each_runtime_namespace(profile: str) -> None:
    module = _load_launch_module()
    parameter_file = (
        _PACKAGE_ROOT / "config" / f"isaac_{profile}_topic_based_controllers.yaml"
    )
    manager = parameter_dict_from_yaml_file(
        str(parameter_file),
        use_wildcard=True,
        target_nodes=["/manyforge/robot_a/controller_manager"],
    )
    controller = parameter_dict_from_yaml_file(
        str(parameter_file),
        use_wildcard=True,
        target_nodes=["/manyforge/robot_a/isaac_arm_controller"],
    )

    assert manager["update_rate"].value.integer_value == 100
    assert manager["isaac_arm_controller.type"].value.string_value == (
        "joint_trajectory_controller/JointTrajectoryController"
    )
    assert "joints" not in manager
    assert len(controller["joints"].value.string_array_value) == 6
    assert controller["constraints.decelerate_on_cancel"].value.bool_value
    assert controller["interpolate_from_desired_state"].value.bool_value is True
    assert controller["set_last_command_interface_value_as_state_on_activation"].value.bool_value is True
    assert "update_rate" not in controller


def test_pose_source_console_entrypoint_fails_closed_without_exact_site_inputs(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from manyforge_ros_bringup import pose_source_snapshot_publisher

    source = Path(pose_source_snapshot_publisher.__file__).read_text(
        encoding="utf-8"
    )
    assert "def _port_target_main" not in source
    assert "manyforge_behavior.deployment" not in source
    assert "composer_url" not in source
    assert pose_source_snapshot_publisher.main([]) == 78
    assert "site_config_path is required" in capsys.readouterr().err


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


def test_owned_nodes_disable_external_library_log_files():
    import ast

    source = _LAUNCH_FILE.read_text(encoding="utf-8")
    nodes = [
        node for node in ast.walk(ast.parse(source))
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
        and node.func.id == "Node"
    ]
    assert nodes
    for node in nodes:
        arguments = next(keyword.value for keyword in node.keywords if keyword.arg == "ros_arguments")
        assert ast.literal_eval(arguments) == ["--disable-external-lib-logs"]
