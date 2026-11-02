# Controlled Stop profiles

A controlled Stop is an operational software command, not a safety-rated
emergency or protective stop. The robot installation needs independent safety
functions and an appropriate risk assessment.

Each robot target's Stop settings in the Deployment choose how the robot
stops and which fallback modes are available. They are edited on the
component page's **Stop settings** card and stored as
`components[].target_settings.<target>.controlled_stop`. ManyForge reports the
outcome in Runtime/history. Nothing acknowledges a Stop: once the robot has
stopped, the next command starts from fresh state. The internals are in
[Controlled Stop internals](../reference/STOP_PROFILES_INTERNALS.md).

## Controlled Stop modes

| Mode | Behavior |
| --- | --- |
| `soft_on_path` | Brakes along the accepted path. The default. |
| `spring_back_hold` | Brakes along the path over half the Stop time, then returns along the same path to the stop-trigger position over the other half. |
| `spring_back_fast` | Sends an immediate brake-and-return trajectory to the stop-trigger position. The robot can overshoot before returning. |

**Spring-back fast does not plan or check the generated stop motion for speed,
acceleration or collision.** The robot's own controller or safety system may
stop it. It is intended for searches and probes stopped by an interrupt; choose
it only when that behavior is appropriate for the cell.

Up to two **fallback modes** may follow the chosen mode, in the order
`soft_on_path` → `spring_back_hold` → `spring_back_fast`. When a mode cannot
be planned within the limits (before anything was sent), the next one is
tried. The last stage is always a hard stop: every joint brakes to rest within
its maximum deceleration, with the controller's cancel as the floor. The hard
stop is not selectable in the editor.

## Settings

- **Deceleration time** (**Brake + return time** for the spring-back modes):
  default 1 s, at most 2 s, including the lead-in below.
- **Explicit maximum Stop time** (optional): the time from the Stop request
  within which braking must start; never below the deceleration time.
- **Controlled Stop jerk**: whether the per-joint jerk limits apply.
- **Per-joint limits**: maximum deceleration, settle-velocity threshold and
  optional jerk per joint. The same maximum deceleration is written into each
  joint's controller configuration (`max_deceleration_on_cancel`) on every
  lane and bounds the hard stop; a Stop profile that misses an arm joint is
  refused.

These limits are separate from the speed limits of ordinary motion. A change
takes effect after Apply.

## Lead-in

A `soft_on_path` Stop (and the braking half of `spring_back_hold`) first
continues the motion the controller is already commanding for a short lead-in
of 0.1 s, then brakes along the path.

A Real robot component has a **Stop lead-in start** setting, shown in the
Deployment editor under *Execution host configuration* and saved as the
component's `stop_lead_in_anchor`:

- **Commanded position** (`commanded`, the default): the lead-in starts at the
  controller's command position.
- **Measured position** (`measured`): the lead-in starts at the robot's
  measured position. On a real arm the measured position trails the command,
  so the lead-in can be used up before the Stop trajectory can start. The Stop
  then falls back to the next mode or the hard stop.

Mock and Isaac components always start the lead-in at the command position.
With either value the measured position is still checked: a robot too far off
the planned path, or moving off it, escalates to the hard stop.

## Natural completion

When **Allow natural completion on a short remaining path** is enabled, a move
near its end may finish at its planned endpoint instead of taking a controlled
Stop. In particular, this does not return to the trigger position. Leave it off
when a search or probe needs the spring-back behavior at the trigger.

When selecting a Stop mode in the editor, natural completion defaults on for
soft on path and off for the two spring-back modes. The saved setting controls
the actual behavior.

## Grippers

A Stop cancels a position gripper's goal, except when the command is already
done: the controller reported it reached the goal, the fingers are within the
gripper's **Stop finish tolerance** of the target, or the node set
`command_duration_s` and that long has passed since the command started. Then
the goal is left to finish, like a robot move stopped in its last moment. The
tolerance comes from the gripper's model (about 5% of its stroke); a
Deployment gripper target may override it (`stop_finish_tolerance`; 0 turns
that case off). A cancelled command ends when the controller accepts the
cancel, which holds the fingers where they are (or on the goal's own result,
if that comes first). See
[Grippers](../authoring/GRIPPERS.md#when-a-gripper-command-completes).

## Stop and resume

A Program's operator Stop policy controls which Program work is held. Depending
on the selected policy, supervisory work may continue while motion is held.
Resume always plans from fresh state; it never continues the stopped
trajectory. Never infer physical standstill from a browser state or a lost
response.

A Stop never latches the cell by itself. After a Stop, the cell is latched
while any component still in the cell is in fault or offline, or cannot
report whether it is still executing. The latch is derived from component
states and clears by itself once every component is healthy and idle; Recover
the faulted components to get there. While latched, runs, nodes, manual
motion and manual I/O are blocked; component actions, Apply, New and Load are
not. A global rule trip is an operational stop: never a fault, never a latch.
