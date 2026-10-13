from __future__ import annotations

import importlib.util
from pathlib import Path

from launch import LaunchContext
from launch.actions import DeclareLaunchArgument
from launch_ros.actions import Node
from launch_ros.utilities import evaluate_parameters
import pytest
import yaml


ROOT = Path(__file__).resolve().parents[1]


def _module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_mock_control_launch", ROOT / "launch/mock_control.launch.py"
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _context(
    tmp_path, *, plugin="mock_components/GenericSystem", gripper_name="", frame_prefix=""
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
        gripper_controller_name=gripper_name,
        frame_prefix=frame_prefix,
    )
    return context


def test_mock_launch_requires_retained_artifacts_and_no_second_pose_input():
    module = _module()
    declarations = {
        action.name for action in module.generate_launch_description().entities
        if isinstance(action, DeclareLaunchArgument)
    }
    assert declarations == {
        "robot_description_file", "controllers_file", "runtime_namespace",
        "controller_manager_name", "arm_controller_name",
        "gripper_controller_name",
        "frame_prefix",
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
    # service call here: actual CCLC/Manager integration belongs to its owner.
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


@pytest.mark.parametrize("prefix", ["", "authored_component/"])
def test_explicit_prefix_only_changes_mock_rsp_frames(tmp_path, prefix):
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


@pytest.mark.parametrize(
    "plugin",
    ["uf_robot_hardware/UFRobotSystemHardware", "topic_based_ros2_control/TopicBasedSystem"],
)
def test_mock_entrypoint_cannot_select_effectful_hardware(tmp_path, plugin):
    with pytest.raises(ValueError, match="GenericSystem"):
        _module().launch_setup(_context(tmp_path, plugin=plugin))


def test_missing_retained_artifact_has_no_vendor_fallback(tmp_path):
    context = _context(tmp_path)
    context.launch_configurations["robot_description_file"] = str(tmp_path / "missing")
    with pytest.raises(ValueError, match="retained absolute file"):
        _module().launch_setup(context)


def test_shared_template_has_no_cancel_bound_or_xarm7_joint7_tolerance_default():
    payload = yaml.safe_load((ROOT / "config/mock_ufactory_controllers.template.yaml").read_text())
    manager = payload["/**/controller_manager"]["ros__parameters"]
    controller = payload["/**/__arm_controller__"]["ros__parameters"]
    assert manager["update_rate"] == 150
    assert controller["command_interfaces"] == ["position"]
    assert controller["state_interfaces"] == ["position", "velocity"]
    assert controller["state_publish_rate"] == 25.0
    assert controller["action_monitor_rate"] == 20.0
    assert controller["interpolate_from_desired_state"] is True
    assert "set_last_command_interface_value_as_state_on_activation" not in controller
    # The pinned xArm7 vendor map omits joint7 tolerances. Replicating a joint1
    # block here would introduce a new path check for joint7 during generation.
    assert controller["constraints"]["__joint__"] == {
        "max_deceleration_on_cancel": None
    }


def test_ur_template_retains_ur_rates_and_tolerances_without_isaac_activation_override():
    payload = yaml.safe_load((ROOT / "config/mock_ur_controllers.template.yaml").read_text())
    manager = payload["/**/controller_manager"]["ros__parameters"]
    controller = payload["/**/__arm_controller__"]["ros__parameters"]
    assert manager["update_rate"] == 100
    assert controller["command_interfaces"] == ["position"]
    assert controller["state_interfaces"] == ["position", "velocity"]
    assert controller["state_publish_rate"] == 100.0
    assert controller["action_monitor_rate"] == 20.0
    assert controller["allow_partial_joints_goal"] is False
    assert controller["interpolate_from_desired_state"] is True
    assert "set_last_command_interface_value_as_state_on_activation" not in controller
    assert controller["constraints"] == {
        "goal_time": 0.0,
        "stopped_velocity_tolerance": 0.2,
        "decelerate_on_cancel": True,
        "__joint__": {
            "trajectory": 0.2,
            "goal": 0.1,
            "max_deceleration_on_cancel": None,
        },
    }


def test_optional_gripper_template_uses_stock_defaults_and_one_retained_joint():
    payload = yaml.safe_load(
        (ROOT / "config/mock_gripper_controller.template.yaml").read_text()
    )
    manager = payload["/**/controller_manager"]["ros__parameters"]
    assert manager == {
        "__gripper_controller__": {
            "type": "position_controllers/GripperActionController"
        }
    }
    assert payload["/**/__gripper_controller__"]["ros__parameters"] == {
        "joint": "__gripper_joint__",
        "goal_tolerance": 0.01,
        "stall_velocity_threshold": 0.001,
        "stall_timeout": 1.0,
        "allow_stalling": False,
    }


def test_stock_gripper_plugin_dependency_is_declared():
    import xml.etree.ElementTree as ET

    manifest = ET.fromstring((ROOT / "package.xml").read_text())
    assert "gripper_controllers" in {
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
        assert ast.literal_eval(arguments) == ["--disable-external-lib-logs"]
