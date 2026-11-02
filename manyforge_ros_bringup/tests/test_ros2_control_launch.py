from __future__ import annotations

import importlib.util
from pathlib import Path
import sys

from launch import LaunchContext
from launch.actions import DeclareLaunchArgument
from launch_ros.actions import Node
from launch_ros.utilities import evaluate_parameters
import pytest
import yaml


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:  # the launch files import manyforge_ros_bringup
    sys.path.insert(0, str(ROOT))


def _module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_ros2_control_launch", ROOT / "launch/ros2_control.launch.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _context(
    tmp_path, *, plugin="mock_components/GenericSystem", gripper_name="", frame_prefix="",
    reset_position_name="", hardware=None,
):
    description = tmp_path / "retained.urdf"
    description.write_text(
        f'<robot name="fixture"><link name="base"/>'
        f'<ros2_control name="arm" type="system"><hardware><plugin>{plugin}</plugin>'
        '<param name="calculate_dynamics">true</param></hardware>'
        '<joint name="joint1"><command_interface name="position"/>'
        '<state_interface name="position"><param name="initial_value">0.4</param>'
        '</state_interface><state_interface name="velocity"/></joint>'
        '</ros2_control></robot>', encoding="utf-8"
    )
    controllers = tmp_path / "controllers.yaml"
    controllers.write_text("{}\n", encoding="utf-8")
    context = LaunchContext()
    context.launch_configurations.update(
        robot_description_file=str(description), controllers_file=str(controllers),
        runtime_namespace="/fixture/arm", controller_manager_name="control",
        arm_controller_name="arm_controller",
        reset_position_controller_name=reset_position_name,
        gripper_controller_name=gripper_name,
        frame_prefix=frame_prefix,
        hardware_plugin=plugin,
    )
    # Unset optional hardware arguments take their launch defaults (none).
    for action in _module().generate_launch_description().entities:
        if isinstance(action, DeclareLaunchArgument) and action.name not in (
            context.launch_configurations
        ):
            action.execute(context)
    context.launch_configurations.update(hardware or {})
    return context


def test_launch_requires_retained_artifacts_and_no_second_pose_input():
    module = _module()
    declarations = {
        action.name for action in module.generate_launch_description().entities
        if isinstance(action, DeclareLaunchArgument)
    }
    assert declarations == {
        "robot_description_file", "controllers_file", "runtime_namespace",
        "controller_manager_name", "arm_controller_name",
        "reset_position_controller_name",
        "gripper_controller_name",
        "frame_prefix",
        "hardware_plugin", "driver_parameters_file", "hardware_remappings",
        "world_parent_frame",
    }


