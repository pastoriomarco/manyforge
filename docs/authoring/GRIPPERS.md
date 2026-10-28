# Grippers

## Pneumatic grippers

Start from one of the Mock or Real pneumatic example Scenarios below; each has
a gripper configuration row in its Deployment. The editor can change that row,
but it cannot add a missing gripper row to an arm-only Draft.

In the Deployment Draft, map a pneumatic gripper's Open and Close levels to
one or two existing Boolean I/O output signals. You can also map named inputs:
`open`, `closed`, `pressure` and `workpiece_grasped`. Outputs and inputs may
belong to different I/O components. Configure the actual wiring, signal
polarity and sensor source for the cell before using hardware. Signal-backed
pneumatic I/O is supported on Mock and Real, not Isaac; Isaac uses its existing
gripper transport.

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
