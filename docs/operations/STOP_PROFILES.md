# Controlled Stop Profiles

> **Current boundary:** I8/P5 is implemented for the maintained ManyForge-only
> native product. The addressed Kernel is the sole product Stop owner and the
> Headless surface is a thin scoped-Stop/Full-Stop/recovery client. Isaac,
> ros2_control product launch, and real-robot use remain I9/I10 qualification
> work; this page does not authorize operating those lanes or treat a software
> Stop as a safety function.

This runbook records the current Stop-profile contract and the behavior that
later transport lanes must preserve.
Implementation details live in
[`../reference/STOP_PROFILES_INTERNALS.md`](../reference/STOP_PROFILES_INTERNALS.md).

ManyForge controlled stops are operational software stops. They are designed to
be deterministic, auditable, and compatible with robot safety functions, but they
are not a certified E-stop or protective-stop channel.

## Stop Modes

| Mode                      | What It Does                                                                                                  | When To Use                                                                                                                     |
| ------------------------- | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `soft_on_path`            | Sends a backend-authored replacement trajectory that decelerates on the already accepted path.                | Default operator stop for ordinary motion.                                                                                      |
| `spring_back_hold`        | Sends a two-phase on-path replacement: decelerate forward, then return on the same path to the trip cursor.   | Operator-selected visible return-to-trip behavior.                                                                              |
| `hard_deceleration`       | Uses JTC `decelerate_on_cancel` when the ros2_control deployment proves that capability.                      | Fast profiled stop floor on ros2_control lanes that support it.                                                                 |
| Immediate hard-stop floor | ros2_control plain cancel/hold or native immediate hold.                                                      | Mandatory final floor when a controlled replacement is refused, times out, or cannot be trusted. Not a normal operator profile. |
| Natural completion        | Keeps the accepted trajectory running when it will reach rest before a useful injected stop can be generated. | Short remaining path. Composer stays in `Stopping` until terminal evidence arrives.                                             |

`spring_back_hold` is not a general fallback. A profile may explicitly qualify
the sole recoverable nominal substitution from `soft_on_path` to
`spring_back_hold`; it is forbidden for precision-contact profiles. Any other
failure advances only through the immutable, deployment-qualified Stop-stage
cascade. Neither a backend nor a client chooses a fallback dynamically.

## Authoring Fields

The retained policy compiler accepts one closed `MotionInterruptStopProfileDTO`.
It is resolved during Prepare and frozen in the participant Stop plan before
physical dispatch; no field is looked up or merged at Stop time.

| Field | Rule |
| ----- | ---- |
| `profile_id` | Portable audit token, not a runtime catalog selector. |
| `stop_mode` | `soft_on_path`, `spring_back_hold`, or `hard_deceleration`. |
| `deceleration_time_s` | Required for `soft_on_path`, optional for `spring_back_hold`, forbidden for `hard_deceleration`; finite and in `(0, 2.0]`. |
| `per_joint_max_deceleration` | Complete source-joint map of typed angular or linear acceleration values; required for the two path modes and forbidden for `hard_deceleration`. |
| `allow_natural_completion_on_short_path` | Legal only for `soft_on_path`; omission normalizes to `true`. |
| `recoverable_substitution_mode` | Optional and solely `soft_on_path` to `spring_back_hold`; never a list of fallbacks. |
| `require_backend_confirmation` | Defaults to `true`; production profiles require it. |
| `per_joint_settle_velocity_threshold` | Optional non-empty typed source-joint velocity map. Prepare may only tighten retained deployment thresholds. |
| `diagnostics` | Bounded closed structured scalar metadata. |

The public profile has no `max_stop_time_s`, `allowed_fallback_modes`,
`joint_deceleration_profile_id`, or unit-implying global joint map. The outer
deadline, escalation order, hard floor, and optional commissioned safety-system
request belong to the verified base plan and participant contract, not to an
operator-editable timeout or backend-selected fallback list.

## Retained Policy Example

The current authoring boundary is the typed policy value retained by the
deployment compiler. Legacy adapter-local `*_rad_s2` profile maps are not the
I8 contract. A mixed angular/linear example is:

```yaml
profile_id: guard.soft
stop_mode: soft_on_path
deceleration_time_s: 1.0
per_joint_max_deceleration:
  shoulder_joint:
    coordinate_kind: angular_rad_s2
    value_rad_s2: 10.0
  slide_axis:
    coordinate_kind: linear_m_s2
    value_m_s2: 2.5
allow_natural_completion_on_short_path: true
recoverable_substitution_mode: spring_back_hold
require_backend_confirmation: true
per_joint_settle_velocity_threshold:
  shoulder_joint:
    coordinate_kind: angular_rad_s
    value_rad_s: 0.01
```

