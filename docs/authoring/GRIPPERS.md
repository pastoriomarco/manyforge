# Grippers

## The gripper is a tool component

A gripper is its own component, a tool, separate from the robot that carries
it. In the Deployment:

- The robot component uses an arm-only model (for example `lite6_robot`, the
  `arm` member of the machine `lite6`, model `ufactory.lite6`). It has no
  gripper row and no `gripper_feedback` stream.
- The tool component is named after its tool family (`lite_gripper`,
  `robotiq_2f_140`, `robotiq_2f_85`) and selects the tool model by its id
  (`component_profile_id: ufactory.lite_gripper`). It carries the gripper row
  (`gripper_target_configurations`, command target `gripper`). The row's
  `mount_frame` is the tool's root link. The tool has no
  `root_pose_in_cell`.
- One `attachments` row mounts the tool on the robot:
  `{"parent": {"component_id": "lite6_robot", "interface": "flange"},
  "child": {"component_id": "lite_gripper", "interface": "mount"}}`. An
  optional `pose` offsets the mount (for a spacer); the default is identity.
  More than one tool may share an interface; when two or more tool-tagged
  components share one interface, mark exactly one attachment row
  `primary: true` in the Scenario Deployment. If only one tool shares the
  interface, it is the primary tool.
- The attachment row may list `collision_exclusions`:
  `[{"child_link": "lite_gripper_left_finger", "parent_link": "link4"}, ...]`.
  Each pair is a tool link and a link of the parent's model that never touch,
  so planning skips checking them. The pairs depend on the arm–tool
  combination, so they live on the mount, not in the tool package. ManyForge
  checks only that each link exists (Apply refuses with
  `component_attachments` otherwise); a wrong pair hides a real collision.
  On the Components page the mounted tool's **Collision exclusions with
  parent** field edits them (one `tool_link parent_link` pair per line);
  moving the tool to another parent clears them. They are cell context: an
  Apply that changes only them restarts nothing.

The tool's `lane` selects how it runs:

| Tool `lane` | Runs on | Streams |
| --- | --- | --- |
| `mock` | Mock: simulated fingers, the tool's own Mock gripper controller | `gripper_feedback`, `measured_joints` |
| `isaac` | Isaac: the simulator's gripper transport (below) | `gripper_feedback`, `measured_joints` |
| none (pneumatic tools only) | The host's generic adapter; the valve and sensors are signals of I/O components on any lane | none |

A lane-free pneumatic tool takes its signals from I/O components, so the same
tool row serves Mock I/O devices and a Real machine's control-box outputs
(for example `lite6_robot_controller_io` `CO0`), with any robot lane. Its
adapter has no device of its own: Prepare, Enable, Disable and Recover succeed
at once, and it is busy while an open or close runs. A commanded lane-free
tool needs `pneumatic_io`; with `output_mode: none` it is display only (as in
`examples/real_lite6_arm_observation.scenario`).

A Program keeps its roles: `gripper` targets the component assigned to role
`tool`. A move chooses its TCP with the selector ([TCPs](TCPS.md)):
**Default TCP of <tool role>** is the mounted tool's default TCP, and
**Current** is the robot's Active TCP. It starts at the End-effector TCP: the
primary tool's default TCP when a tool is on the flange, otherwise the robot's
own default TCP or flange. If the primary tool has no TCP, the robot has no
End-effector TCP. A Scenario may mount a tool without assigning it, for
example when its Program only moves the arm.

To add a tool to a Draft, use Add component with kind Tool and pick the robot
to mount it on.

## Pneumatic grippers

Start from one of the Mock or Real pneumatic example Scenarios below; each has
a pneumatic tool component with its gripper row. The editor can change that
row.

In the Deployment Draft, map a pneumatic gripper's Open and Close levels to
one or two existing Boolean I/O output signals. You can also map named inputs:
`open`, `closed`, `pressure` and `workpiece_grasped`. Outputs and inputs may
belong to different I/O components. Configure the actual wiring, signal
polarity and sensor source for the cell before using hardware. Signal-backed
pneumatic I/O belongs to a lane-free tool; on the Isaac lane the tool uses the
simulator's gripper transport, and Apply refuses `pneumatic_io` there
(`pneumatic_io_unsupported_on_isaac`).

Each Program gripper command can select the named inputs and values it
requires. All selected inputs must match: the current fresh value is checked,
so a value that already matches counts. After they have matched, the values
are read again to confirm they still match. With required inputs, a positive
`command_duration_s` is the maximum wait; the node fails if they do not match
in time. A duration of zero waits without a time limit, subject to Program
Stop, Reset and interrupts. With no required inputs, the ordinary dwell
behavior applies.