def test_observation_and_inactive_arm_use_exact_manager_route(tmp_path):
    module = _module()
    context = _context(tmp_path)
    nodes = module.launch_setup(context)
    assert len(nodes) == 4 and all(isinstance(node, Node) for node in nodes)
    assert [node._Node__node_executable for node in nodes] == [
        "robot_state_publisher", "ros2_control_node", "spawner", "spawner",
    ]
    # Spawners load observation and an inactive arm. No activate-controller
    # service call here: activation is the component adapter's Enable.
    assert nodes[2]._Node__arguments == [
        "joint_state_broadcaster", "--controller-manager", "/fixture/arm/control",
    ]
    assert nodes[3]._Node__arguments == [
        "arm_controller", "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    publisher_parameters = evaluate_parameters(context, nodes[0]._Node__parameters)
    assert 'name="initial_value">0.4' in publisher_parameters[0]["robot_description"]
    assert publisher_parameters[0]["use_sim_time"] is False
    assert publisher_parameters[0]["frame_prefix"] == ""
    manager_parameters = evaluate_parameters(context, nodes[1]._Node__parameters)
    assert manager_parameters[0] == tmp_path / "controllers.yaml"
    assert manager_parameters[1] == {"use_sim_time": False}
    assert len(manager_parameters) == 2
    assert _remaps(context, nodes[1]) == [("~/robot_description", "robot_description")]
    # Only the manager node is named; a process-wide name would also rename
    # the hardware plugin's own in-process nodes.
    assert nodes[1]._Node__node_name is None
    assert nodes[1]._Node__ros_arguments == [
        "--disable-external-lib-logs", "-r", "controller_manager:__node:=control",
    ]


def _remaps(context, node):
    from launch.utilities import perform_substitutions

    return [
        (perform_substitutions(context, source), perform_substitutions(context, target))
        for source, target in node._Node__remappings
    ]


def test_configuration_scoped_position_writer_is_loaded_inactive(tmp_path):
    writer = "manyforge_reset_position_" + "a" * 64
    nodes = _module().launch_setup(_context(tmp_path, reset_position_name=writer))
    assert nodes[4]._Node__arguments == [
        writer, "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    assert len(nodes) == 5


@pytest.mark.parametrize("prefix", ["", "authored_component/"])
def test_a_world_parent_frame_publishes_the_static_world_transform(tmp_path, prefix):
    # Isaac (as before v2): the scene's `universe` holds `<prefix>world`.
    context = _context(
        tmp_path, frame_prefix=prefix, hardware={"world_parent_frame": "universe"}
    )
    nodes = _module().launch_setup(context)
    assert [node._Node__node_executable for node in nodes] == [
        "robot_state_publisher", "ros2_control_node", "spawner",
        "static_transform_publisher", "spawner",
    ]
    assert nodes[3]._Node__arguments == [
        "--frame-id", "universe", "--child-frame-id", f"{prefix}world",
    ]
    assert nodes[3]._Node__node_namespace == "/fixture/arm"


@pytest.mark.parametrize("prefix", ["", "authored_component/"])
def test_explicit_prefix_only_changes_rsp_frames(tmp_path, prefix):
    module = _module()
    context = _context(tmp_path, frame_prefix=prefix)
    nodes = module.launch_setup(context)
    assert len(nodes) == 4
    parameters = evaluate_parameters(context, nodes[0]._Node__parameters)[0]
    assert parameters["frame_prefix"] == prefix
    assert parameters["robot_description"] == (tmp_path / "retained.urdf").read_text()
    assert evaluate_parameters(context, nodes[1]._Node__parameters)[0] == (
        tmp_path / "controllers.yaml"
    )
    assert nodes[3]._Node__arguments == [
        "arm_controller", "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    assert all(node._Node__node_executable != "static_transform_publisher" for node in nodes)
    declaration = next(
        action for action in module.generate_launch_description().entities
        if isinstance(action, DeclareLaunchArgument) and action.name == "frame_prefix"
    )
    defaults = LaunchContext()
    declaration.execute(defaults)
    assert defaults.launch_configurations["frame_prefix"] == ""


@pytest.mark.parametrize(
    "gripper_name",
    ["lite_gripper_controller", "robotiq_gripper_controller", "authored_tool_controller"],
)
def test_optional_authored_gripper_starts_inactive_on_the_same_manager(tmp_path, gripper_name):
    nodes = _module().launch_setup(_context(tmp_path, gripper_name=gripper_name))
    assert len(nodes) == 5
    assert nodes[3]._Node__arguments == [
        "arm_controller", "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    assert nodes[4]._Node__node_executable == "spawner"
    assert nodes[4]._Node__arguments == [
        gripper_name, "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    # Generated description/controllers remain the only joint/seed source.
    context = _context(tmp_path, gripper_name=gripper_name)
    assert evaluate_parameters(context, nodes[1]._Node__parameters)[0] == (
        tmp_path / "controllers.yaml"
    )


@pytest.mark.parametrize("gripper_name", ["lite_gripper_controller", "robotiq_gripper_controller"])
def test_gripper_only_spawns_real_gripper_without_an_arm(tmp_path, gripper_name):
    module = _module()
    context = _context(tmp_path, gripper_name=gripper_name, frame_prefix="tool/")
    # Match the gripper-only generator output: actual commanded model joint.
    description_path = tmp_path / "retained.urdf"
    description_path.write_text(
        description_path.read_text().replace('name="arm"', 'name="tool"')
        .replace('name="joint1"', 'name="right_finger_joint"'),
        encoding="utf-8",
    )
    del context.launch_configurations["arm_controller_name"]
    declaration = next(
        action for action in module.generate_launch_description().entities
        if isinstance(action, DeclareLaunchArgument) and action.name == "arm_controller_name"
    )
    declaration.execute(context)
    assert context.launch_configurations["arm_controller_name"] == ""
    nodes = module.launch_setup(context)
    assert len(nodes) == 4
    assert nodes[2]._Node__arguments == [
        "joint_state_broadcaster", "--controller-manager", "/fixture/arm/control",
    ]
    assert nodes[3]._Node__arguments == [
        gripper_name, "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    parameters = evaluate_parameters(context, nodes[0]._Node__parameters)[0]
    assert parameters["robot_description"] == description_path.read_text()
    assert parameters["frame_prefix"] == "tool/"
    assert parameters["use_sim_time"] is False
    assert evaluate_parameters(context, nodes[1]._Node__parameters)[0] == (
        tmp_path / "controllers.yaml"
    )


def test_arm_with_no_gripper_output_keeps_structural_fingers_and_arm_spawner(tmp_path):
    module = _module()
    context = _context(tmp_path)
    description = tmp_path / "retained.urdf"
    payload = description.read_text().replace(
        '</robot>',
        '<link name="finger"/><joint name="right_finger_joint" type="prismatic">'
        '<parent link="base"/><child link="finger"/>'
        '<limit lower="0" upper="0.04" effort="5" velocity="0.1"/>'
        '</joint></robot>',
    )
    description.write_text(payload, encoding="utf-8")
    nodes = module.launch_setup(context)
    assert len(nodes) == 4
    assert nodes[3]._Node__arguments == [
        "arm_controller", "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    retained = evaluate_parameters(context, nodes[0]._Node__parameters)[0]["robot_description"]
    assert retained == payload
    assert 'joint name="right_finger_joint" type="prismatic"' in retained
    assert all(node._Node__arguments[0] in ["joint_state_broadcaster", "arm_controller"]
               for node in nodes if node._Node__node_executable == "spawner")


def test_missing_retained_artifact_has_no_vendor_fallback(tmp_path):
    context = _context(tmp_path)
    context.launch_configurations["robot_description_file"] = str(tmp_path / "missing")
    with pytest.raises(ValueError, match="retained absolute file"):
        _module().launch_setup(context)


def test_stock_gripper_plugin_dependency_is_declared():
    import xml.etree.ElementTree as ET

    manifest = ET.fromstring((ROOT / "package.xml").read_text())
    assert "gripper_controllers" in {
        dependency.text for dependency in manifest.findall("exec_depend")
    }
    assert "forward_command_controller" in {
        dependency.text for dependency in manifest.findall("exec_depend")
    }


def test_owned_nodes_disable_external_library_log_files():
    import ast

    source = Path(_module().__file__).read_text(encoding="utf-8")
    nodes = [
        node for node in ast.walk(ast.parse(source))
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
        and node.func.id == "Node"
    ]
    assert nodes
    for node in nodes:
        arguments = next(keyword.value for keyword in node.keywords if keyword.arg == "ros_arguments")
        assert ast.literal_eval(arguments.elts[0]) == "--disable-external-lib-logs"


# CP8 step 2a: the same launch runs a Real component; its hardware data comes
# from the generated artifacts.
REAL_PLUGIN = "uf_robot_hardware/UFRobotSystemHardware"
REAL_REMAPS = (
    "/controller_manager/list_controllers=/fixture/arm/control/list_controllers;"
    "/controller_manager/switch_controller=/fixture/arm/control/switch_controller;"
    "/rviz/moveit/update_goal_state=/fixture/arm/update_goal_state"
)


def _real_context(tmp_path, **hardware):
    driver = tmp_path / "driver_parameters.yaml"
    driver.write_text("/fixture/arm/ufactory_driver:\n  ros__parameters: {}\n", encoding="utf-8")
    values = {
        "hardware_plugin": REAL_PLUGIN,
        "driver_parameters_file": str(driver),
        "hardware_remappings": REAL_REMAPS,
    }
    values.update(hardware)
    return _context(tmp_path, plugin=REAL_PLUGIN, hardware=values)


def test_real_hardware_data_reaches_ros2_control_node(tmp_path):
    context = _real_context(tmp_path)
    nodes = _module().launch_setup(context)
    assert [node._Node__node_executable for node in nodes] == [
        "robot_state_publisher", "ros2_control_node", "spawner", "spawner",
    ]
    parameters = evaluate_parameters(context, nodes[1]._Node__parameters)
    assert parameters[0] == tmp_path / "controllers.yaml"
    assert parameters[1] == tmp_path / "driver_parameters.yaml"
    assert parameters[2] == {"use_sim_time": False}
    assert _remaps(context, nodes[1]) == [
        ("~/robot_description", "robot_description"),
        ("/controller_manager/list_controllers", "/fixture/arm/control/list_controllers"),
        ("/controller_manager/switch_controller", "/fixture/arm/control/switch_controller"),
        ("/rviz/moveit/update_goal_state", "/fixture/arm/update_goal_state"),
    ]
    # The same spawner mechanism loads Real's arm controller inactive with the
    # generated controller parameters.
    assert nodes[3]._Node__arguments == [
        "arm_controller", "--inactive", "--controller-manager", "/fixture/arm/control",
        "--param-file", str(tmp_path / "controllers.yaml"),
    ]
    assert _remaps(context, nodes[0]) == []


@pytest.mark.parametrize(
    ("description_plugin", "expected_plugin"),
    [
        ("mock_components/GenericSystem", REAL_PLUGIN),
        (REAL_PLUGIN, "mock_components/GenericSystem"),
        ("topic_based_ros2_control/TopicBasedSystem", REAL_PLUGIN),
    ],
)
def test_a_description_plugin_other_than_the_explicit_one_is_refused(
    tmp_path, description_plugin, expected_plugin,
):
    context = _context(
        tmp_path, plugin=description_plugin, hardware={"hardware_plugin": expected_plugin},
    )
    with pytest.raises(ValueError, match="must use only " + expected_plugin):
        _module().launch_setup(context)


@pytest.mark.parametrize(
    "remaps",
    [
        "/a",
        "a=/b",
        "/a=b",
        "/a=/b=/c",
        "/a=/b;/a=/c",
    ],
)
def test_malformed_hardware_remappings_are_refused(tmp_path, remaps):
    with pytest.raises(ValueError, match="hardware_remappings"):
        _module().launch_setup(_real_context(tmp_path, hardware_remappings=remaps))


def test_missing_driver_parameters_file_is_refused(tmp_path):
    context = _real_context(tmp_path, driver_parameters_file=str(tmp_path / "missing.yaml"))
    with pytest.raises(ValueError, match="driver_parameters_file"):
        _module().launch_setup(context)
