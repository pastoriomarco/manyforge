# ManyForge ros2_control execution

`manyforge_ros2_control_execution` is the ROS 2 transport the execution host
(`manyforge_roboplan`) uses to drive a component's controllers on every lane
(Mock, Isaac, Real). All three run stock ros2_control with the stock
JointTrajectoryController and GripperActionController; the lanes differ only in
the hardware behind the controllers. The package keeps ROS types out of
`manyforge_core`: it implements the kernel's ports and reports observed facts;
it owns no planning, Stop policy, lifecycle action or recovery.

## Contents

| Part | What it does |
| --- | --- |
| `Ros2ControlExecutionBackend` | The kernel's execution port for one arm controller binding: sends, replaces and cancels `FollowJointTrajectory` goals, sets the speed scale, reads goal progress, controller state samples and readiness, and the controller's own goal activity (for the component's `activity`). |
| `Ros2GripperCommandBackend` | Gripper commands (`control_msgs/GripperCommand` action of the stock GripperActionController) and their progress; Stop policy stays with the kernel. |
| `controller_binding` | The exact controller, action, topic and joint names compiled from the generated ros2_control artifacts; the backend never derives vendor prefixes, namespaces or controller names. |
| `ros2_control_capability` | Backend configuration and the pure capability evaluation for ros2_control components (no ROS runtime dependency). |
| `Ros2TransportResources` | The ROS node and executor shared by the arm and gripper backends of the host. |
| `SelectedRosEndpointFacts` | Counts the publishers of each lane-owned, exclusive ROS topic: an action needs exactly one (0 not established, 2+ `duplicate_component_instance`). |
| `VolatileMotionTrace` | A diagnostic in-memory trace of motion commands and feedback, capped at 3 MiB. |
| Scene projection worker | Pushes observed arm and gripper states to the Scene off the controller path. |
| `lane/` | The Mock lane folder (`driver.yaml`, `manyforge_settings.yaml`, [`README.md`](lane/README.md)): stock `mock_components/GenericSystem` hardware. |

How a component's controllers are generated and launched (`robot_description`,
controllers YAML, `ros2_control.launch.py`) is the launch contribution of its
lane or vendor adapter; see [`lane/README.md`](lane/README.md) for Mock,
`manyforge_isaac_adapter/lane/README.md` for Isaac and
`manyforge_xarm_control/vendor/README.md` for the xArm.

## Behaviour

- A goal is sent only while the controller and its hardware are reported
  active for the current binding; otherwise it is refused before it is sent.
- The backend reports what the controller reports (goal accepted, progress,
  finished, cancelled); nothing resends a goal.
- Controller-manager queries and other non-real-time work run outside the
  controller update loop.

## Tests

`test/` holds the package tests (`test_ros2_control_execution_backend`,
`test_ros2_gripper_command_backend`, `test_ros2_capability`,
`test_volatile_motion_trace`, `test_ros2_control_execution_package`); they
run in the ROS container with the rest of the colcon tests.