For a signal-backed pneumatic gripper, manual Open and Close are available
only while the Program is idle. They write the configured outputs and complete
without waiting for the Program node's required inputs. For both paths, outputs
set to `false` are written before outputs set to `true`. A refused write, or
one whose outcome is unknown, fails the command; nothing resends it, and an
unknown output stays unknown until the device reports it. A bare gripper operation outside these paths changes only its logical
state and does not itself write the valve outputs.

Do not command the same pneumatic gripper, or grippers sharing valve outputs,
from parallel Program branches. Also avoid parallel I/O writes to those valves
while a gripper command is running. Operators sharing a cell must not issue
overlapping manual commands to the same mapped valves. The last write to each
output wins.

A Stop does not cut off an output write: a write already sent completes, and
its result settles the command. Once waiting for inputs or dwell, the normal
while-held rules apply.

If Close has required inputs, success means those input values matched; it does
not by itself add typed physical-grasp evidence to the Scene attachment. A
`workpiece_grasped` signal can also be used in a carrying-related interrupt
rule ([Interrupts](INTERRUPTS.md)). While the rule can't watch its signal
(stale or missing), it holds its scope (`rule_holds`), even while the carrying
condition is false, and clears by itself when the signal is fresh again.

### Examples

- `examples/mock_lite6_pneumatic_pick_and_place.scenario` uses a separate Mock I/O device. Its
  delayed valve-to-input echo is synthetic; it does not prove a physical grasp.
- `examples/mock_lite6_pneumatic_grasp_confirmation.scenario` is the same cell
  with grasp confirmation required on its `lite_gripper` tool (see Grasp
  confirmation below). Its Close has no required inputs, so the
  `workpiece_grasped` evidence alone decides the Close. The Mock echo is still
  synthetic.
- `examples/real_lite6_pneumatic_valve_only.scenario` is the starting point for
  a valve-only gripper without a sensor. It uses a site robot address and one
  valve output on a control-box pin (`lite6_robot_controller_io` `CO0`).
  Close completes after its `command_duration_s` dwell; nothing confirms the
  grasp, and there is no workpiece-loss rule. Its pin and polarity are illustrative: adapt and verify
  them for the actual wiring before using hardware.
- `examples/real_lite6_pneumatic_pick_and_place.scenario` is the one to adapt
  when a grasp sensor is wired. It uses a site robot address, a valve output and a
  separate periodically published Boolean sensor input; Close waits for the
  input and a workpiece-loss rule watches it while carrying. Its pin, polarity
  and topic are illustrative: adapt and verify them for the actual wiring and
  sensor before using hardware.

Neither Real example alone qualifies the real robot or its safety setup.

## Grasp confirmation

By default a successful Close does not confirm that a part is held. The tool
component's gripper row (its `gripper_target_configurations` entry, for
example on `lite_gripper`) can require that confirmation:

```json
"grasp_confirmation": {"policy": "required"}
```

`policy` is `required` or `not_required`. Leaving the field out is the same as
`not_required`. When confirmation is required, each Program Close also needs
evidence of the grasp (manual Close from the Composer is not checked):

- **Pneumatic tool.** The row must map the `workpiece_grasped` input in
  `pneumatic_io`. When the Close completes, that input is read once. Only a
  fresh `true` value counts as evidence.
- **Position gripper.** The row must also set
  `"stall_window": {"min_position": <lower>, "max_position": <upper>}`. The
  values use the gripper's own unit (m or rad). When the Close completes, the
  measured finger position is checked; if no measured position is available,
  the commanded end position is used. A position inside the window counts as
  evidence.

Apply refuses the row with `grasp_confirmation_invalid` when the policy is not
one of the two values, the field has an extra key, a pneumatic row sets
`stall_window`, or the window is not finite with `min_position` at most
`max_position`. It refuses with `grasp_confirmation_source_missing` when a
required pneumatic row has no `workpiece_grasped` input, or a required
position row has no `stall_window`.

A command counts as a Close when its position is at least as near to the
closed preset as to the open preset. Opens are never checked. Without
evidence, the gripper node fails with the reason `grasp_not_confirmed`. In the
usual Close-then-attach sequence, the attach node then does not run, so the
Scene gets no attachment. Grasp-lost handling after an attach does not change.

The position check runs only when the Close completes. A Close that the
gripper driver itself reports as failed fails with that driver error.

On a Mock cell the evidence is synthetic. In
`examples/mock_lite6_pneumatic_grasp_confirmation.scenario`, the Mock I/O device
echoes the valve to `workpiece_grasped` after 100 ms, well within the Close's
0.8 s dwell. To see `grasp_not_confirmed`, raise the echo's `delay_ms` above
the dwell (for example to 3000) and Apply.

## When a gripper command completes

