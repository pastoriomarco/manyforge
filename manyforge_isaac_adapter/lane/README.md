# Isaac lane adapter

Runs ManyForge components in NVIDIA Isaac Sim. Isaac Sim itself runs outside
ManyForge, with a scene that publishes the robots' joint states and takes their
joint commands on ROS topics.

## What runs where

- **The simulator** is a component of its own (model `nvidia.isaac_sim`,
  folder `manyforge_example_models/models/isaac_sim`). It runs on the generic
  simulator adapter (`simulation_interfaces`, the ROS 2 standard), so the core
  and the Composer see the same interface for every simulator:
  - `playing` shows as enabled;
  - `paused` or `stopped` shows as prepared, with the reason
    (`simulation_paused`, `simulation_stopped`), never a fault;
  - unreachable shows as offline (`simulator_unreachable`);
  - Enable plays and Disable pauses (there are no separate play/pause
    actions); Prepare and Recover read the state; the capability `sim_reset`
    resets the simulation.
  - `sim_reset` identifies a simulator: its own state never latches the cell
    or blocks Execute. An unreachable or faulted simulator stays visibly
    Offline or Fault with a warning; robot and tool health follows their own
    reported states. An unanswered Play, Pause or Reset stays as that action's
    unknown result and warning; it is never resent.
  - The service names and timing are in the model's `lanes.isaac` row.
- **Robots** run the ros2_control lane, like Mock and Real. This lane
  contributes the hardware section (`topic_based_ros2_control/TopicBasedSystem`),
  the controller manager's process settings (no memory locking, thread
  priority 50) and the static transform `universe` → `<prefix>world`. A model
  of a vendor with an adapter (xArm, row key `vendor`) runs that vendor's
  controllers, as on the real lane; any other model (UR) runs this lane's
  template (`controllers.template.yaml`). The generic generator writes the run
  files, seeding the hardware with the Deployment's initial joints (the robot
  starts there), and `ros2_control.launch.py` runs them.
  A UFactory arm is placed as the arm member of its machine (e.g.
  `ufactory.lite6_machine`) here too; the machine model has no Isaac row, so
  its owner is the host's passive owner and its I/O members are Mock I/O.
- **Reset position** (`reset_position`): an Isaac robot declares it,
  with the Mock lane's stock writer (a `ForwardCommandController` loaded
  inactive next to the arm's JTC). The sequence: JTC off and the writer on;
  the Deployment's initial joints published once to the writer, which the
  topic-based hardware sends to Isaac on `/isaac_joint_commands`; one sample
  there carrying exactly those joints acknowledges the write; writer off and
  JTC on, holding the initial joints. Each step has the stock 1 s switch
  bound, as on Mock; ManyForge does not wait for Isaac to arrive. While the
  simulation isn't playing the write can't take effect: the action and the
  Program Reset option **Reset robot position** are refused with
  `simulation_not_playing`.
- **A simulation that isn't playing** never blocks Enable, Activate or
  Execute. A robot or tool on this lane keeps the lifecycle its controller
  reports (an enabled robot stays enabled); while its streams are stale it
  shows the reason `simulation_not_playing` (never a stream fault or a
  latch). New motion and tool commands on it are refused with
  `simulation_not_playing` and handled like the operator's Stop: the run
  holds and the node waits for Run with that reason; play the simulation
  (by hand or through the Program's own nodes), then press Run. The same
  applies when Isaac stops publishing for any other reason: Isaac is
  external, and ManyForge does not diagnose its failures.
- **Tools** (grippers) have no controller manager: their joints are part of the
  robot's articulation. The tool's launch (`isaac_gripper.launch.py`) publishes
  its description. For the ParallelGripperCommand transport it also runs the bridge.
- **Scene poses** come from the pose source component of the Scenario
  (`pose_source_v0`, the Isaac pose observer). It reads joint states,
  simulation entity poses (by entity name, through `simulation_interfaces`)
  and Detection3D topics. Its per-scene mappings are Scenario data: the pose
  source's `configuration` and the Deployment's `scene_pose_sources[]` with
  `source_key.simulation_entity`.
- **Isaac Sim extensions:** the lane needs none.

## Files

| File | What |
| --- | --- |
| `driver.yaml` | The topic-based hardware plugin and its pinned source. |
| `manyforge_settings.yaml` | The topic-based hardware settings, the controller manager's process settings, the reset-position writer, the cell frame and the gripper defaults, each with its reason. |
| `controllers.template.yaml` | The controller manager and JTC of an Isaac arm without a vendor adapter (UR). The generator fills in joints, `goal`, `goal_time` and the Stop deceleration. |
| `gripper_bridges/<model>.yaml` | The ParallelGripperCommand bridge parameters of a tool model. |

## Supported models and their rows

`lanes.isaac` in each model's `component.yaml`:

| Model | Row |
| --- | --- |
| Lite 6 | `vendor: xarm`, `vendor_model: lite6`, `controller: lite6_traj_controller`, `arrival_tolerance_rad: 0.05`, `runtime_scale: supported`, `goal_time_s: 1.0`, `hard_stop_completion_timeout_s: 1.5` |
| UR10e | `controller: isaac_arm_controller`, `arrival_tolerance_rad: 0.1`, `runtime_scale: supported`, `goal_time_s: 1.0`, `hard_stop_completion_timeout_s: 0.5` |
| Lite gripper | `gripper_transport: isaac_joint_state_effort`: effort on `/isaac_joint_commands_gripper`, dwell completion, no bridge |
| Robotiq 2F-140 | `gripper_transport: ros_gripper_command`, bridge `gripper_bridges/robotiq_2f_140.yaml` |
| Isaac Sim | `adapter: simulation_interfaces` with the standard service names at the root, a 10 s response timeout and a 1 s state poll |

The arrival tolerances and goal times are the values the Isaac lane ran with
before v2.

## Adding a model

1. Build the model's articulation in the scene so that it uses this lane's
   topics (`manyforge_settings.yaml`).
2. Add a `lanes.isaac` row to the model's `component.yaml`:
   - a robot needs `adapter: isaac`, `controller`, `arrival_tolerance_rad`,
     `runtime_scale: supported` (the lane's arm controller claims position only),
     `goal_time_s` and `hard_stop_completion_timeout_s`; a vendor's model also
     `vendor` and the vendor's keys (xArm: `vendor_model`), and its
     `controller` is the vendor's;
   - a tool needs `adapter: isaac`, `controller` (its ParallelGripperCommand action is
     `<controller>/gripper_cmd`), `gripper_transport`, `command_topic`,
     `state_topic`, `completion_policy`, the open/closed positions and efforts
     and `publish_nominal_position` (the command's completion deadline is the
     node's `completion_timeout_s`);
   - a tool with the `ros_gripper_command` transport also needs
     `gripper_bridge`, a file in `gripper_bridges/`, and
     `gripper_stop_finish_tolerance` (a Stop that finds the fingers this close
     to the target lets the goal finish instead of cancelling it). Like the
     stock parallel gripper action controller, the bridge holds the fingers at their
     current position before it accepts a cancel; the acceptance ends the
     stopped command.
3. Map the scene's entities for the Scenario's pose source (Scenario data, not
   here).
