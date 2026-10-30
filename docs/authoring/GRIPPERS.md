# Grippers

## The gripper is a tool component

A gripper is its own component, a tool, separate from the robot that carries
it. In the Deployment:

- The robot component uses an arm-only model and profile (for example
  `lite6_robot` with `lite6_mock_commanded_v0`). It has no gripper row and no
  `gripper_feedback` stream.
- The tool component is named after its tool family (`lite_gripper`,
  `robotiq_2f_140`, `robotiq_2f_85`). It uses the tool model and carries the
  gripper row (`gripper_target_configurations`, command target `gripper`). The
  row's `mount_frame` is the tool's root link. The tool has no
  `root_pose_in_cell`.
- One `attachments` row mounts the tool on the robot:
  `{"parent": {"component_id": "lite6_robot", "interface": "flange"},
  "child": {"component_id": "lite_gripper", "interface": "mount"}}`. An
  optional `pose` offsets the mount (for a spacer); the default is identity.
  More than one tool may share an interface; when two or more tool-tagged
  components share one interface, mark exactly one attachment row
  `primary: true` in the Scenario Deployment. If only one tool shares the
  interface, it is the primary tool.

The tool's profile selects its lane:

| Tool profile | Lane | Streams |
| --- | --- | --- |
| `<tool>_mock_commanded_v0` | Mock: simulated fingers, the tool's own Mock controller | `gripper_feedback`, `measured_joints` |
| `<tool>_isaac_commanded_v0` | Isaac: the simulator's gripper transport | `gripper_feedback`, `measured_joints` |
| `<tool>_pneumatic_v0` | None of its own: the pneumatic tool's lane is that of the I/O components its `pneumatic_io` names | none |

A pneumatic tool takes its signals from I/O components, so the same tool row
serves Mock I/O devices and Real `xarm_digital` outputs, with any robot lane.
A commanded `<tool>_pneumatic_v0` tool needs `pneumatic_io`; with
`output_mode: none` it is display only (as in
`examples/lite6_real_arm_observation.scenario`).

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
pneumatic I/O belongs to the `<tool>_pneumatic_v0` profile; the Isaac tool
profile uses the simulator's gripper transport and takes no `pneumatic_io`.

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
set to `false` are written before outputs set to `true`. A refused or uncertain
output write stops the command; the ordinary I/O recovery rules apply before another
command. A bare gripper operation outside these paths changes only its logical
state and does not itself write the valve outputs.

Do not command the same pneumatic gripper, or grippers sharing valve outputs,
from parallel Program branches. Also avoid parallel I/O writes to those valves
while a gripper command is running. Operators sharing a cell must not issue
overlapping manual commands to the same mapped valves. The last write to each
output wins.

If a Program Stop holds admission while valve writes are in progress, Resume
can continue after an already-accepted write settles while held. A write
refused before admission can instead cause a fresh attempt that reissues
already-confirmed values, writing false outputs before true outputs. An
uncertain write uses the normal recovery rules. Once waiting for inputs or
dwell, the normal while-held rules apply.

If Close has required inputs, success means those input values matched; it does
not by itself add typed physical-grasp evidence to the Scene attachment. A
`workpiece_grasped` signal can also be used in a carrying-related interrupt
rule. Source-loss handling can still apply when the input source becomes
unavailable, even while the carrying condition is false.

### Examples

- `examples/lite6_pneumatic_mock.scenario` uses a separate Mock I/O device. Its
  delayed valve-to-input echo is synthetic; it does not prove a physical grasp.
- `examples/lite6_pneumatic_grasp_confirmed_mock.scenario` is the same cell
  with grasp confirmation required on its `lite_gripper` tool (see Grasp
  confirmation below). Its Close has no required inputs, so the
  `workpiece_grasped` evidence alone decides the Close. The Mock echo is still
  synthetic.
- `examples/lite6_pneumatic_real_no_sensor.scenario` is the starting point for
  a valve-only gripper without a sensor. It uses a site robot address and one
  valve output on a controller pin. Close completes after its
  `command_duration_s` dwell; nothing confirms the grasp, and there is no
  workpiece-loss rule. Its pin and polarity are illustrative: adapt and verify
  them for the actual wiring before using hardware.
- `examples/lite6_pneumatic_real.scenario` is the one to adapt when a grasp
  sensor is wired. It uses a site robot address, a valve output and a
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
`examples/lite6_pneumatic_grasp_confirmed_mock.scenario`, the Mock I/O device
echoes the valve to `workpiece_grasped` after 100 ms, well within the Close's
0.8 s dwell. To see `grasp_not_confirmed`, raise the echo's `delay_ms` above
the dwell (for example to 3000) and Apply.

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
