"""Launch ros2_control/JTC over Isaac Sim's JointState command topics.

This is the standard ManyForge Isaac execution path:

ManyForge -> FollowJointTrajectory -> joint_trajectory_controller ->
topic_based_ros2_control -> /isaac_joint_commands and /isaac_joint_states.

The launch deliberately exposes the arm through JTC. Position-commanded
GripperCommand profiles may start the Isaac action bridge; Lite6 pneumatic
profiles should use ManyForge's C++ isaac_joint_state_effort transport and do
not require a gripper action server.
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
                "isaac_profile",
                default_value="ur10e",
                description=(
                    "Isaac topic-based robot profile to expose through "
                    "ros2_control. Supported: ur10e, lite6."
                ),
            ),
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
                "controller_manager_lock_memory",
                default_value="false",
                description=(
                    "Lock controller_manager memory for a strict target resource "
                    "profile. Requires a matching memlock limit."
                ),
            ),
            DeclareLaunchArgument(
                "controller_manager_thread_priority",
                default_value="50",
                description=(
                    "SCHED_FIFO priority requested by the controller_manager "
                    "update thread. The target profile keeps this above every "
                    "ManyForge native scheduler role."
                ),
            ),
            DeclareLaunchArgument(
                "controller_manager_cpu_affinity",
                default_value="",
                description=(
                    "Comma/range CPU set reserved for controller_manager, for "
                    "example '13' or '12-13'. Empty preserves inherited affinity."
                ),
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
                description=(
                    "ManyForge Bool topic that enables Isaac arm command "
                    "publishing. Written ONLY by the execution backend (the "
                    "single backend gate authority)."
                ),
            ),
            DeclareLaunchArgument(
                "supervisor_command_gate_enable_topic",
                default_value="/manyforge/isaac_supervisor_command_enable",
                description=(
                    "Supervisor veto Bool topic, written ONLY by the Isaac "
                    "demo supervisor. The command gate node requires this AND "
                    "the backend enable topic to be true before commands flow."
                ),
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
                "joint1_initial",
                default_value="0.0",
                description="Initial Lite6 joint1 position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "joint2_initial",
                default_value="0.0",
                description="Initial Lite6 joint2 position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "joint3_initial",
                default_value="1.57",
                description="Initial Lite6 joint3 position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "joint4_initial",
                default_value="0.0",
                description="Initial Lite6 joint4 position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "joint5_initial",
                default_value="0.0",
                description="Initial Lite6 joint5 position for commanded Isaac sim.",
            ),
            DeclareLaunchArgument(
                "joint6_initial",
                default_value="0.0",
                description="Initial Lite6 joint6 position for commanded Isaac sim.",
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
                "gripper_readiness_topic",
                default_value="/manyforge/isaac_gripper_bridge/ready",
                description="Fail-closed gripper topology/readiness signal.",
            ),
            DeclareLaunchArgument(
                "start_gripper_action_bridge",
                default_value="auto",
                description=(
                    "Start the Python Isaac GripperCommand bridge. Values: "
                    "auto, true, false. auto starts it for action-based UR/Robotiq "
                    "profiles and skips it for Lite6 direct-effort profiles."
                ),
            ),
            DeclareLaunchArgument(
                "base_parent_frame",
                default_value="universe",
                description="Parent frame for the robot root static transform.",
            ),
            DeclareLaunchArgument(
                "base_child_frame",
                default_value="world",
                description=(
                    "Child frame for the static transform. The Lite6 URDF already "
                    "publishes world -> link_base, so the default bridges "
                    "ManyForge's universe frame to the URDF world link."
                ),
            ),
            DeclareLaunchArgument(
                "base_xyz",
                default_value="0 0 0",
                description="Static base transform translation: 'x y z' in meters.",
            ),
            DeclareLaunchArgument(
                "base_rpy",
                default_value="0 0 0",
                description="Static base transform rotation: 'roll pitch yaw' in radians.",
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
            DeclareLaunchArgument(
                "pose_source_sidecar",
                default_value="true",
                description=(
                    "When true, poll configured simulation_interfaces pose sources "
                    "and publish observe-only snapshots to Composer."
                ),
            ),
            DeclareLaunchArgument(
                "pose_source_service_timeout_s",
                default_value="2.0",
                description="Timeout for simulation_interfaces pose-source service calls.",
            ),
            DeclareLaunchArgument(
                "pose_source_publish_timeout_s",
                default_value="1.0",
                description="Timeout for publishing pose-source snapshots to Composer.",
            ),
            OpaqueFunction(function=_launch_setup),
        ]
    )


def _launch_setup(context, *_, **__) -> list[object]:
    package_share = Path(
        FindPackageShare("manyforge_ros_bringup").find("manyforge_ros_bringup")
    )
    isaac_profile = LaunchConfiguration("isaac_profile").perform(context).strip()
    if isaac_profile == "ur10e":
        urdf_file = "isaac_ur10e_topic_based.ros2_control.urdf"
        controller_config_file = "isaac_ur10e_topic_based_controllers.yaml"
        gripper_bridge_config_file = "isaac_robotiq_2f_140_gripper_bridge.yaml"
        profile_default_gripper_action = "/robotiq_gripper_controller/gripper_cmd"
        profile_default_gripper_command_topic = "/isaac_gripper_state"
    elif isaac_profile == "lite6":
        urdf_file = "isaac_lite6_topic_based.ros2_control.urdf"
        controller_config_file = "isaac_lite6_topic_based_controllers.yaml"
        gripper_bridge_config_file = "isaac_lite6_gripper_bridge.yaml"
        profile_default_gripper_action = "/lite_gripper_controller/gripper_cmd"
        profile_default_gripper_command_topic = "/isaac_joint_commands_gripper"
    else:
        raise ValueError(
            f"unsupported isaac_profile={isaac_profile!r}; expected ur10e or lite6"
        )

    urdf_template = (package_share / "urdf" / urdf_file).read_text(encoding="utf-8")
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
        joint1_initial=LaunchConfiguration("joint1_initial").perform(context),
        joint2_initial=LaunchConfiguration("joint2_initial").perform(context),
        joint3_initial=LaunchConfiguration("joint3_initial").perform(context),
        joint4_initial=LaunchConfiguration("joint4_initial").perform(context),
        joint5_initial=LaunchConfiguration("joint5_initial").perform(context),
        joint6_initial=LaunchConfiguration("joint6_initial").perform(context),
    )
    controller_config = package_share / "config" / controller_config_file
    gripper_bridge_config = package_share / "config" / gripper_bridge_config_file
    controller_manager_name = LaunchConfiguration("controller_manager_name").perform(
        context
    )
    arm_controller_name = LaunchConfiguration("arm_controller_name").perform(context)
    controller_manager_path = f"/{controller_manager_name}"
    gripper_action_name = LaunchConfiguration("gripper_action_name").perform(context)
    if gripper_action_name == "/robotiq_gripper_controller/gripper_cmd":
        gripper_action_name = profile_default_gripper_action
    gripper_command_topic = LaunchConfiguration("gripper_command_topic").perform(context)
    if gripper_command_topic == "/isaac_gripper_state":
        gripper_command_topic = profile_default_gripper_command_topic
    start_bridge = _resolve_start_gripper_action_bridge(
        LaunchConfiguration("start_gripper_action_bridge").perform(context),
        isaac_profile=isaac_profile,
    )
    controller_manager_lock_memory = _parse_bool_launch_value(
        LaunchConfiguration("controller_manager_lock_memory").perform(context),
        name="controller_manager_lock_memory",
    )
    controller_manager_thread_priority = _parse_bounded_int_launch_value(
        LaunchConfiguration("controller_manager_thread_priority").perform(context),
        name="controller_manager_thread_priority",
        minimum=1,
        maximum=99,
    )
    controller_manager_cpu_affinity = _parse_cpu_affinity_launch_value(
        LaunchConfiguration("controller_manager_cpu_affinity").perform(context),
        name="controller_manager_cpu_affinity",
    )
    controller_manager_resources: dict[str, object] = {
        "lock_memory": controller_manager_lock_memory,
        "thread_priority": controller_manager_thread_priority,
    }
    if controller_manager_cpu_affinity:
        controller_manager_resources["cpu_affinity"] = (
            controller_manager_cpu_affinity
        )

    ros2_control_node = Node(
        package="controller_manager",
        executable="ros2_control_node",
        name=controller_manager_name,
        output="screen",
        parameters=[
            {"robot_description": robot_description},
            controller_manager_resources,
            str(controller_config),
        ],
    )
    robot_state_publisher = Node(
        package="robot_state_publisher",
        executable="robot_state_publisher",
        output="screen",
        parameters=[{"robot_description": robot_description}],
    )
    base_xyz = LaunchConfiguration("base_xyz").perform(context).split()
    base_rpy = LaunchConfiguration("base_rpy").perform(context).split()
    if len(base_xyz) != 3:
        raise ValueError("base_xyz must contain exactly three numbers: 'x y z'")
    if len(base_rpy) != 3:
        raise ValueError("base_rpy must contain exactly three numbers: 'roll pitch yaw'")
    static_base_tf = Node(
        package="tf2_ros",
        executable="static_transform_publisher",
        name="manyforge_static_base_tf",
        output="screen",
        arguments=[
            "--x",
            base_xyz[0],
            "--y",
            base_xyz[1],
            "--z",
            base_xyz[2],
            "--roll",
            base_rpy[0],
            "--pitch",
            base_rpy[1],
            "--yaw",
            base_rpy[2],
            "--frame-id",
            LaunchConfiguration("base_parent_frame"),
            "--child-frame-id",
            LaunchConfiguration("base_child_frame"),
        ],
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
                "action_name": gripper_action_name,
                "command_topic": gripper_command_topic,
                "state_topic": LaunchConfiguration("gripper_state_topic").perform(
                    context
                ),
                "readiness_topic": LaunchConfiguration(
                    "gripper_readiness_topic"
                ).perform(context),
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
                "supervisor_enable_topic": LaunchConfiguration(
                    "supervisor_command_gate_enable_topic"
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
                "gripper_action_name": gripper_action_name,
                "gripper_readiness_topic": LaunchConfiguration(
                    "gripper_readiness_topic"
                ).perform(context),
                "require_gripper_action": start_bridge,
                "supervisor_enable_topic": LaunchConfiguration(
                    "supervisor_command_gate_enable_topic"
                ).perform(context),
                "auto_load": LaunchConfiguration("supervisor_auto_load").perform(
                    context
                ).lower()
                == "true",
            }
        ],
    )
    pose_source_snapshot_publisher = Node(
        package="manyforge_ros_bringup",
        executable="pose_source_snapshot_publisher",
        name="manyforge_pose_source_snapshot_publisher",
        output="screen",
        parameters=[
            {
                "composer_url": LaunchConfiguration("composer_url").perform(context),
                "deployment_path": LaunchConfiguration("deployment_path").perform(
                    context
                ),
                "service_timeout_s": float(
                    LaunchConfiguration("pose_source_service_timeout_s").perform(
                        context
                    )
                ),
                "publish_timeout_s": float(
                    LaunchConfiguration("pose_source_publish_timeout_s").perform(
                        context
                    )
                ),
            }
        ],
    )

    # Give controller_manager a short deterministic startup window before
    # spawners call its services. This keeps the launch file simple and robust
    # in containers where DDS discovery can be a little slow.
    actions = [
        topic_command_gate,
        demo_supervisor,
        robot_state_publisher,
        static_base_tf,
        ros2_control_node,
        TimerAction(period=2.0, actions=[joint_state_broadcaster]),
        TimerAction(period=3.0, actions=[arm_controller]),
    ]
    if start_bridge:
        actions.insert(5, gripper_action_bridge)
    if (
        LaunchConfiguration("pose_source_sidecar").perform(context).strip().lower()
        == "true"
    ):
        actions.insert(2, pose_source_snapshot_publisher)
    return actions


def _resolve_start_gripper_action_bridge(value: str, *, isaac_profile: str) -> bool:
    normalized = value.strip().lower()
    if normalized == "auto":
        return isaac_profile != "lite6"
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(
        "start_gripper_action_bridge must be auto, true, or false; "
        f"got {value!r}"
    )


def _parse_bool_launch_value(value: str, *, name: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be true or false; got {value!r}")


def _parse_bounded_int_launch_value(
    value: str,
    *,
    name: str,
    minimum: int,
    maximum: int,
) -> int:
    try:
        parsed = int(value.strip())
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer") from exc
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be in [{minimum}, {maximum}]")
    return parsed


def _parse_cpu_affinity_launch_value(value: str, *, name: str) -> list[int]:
    # controller_manager and the native scheduler ultimately use Linux
    # cpu_set_t. Bound expansion before constructing a Python range so a
    # malformed deployment value cannot consume startup memory or CPU.
    maximum_cpu_index = 1023
    normalized = value.strip()
    if not normalized:
        return []
    cpus: set[int] = set()
    for raw_token in normalized.split(","):
        token = raw_token.strip()
        if not token:
            raise ValueError(f"{name} contains an empty CPU token")
        if "-" not in token:
            cpus.add(
                _parse_bounded_int_launch_value(
                    token,
                    name=name,
                    minimum=0,
                    maximum=maximum_cpu_index,
                )
            )
            continue
        pieces = token.split("-")
        if len(pieces) != 2:
            raise ValueError(f"{name} contains an invalid CPU range: {token!r}")
        first = _parse_bounded_int_launch_value(
            pieces[0],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        last = _parse_bounded_int_launch_value(
            pieces[1],
            name=name,
            minimum=0,
            maximum=maximum_cpu_index,
        )
        if last < first:
            raise ValueError(f"{name} contains a descending CPU range: {token!r}")
        cpus.update(range(first, last + 1))
    return sorted(cpus)