Names are source-URDF/SRDF joints. Prepare must resolve every row to exactly one
affected resource with the same coordinate kind and complete coverage. Unknown,
duplicate, wrong-unit, incomplete, or looser settle rows fail before dispatch.

## Velocity Scaling

ManyForge freezes the owned runtime velocity scale during timed controlled stops.
Operator slider edits made during the stop are deferred and applied after the
stop window ends. The slider should be disabled only while the cycle is
`Stopping`, not after it has reached `Stopped`.

Hardware or pendant scaling that ManyForge cannot read/freeze is deferred from
the current release gate. It must be qualified on the real robot lane before
claiming timed behavior under external scaling.

## Stop History And Incidents

Every accepted Stop publishes immutable scope, revision, deadline, and
participant-budget truth. Terminalization publishes one immutable certificate;
faults and stronger-than-nominal stages also retain a fault latch. History
contains:

- all recent events in order;
- events grouped by channel;
- requested and executed stop modes;
- hard-stop floor level;
- timing and settle evidence;
- exact qualified substitution or escalation reasons;
- participant stage results and evidence-source freshness;
- persistence and post-terminal surveillance state.

Inspect the exact Stop status, certificate, and recovery offer after any warning
or fault. Alarm acknowledgement records operator awareness only; it never clears
the Stop latch or reopens admission.

## Offline Qualification

Run the offline controlled-stop suite from the repo root:

```bash
./scripts/qualify-controlled-stop-offline.sh
```

This runs Docker-contained tests for:

- Composer stop-capability admission gates;
- frontend stop profile selectors and build;
- core soft-on-path, interrupt-manager, and execution-kernel tests;
- native backend and ros2_control capability tests;
- planner latency evidence.

Offline qualification does not prove live controller behavior. It proves the
deterministic math, policies, UI gates, and native/ros2 capability surfaces.

## Live Validation Checklist

When Isaac or a real robot is available, capture evidence for:

1. `soft_on_path` at the selected stop time.
1. `spring_back_hold` returning to the trip cursor.
1. `hard_deceleration` when supported by JTC.
1. Natural completion when remaining trajectory time is shorter than the stop
   window.
1. Stop while planning, validating, executing, and stopping.
1. Run-from-stopped replans from current state and does not reset.
1. Velocity slider is frozen only during `Stopping`.
1. Stop history shows requested/executed mode, hard-stop floor, timing, and
   settle evidence.
1. Stop/fault intervention recovery choices work as labeled:
   **Acknowledge fault** clears the operational latch without backend reset,
   leaves motion stopped, and exposes a separate Resume command that replans
   from the current pose; **Reset cycle** explicitly uses the reset path and may
   command the configured initial pose in
   commanded-sim deployments.
1. For real robots, hardware/pendant scaling behavior is observed and classified
   before claiming timed guarantees.

For Lite6 real-robot validation, first reproduce the lane in Isaac with the Lite6
scene and deployment, then repeat the same stop matrix on the physical robot.

## Intervention Recovery Choices

Stop/fault `INTERVENTION_REQUIRED` is a recovery boundary. Pressing Run while it
is latched does not silently acknowledge the fault. The current owner presents:

- **Recover Stop fault**: a revision-fenced request over the exact current offer.
  The Kernel joins fresh quiescence, configuration, reset, external
  reconciliation, and orphan-fence facts, persists the cleared latch and
  resulting status, and only then may reopen admission. It never resumes work.
- **Reset operation**: an independent operation control. If containment is
  required, it joins the one addressed Stop certificate before process-local
  retirement; it cannot create a competing backend Stop chain.
- **Cancel**: leave the intervention latched.

The floating alarm stack exposes **Acknowledge alarm** for the exact current
activation. Acknowledgement records operator awareness; it does not clear the
condition, fault, recovery offer, or backend state. Up to three newest alarms
requiring acknowledgement are shown. Older attention alarms remain retained
and surface as newer ones are acknowledged. Repeated samples of one still-
active condition update its bounded evidence instead of creating popup spam;
after a real return and recurrence, a new activation is annunciated.

**Acknowledge all** appears only when at least two alarms require
acknowledgement and requires confirmation. Retry/Resume/Replan remains disabled
until every motion-attention alarm is acknowledged and alarm state is healthy/
reconciled, while Stop and explicitly safe Reset remain available. An
acknowledged active condition remains a standing alarm until its named source
reports return-to-normal. Operational fault acknowledgement exists only in the
execution controls. The Messages/History drawer retains source events and
alarm/recovery audit records after attention clears.
Unknown/unclassified interventions, unreadable backend state, active
direct-control leases, backend still stopping, or non-settled measured velocity
remain fail-closed until the operator fixes the cause or chooses Reset.
