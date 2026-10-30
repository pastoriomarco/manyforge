# Controlled Stop profiles

A controlled Stop is an operational software command, not a safety-rated
emergency or protective stop. The robot installation needs independent safety
functions and an appropriate risk assessment.

A Deployment's controlled-Stop settings choose how the robot should stop and
which configured fallback stages are available. ManyForge reports the
outcome in Runtime/history. An acknowledgement does not by itself clear a
fault, prove the cause is gone or start motion again.

## Controlled Stop modes

| Mode | Behavior |
| --- | --- |
| `soft_on_path` | Brakes along the accepted path. |
| `spring_back_hold` | Brakes and returns along the path to the stop-trigger position. |
| `spring_back_fast` | Sends an immediate brake-and-return trajectory to the stop-trigger position. The robot can overshoot before returning. |

**Spring-back fast does not plan or check the generated stop motion for speed,
acceleration or collision.** The robot's own controller or safety system may
stop it. It is intended for searches and probes stopped by an interrupt; choose
it only when that behavior is appropriate for the cell.

The settings also include a Stop time, joint limits for controlled Stops and
fallback stages. The available fields and values are shown in the Deployment
editor. They are separate from the speed limits of ordinary motion.

## Lead-in

A `soft_on_path` Stop (and the braking half of `spring_back_hold`) first
continues the motion the controller is already commanding for a short lead-in
of 0.1 s, then brakes along the path.

A Real robot component has a **Stop lead-in start** setting, shown in the
Deployment editor under *Native runtime configuration* and saved as the
component's `stop_lead_in_anchor`:

- **Commanded position** (`commanded`, the default): the lead-in starts at the
  controller's command position.
- **Measured position** (`measured`): the lead-in starts at the robot's
  measured position. On a real arm the measured position trails the command,
  so the lead-in can be used up before the Stop trajectory can start. The Stop
  then falls back to the hard-stop stage.

Mock and Isaac components always start the lead-in at the command position.
With either value the measured position is still checked: a robot too far off
the planned path, or moving off it, escalates to the hard-stop stage. Like other
Deployment settings, a change takes effect after Apply.

If controlled stages cannot complete, Stop handling can escalate to a hard-stop
stage, using an available hard deceleration or controller cancel. This stage is
not a selectable controlled Stop mode in the Deployment editor.

## Natural completion

When **Allow natural completion on a short remaining path** is enabled, a move
near its end may finish at its planned endpoint instead of taking a controlled
Stop. In particular, this does not return to the trigger position. Leave it off
when a search or probe needs the spring-back behavior at the trigger.

When selecting a Stop mode in the editor, natural completion defaults on for
soft on path and off for the two spring-back modes. The saved setting controls
the actual behavior.

## Stop and resume

A Program's operator Stop policy controls which Program work is held. Depending
on the selected policy, supervisory work may continue while motion is held.
Use the available owner-reported controls to resume or reset; a full-cell Stop
and a fault can require separate recovery. Never infer physical standstill
from a browser state or a lost response.

For a Stop fault, owner-reported evidence that every captured resource has no
active goal, together with the Kernel reporting none reserved, dispatched or
stopping, satisfies the stopped check without a fresh measured-rest sample.
This replaces measured-rest, post-terminal surveillance, and trigger
rest/reconciliation checks. Recovery still waits for durable Stop acceptance
and terminal records, reconciled configuration and Reset state, and the absence
of blocking Stop certificates, uncertain operations and orphan resources. A
stopped check alone does not clear the cell latch.
