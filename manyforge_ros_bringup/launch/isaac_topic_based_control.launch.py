"""Launch ros2_control/JTC over Isaac Sim's JointState command topics.

This is the standard ManyForge Isaac execution path:

ManyForge -> FollowJointTrajectory -> joint_trajectory_controller ->
topic_based_ros2_control -> /isaac_joint_commands and /isaac_joint_states.

The launch deliberately exposes the UR arm through JTC and the Robotiq gripper
through a separate standard GripperCommand action. The Isaac-specific bridge is
the only node that publishes the scene's raw /isaac_gripper_state command topic.
"""

from __future__ import annotations

from pathlib import Path

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction, TimerAction
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description() -> LaunchDescription:
    return LaunchDescription(
        [
            DeclareLaunchArgument(
                "joint_commands_topic",
                default_value="/isaac_joint_commands",
                description="Isaac Sim JointState command topic for arm joints.",
            ),
            DeclareLaunchArgument(
                "joint_states_topic",
                default_value="/isaac_joint_states",
                description="Isaac Sim JointState feedback topic.",
            ),
            DeclareLaunchArgument(
                "controller_manager_name",
                default_value="controller_manager",
                description="Name of the ros2_control controller_manager node.",
            ),
            DeclareLaunchArgument(
                "arm_controller_name",
                default_value="isaac_arm_controller",
                description="JointTrajectoryController to spawn for the UR arm.",
            ),
            DeclareLaunchArgument(
                "trigger_joint_command_threshold",
                default_value="-1.0",
                description=(
                    "topic_based_ros2_control command threshold. Isaac commanded-sim "
                    "uses -1.0 so the bridge continuously republishes the commanded "
                    "hold while the fail-closed command gate is enabled."
                ),
            ),
            DeclareLaunchArgument(
                "command_gate_enable_topic",
                default_value="/manyforge/isaac_arm_command_enable",
                description="ManyForge Bool topic that enables Isaac arm command publishing.",
            ),
            DeclareLaunchArgument(
                "stop_joint_commands_topic",
                default_value="/stop_joint_commands",
                description="topic_based_ros2_control Bool topic that suppresses joint commands.",
            ),
            DeclareLaunchArgument(
                "command_gate_publish_rate_hz",
                default_value="20.0",
                description="Rate for republishing the fail-closed stop-joint-commands signal.",
            ),
            DeclareLaunchArgument(
                "sum_wrapped_joint_states",
                default_value="false",
                description="Pass-through topic_based_ros2_control wrap handling.",
            ),
            DeclareLaunchArgument(
                "shoulder_pan_initial",
                default_value="2.63422544",
                description="Initial shoulder_pan_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "shoulder_lift_initial",
                default_value="-0.875980752",
                description="Initial shoulder_lift_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "elbow_initial",
                default_value="1.551074106",
                description="Initial elbow_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "wrist_1_initial",
                default_value="-2.29231544",
                description="Initial wrist_1_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "wrist_2_initial",
                default_value="-1.560498884",
                description="Initial wrist_2_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "wrist_3_initial",
                default_value="-2.038544566",
                description="Initial wrist_3_joint position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "gripper_action_name",
                default_value="/robotiq_gripper_controller/gripper_cmd",
                description="Standard GripperCommand action exposed for the Robotiq gripper.",
            ),
            DeclareLaunchArgument(
                "gripper_command_topic",
                default_value="/isaac_gripper_state",
                description="Isaac Sim raw JointState command topic for the gripper.",
            ),
            DeclareLaunchArgument(
                "gripper_state_topic",
                default_value="/isaac_joint_states",
                description="Isaac Sim JointState feedback topic used by the gripper bridge.",
            ),
            DeclareLaunchArgument(
                "supervisor_auto_load",
                default_value="true",
                description=(
                    "When true, the Isaac demo supervisor loads Composer artifacts once "
                    "Composer is healthy and defers the runtime reset until Isaac/controller "
                    "readiness is observed."
                ),
            ),
            DeclareLaunchArgument(
                "composer_url",
                default_value="http://127.0.0.1:9000",
                description="Composer HTTP base URL used by the Isaac demo supervisor.",
            ),
            DeclareLaunchArgument(
                "program_path",
                default_value="/workspace/examples/isaac_manipulator_scene_light.program.yaml",
                description="Program artifact loaded by the Isaac demo supervisor.",
            ),
            DeclareLaunchArgument(
                "deployment_path",
                default_value="/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
                description="Deployment artifact loaded by the Isaac demo supervisor.",
            ),
            OpaqueFunction(function=_launch_setup),
        ]
    )