A controlled gripper (a position gripper on a controller: the Mock lane's
gripper controller, Isaac's ParallelGripperCommand transport, or the Real driver) is
done when its controller reports the command done. `command_duration_s` with
**Wait for command duration** (`wait_for_duration: true`) makes the node wait
**longer** than that: until the duration has also passed since the command
started. That is settling time, useful mostly for pneumatic and other grippers
without a controller; a controlled gripper rarely needs it. A pneumatic gripper
has no controller report: its `command_duration_s` is the dwell.

| Gripper | Wait for command duration off | on | Stop |
|---|---|---|---|
| Controlled | the controller's result (Isaac `isaac_joint_state_effort` has none: after `command_duration_s`) | the controller's result and `command_duration_s`, whichever comes last | cancels the goal, unless the command is already done (below) |
| Pneumatic | the confirmed output write, then the `command_duration_s` dwell (with required inputs: the inputs, `command_duration_s` the maximum wait) | same as off (ignored) | no cancel: a sent write completes, then the while-held rules apply |

The flag works the same on Mock, Isaac and Real; the Mock example Programs
with a position gripper set it. In a Pick or Place template,
`tool_wait_for_duration` sets it on the template's gripper node, like
`tool_command_duration_s`.

If the controller's report doesn't come, the command fails after its result
timeout (`gripper_result_timeout`):

- controlled grippers: `gripper_result_timeout_s` in the component model's
  lane row (5 s in the component template; the Robotiq examples use 15 s),
  counted after `command_duration_s`;
- pneumatic grippers: `delivery_result_timeout_s` on the gripper's row of the
  Deployment's Current gripper configuration, default 1 s, for the output
  write's delivery result.

**Stop.** A Stop sends no cancel when a controlled gripper's command is
already done: the controller reported it reached the goal, the fingers are
within the Stop finish tolerance of the target (see below), or the node set
`command_duration_s` and that long has passed since the command started (the
author's declared duration; a too-short one lets a still-moving gripper
finish). The command then finishes and the controller's own result ends it,
like a robot move stopped in its last moment; this includes a Stop while the
command waits for its duration. Otherwise the Stop cancels the goal: the
controller holds the fingers where they are and accepts the cancel, and that
acceptance ends the command (or the goal's own result, if it comes first; a
later one is ignored). The next gripper command can then start.

## Stop near the target

One case where a Stop does not cancel a controlled gripper's goal (above) is
the Stop finish tolerance: when the fingers are already within it of the
commanded position, the command finishes and the controller's own result
settles it, instead of the fingers being held short of the target. The tolerance is in the gripper's position unit (m
or rad) and deliberately wide. Its default is the tool's lane row
`gripper_stop_finish_tolerance` (about 5% of the stroke: Robotiq 2F-85
0.04 rad, 2F-140 0.035 rad, xArm gripper 0.04 rad, Lite gripper 0.0004 m on
Mock). A position gripper's row can override it, from the Deployment's
gripper settings ("Stop finish tolerance"; blank keeps the default) or as
`"stop_finish_tolerance": <value>` (0 or more; 0 always cancels). Like other
gripper settings it takes effect at the confirmed Apply, which replaces the
tool.

## Grippers in Isaac

In Isaac a tool's finger joints are part of the robot's articulation; the
tool has no controller manager of its own. Its launch
(`isaac_gripper.launch.py`) publishes the tool's description. The tool's
`lanes.isaac` row picks the transport (`gripper_transport`):

- `ros_gripper_command` (Robotiq 2F-140): a ParallelGripperCommand action
  `<controller>/gripper_cmd`, served by a bridge that the tool's launch also
  runs; its parameters are the lane's `gripper_bridges/<model>.yaml`, named
  by the row's `gripper_bridge`. The row also has
  `gripper_stop_finish_tolerance`.
- `isaac_joint_state_effort` (Lite gripper): an effort written straight to
  the joint command topic, completion after the dwell, no bridge.

The row also names the command and state topics, the completion policy and
the open/closed positions and efforts; `manyforge_isaac_adapter/lane/README.md`
lists them. While the simulation isn't playing, a new gripper command on the
tool fails with `simulation_not_playing`.

## Position without feedback

For a pneumatic gripper without position feedback, the displayed opening is
an expected position, not a measured finger position or proof of grasp. After a
command completes, a later motion normally uses its commanded opening for
planning and runtime collision checks. If model projection fails, the operation
reports `gripper_projection_failed`; the display may already show the attempted
opening. Reset the cycle, or Resume after a Stop. Once recovery permits a new
command, it republishes the opening.

A known exception can occur if an older opening update is delayed across Reset
or rebind: it can overwrite even a newer completed command in the planning
state. Once that delayed update has landed, a further completed gripper command
restores the commanded opening.

## Gripper changes during arm motion

If a gripper opens or closes while an arm motion is already running, that
motion's runtime collision check does not incorporate the changed finger
opening. A motion prepared afterward normally uses the updated gripper state;
the no-feedback exception above also applies. This limitation applies to all
grippers.