def _launch_setup(context, *_, **__) -> list[object]:
    package_share = Path(
        FindPackageShare("manyforge_ros_bringup").find("manyforge_ros_bringup")
    )
    urdf_template = (
        package_share / "urdf" / "isaac_ur10e_topic_based.ros2_control.urdf"
    ).read_text(encoding="utf-8")
    robot_description = urdf_template.format(
        joint_commands_topic=LaunchConfiguration("joint_commands_topic").perform(
            context
        ),
        joint_states_topic=LaunchConfiguration("joint_states_topic").perform(context),
        trigger_joint_command_threshold=LaunchConfiguration(
            "trigger_joint_command_threshold"
        ).perform(context),
        sum_wrapped_joint_states=LaunchConfiguration("sum_wrapped_joint_states").perform(
            context
        ),
        shoulder_pan_initial=LaunchConfiguration("shoulder_pan_initial").perform(
            context
        ),
        shoulder_lift_initial=LaunchConfiguration("shoulder_lift_initial").perform(
            context
        ),
        elbow_initial=LaunchConfiguration("elbow_initial").perform(context),
        wrist_1_initial=LaunchConfiguration("wrist_1_initial").perform(context),
        wrist_2_initial=LaunchConfiguration("wrist_2_initial").perform(context),
        wrist_3_initial=LaunchConfiguration("wrist_3_initial").perform(context),
    )
    controller_config = (
        package_share / "config" / "isaac_ur10e_topic_based_controllers.yaml"
    )
    gripper_bridge_config = (
        package_share / "config" / "isaac_robotiq_2f_140_gripper_bridge.yaml"
    )
    controller_manager_name = LaunchConfiguration("controller_manager_name").perform(
        context
    )
    arm_controller_name = LaunchConfiguration("arm_controller_name").perform(context)
    controller_manager_path = f"/{controller_manager_name}"

    ros2_control_node = Node(
        package="controller_manager",
        executable="ros2_control_node",
        name=controller_manager_name,
        output="screen",
        parameters=[
            {"robot_description": robot_description},
            str(controller_config),
        ],
    )
    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        output="screen",
        parameters=[{"robot_description": robot_description}],
    )

    joint_state_broadcaster = Node(
        package="controller_manager",
        executable="spawner",
        output="screen",
        arguments=[
            "joint_state_broadcaster",
            "--controller-manager",
            controller_manager_path,
        ],
    )
    arm_controller = Node(
        package="controller_manager",
        executable="spawner",
        output="screen",
        arguments=[
            arm_controller_name,
            "--controller-manager",
            controller_manager_path,
        ],
    )
    gripper_action_bridge = Node(
        package="manyforge_ros_bringup",
        executable="isaac_gripper_action_bridge",
        name="manyforge_isaac_gripper_action_bridge",
        output="screen",
        parameters=[
            str(gripper_bridge_config),
            {
                "action_name": LaunchConfiguration("gripper_action_name").perform(
                    context
                ),
                "command_topic": LaunchConfiguration("gripper_command_topic").perform(
                    context
                ),
                "state_topic": LaunchConfiguration("gripper_state_topic").perform(
                    context
                ),
            }
        ],
    )
    topic_command_gate = Node(
        package="manyforge_ros_bringup",
        executable="isaac_topic_command_gate",
        name="manyforge_isaac_topic_command_gate",
        output="screen",
        parameters=[
            {
                "enable_topic": LaunchConfiguration(
                    "command_gate_enable_topic"
                ).perform(context),
                "stop_topic": LaunchConfiguration(
                    "stop_joint_commands_topic"
                ).perform(context),
                "publish_rate_hz": float(
                    LaunchConfiguration("command_gate_publish_rate_hz").perform(
                        context
                    )
                ),
                "default_enabled": False,
            }
        ],
    )
    demo_supervisor = Node(
        package="manyforge_ros_bringup",
        executable="isaac_demo_supervisor",
        name="manyforge_isaac_demo_supervisor",
        output="screen",
        parameters=[
            {
                "composer_url": LaunchConfiguration("composer_url").perform(context),
                "program_path": LaunchConfiguration("program_path").perform(context),
                "deployment_path": LaunchConfiguration("deployment_path").perform(
                    context
                ),
                "isaac_joint_state_topic": LaunchConfiguration(
                    "joint_states_topic"
                ).perform(context),
                "controller_state_topic": f"/{arm_controller_name}/controller_state",
                "arm_action_name": f"/{arm_controller_name}/follow_joint_trajectory",
                "gripper_action_name": LaunchConfiguration(
                    "gripper_action_name"
                ).perform(context),
                "command_gate_enable_topic": LaunchConfiguration(
                    "command_gate_enable_topic"
                ).perform(context),
                "auto_load": LaunchConfiguration("supervisor_auto_load").perform(
                    context
                ).lower()
                == "true",
            }
        ],
    )

    # Give controller_manager a short deterministic startup window before
    # spawners call its services. This keeps the launch file simple and robust
    # in containers where DDS discovery can be a little slow.
    return [
        topic_command_gate,
        demo_supervisor,
        robot_state_publisher,
        ros2_control_node,
        gripper_action_bridge,
        TimerAction(period=2.0, actions=[joint_state_broadcaster]),
        TimerAction(period=3.0, actions=[arm_controller]),
    ]
