# Controlled Stop — Internals Reference

Reference companion to the operator runbook
[`operations/STOP_PROFILES.md`](../operations/STOP_PROFILES.md). This document
is for maintainers and for any future contributor (human or LLM) who needs to
**verify the stop subsystem without re-deriving it from the code**: the exact
call chains, the boundary-value math of the soft-on-path ramp, and the
feasibility / scaling rules. File and symbol references are given so each claim
is locatable.

This reference is self-contained for deployment and runtime use. Historical
planning rationale lives outside this repository; this document describes the
*implemented* mechanism and must not depend on private development-spec paths.

---

## 1. What a stop is

An operator/fault stop **preempts** the active mission trajectory with a
*self-computed* stop trajectory. It is not a velocity-scaling burst and it is
not a hold of the existing goal: the kernel computes a new, short trajectory
that brings the arm to rest and dispatches it as a replacement goal. After the
stop completes there is **no active mission trajectory** — the program is
stopped, not paused mid-goal.

Three stop *modes* exist (`MotionInterruptStopMode`,
`manyforge_core/include/manyforge_core/types/motion_interrupt.hpp`):

| Mode | Meaning | Default role |
|---|---|---|
| `soft_on_path` | Decelerate to rest **along the remaining path geometry**, time-scaling the path parameter with a jerk-continuous ramp. | **Universal default** operator stop. |
| `spring_back_hold` | Hold at a zero-velocity setpoint (servo brake to the current commanded pose). | Fallback when `soft_on_path` is unavailable; default *fault* stop. |
| `hard_deceleration` | Maximum-rate joint-space deceleration ignoring path shape. | Never a default unless it is the *only* feasible mode. |

There is also an **immediate floor**: if no controlled stop can be computed or
dispatched, the kernel falls back to the legacy hard stop
(`execution_backend_.stop()`) so a stop request can never be silently dropped.

---

## 2. Call chains

### 2.1 Operator Stop (the default path)

```
UI "Stop"  ─►  POST /api/cycle/stop
           ─►  cycle_manager.cycle.stop()            manyforge_behavior/cycle_manager.py
                  kernel_action = "stop_if_active"    ← routes ANY active state to a stop
           ─►  kernel.stop()  (python binding)
           ─►  ExecutionKernel::stop()                manyforge_core/src/execution_kernel.cpp
                  if operator_stop_profile set AND state ∈ {Executing,Paused,Stopping}:
                      execution_backend_.execute_controlled_stop(profile, stop_token)   (~:1205)
                  else:
                      execution_backend_.stop()       (legacy hard stop, ~:1217)
           ─►  Ros2ControlExecutionBackend::execute_controlled_stop(profile, token)     ros2_control_execution_backend.cpp:2045
                  soft_on_path:
                      plan_soft_on_path_stop(...)      manyforge_core/src/soft_on_path_stop_planner.cpp:384
                      send_fjt_replacement_trajectory(...)   ← preempting FollowJointTrajectory goal
                      if enable_jtc_speed_scaling_input:
                          publish_runtime_velocity_scale(1.0)    (~:2356, atomic with the preempt)
```

Key point for the cycle layer: `cycle.stop()` uses `kernel_action="stop_if_active"`,
**not** `pause_if_executing`. Pausing routes to `kernel.pause()` and never reaches
the controlled-stop branch; the operator Stop must reach `ExecutionKernel::stop()`.
(`_enter_motion_paused_state` intentionally keeps `pause_if_executing` — pause is a
distinct, resumable action.)

If the controlled stop fails for any reason, `ExecutionKernel::stop()` escalates
to the legacy `execution_backend_.stop()` as an **emergency fallback**
(`execution_kernel.cpp` ~:1217–1315, events `operator_stop_profile_failed` /
`legacy_stop_succeeded|failed`). A stop request is never dropped.

### 2.2 Fault preempt

```
fault detected ─► ExecutionKernel fault path (execution_kernel.cpp ~:1529–1578)
                    if default_fault_stop_profile set:
                        execution_backend_.execute_controlled_stop(*fault_stop_profile)   (~:1550)
                    else:
                        execution_backend_.stop()   (legacy, ~:1577)
```

The default fault profile is `spring_back_hold` — a fault should brake-and-hold
the commanded pose, not continue tracing path geometry.

### 2.3 soft_on_path → spring_back fallback (backend)

`execute_soft_on_path_spring_back_fallback`
(`ros2_control_execution_backend.cpp:2179`) re-enters `execute_controlled_stop`
with a `spring_back_hold` profile when soft-on-path cannot be carried out (e.g.
the planner rejects the geometry, or required capability/config is missing).
This keeps the *degraded* behaviour safe (brake-and-hold) rather than failing
open.

### 2.4 Capability advertisement (decision 5R)

`compute_ros2_control_motion_interrupt_stop_capability`
(`manyforge_roboplan/src/ros2_control_capability.cpp`) advertises
`kSoftOnPath` when **both**:

- `enable_soft_on_path_path_ramp` is set, and
- `soft_on_path_capability_config_valid` holds — finite
  `soft_on_path_stop_traj_delta_s ≥ 0`, `sample_period_s > 0`, and every provided
  per-joint jerk entry is a finite positive value.

It is **no longer gated on `!enable_jtc_speed_scaling_input`**. soft_on_path and
the JTC velocity-scaling slider coexist: ManyForge owns the timeline (computes
the stop from effective state, then asserts unit scale atomically with the
preempt), so the slider being wired does not disqualify soft_on_path.

---

## 3. The math (soft_on_path ramp)

Implemented in `manyforge_core/src/soft_on_path_stop_planner.cpp` (~:339–380).

### 3.1 Setup

soft_on_path slows the **path-time rate** `u̇` (the rate at which the path
parameter `u` advances) from the incoming `path_time_rate` to `0` over a ramp of
length `ramp_s` seconds. With normalized ramp coordinate `r = t / ramp_s ∈ [0,1]`:

```
u̇(r) = path_time_rate · v(r)
ü(r) = (path_time_rate / ramp_s) · v'(r)
u(r) = handoff + path_time_rate · ramp_s · ∫₀ʳ v
```

Joint velocity/acceleration follow by the chain rule against the path geometry
`q(u)`:

```
q̇ = q'(u) · u̇
q̈ = q''(u) · u̇²  +  q'(u) · ü
```

The incoming joint acceleration is carried by the geometry term `q''·u̇²`, which
is continuous across the preemption seam because `u̇` matches the incoming rate
there (`v(0)=1`). So only the **time-scaling factor** `v` must be made
acceleration- *and* jerk-continuous at the seam (`r=0`) and at rest (`r=1`).

### 3.2 The chosen factor — minimum-jerk quintic

```
v(r) = 1 − 10r³ + 15r⁴ − 6r⁵          (= 1 − smootherstep(r))
```

Boundary conditions (checkable by hand):

| | r = 0 | r = 1 |
|---|---|---|
| `v`   | 1 | 0 | full incoming rate → rest |
| `v'`  | 0 | 0 | **acceleration = 0** at both ends |
| `v''` | 0 | 0 | **jerk = 0** at both ends |

- `v'(r)  = −30r² + 60r³ − 30r⁴ = −30 r²(r−1)²` → zero at `r=0` and `r=1`.
- `v''(r) = −60r + 180r² − 120r³ = −60 r (1 − 3r + 2r²)` → zero at `r=0` and `r=1`.

So acceleration is continuous with the steady `ü = 0` *before* the seam and is
zero at rest; jerk is likewise continuous (zero) at both ends — no jerk step is
injected at the preemption seam, and none propagates to the joints through the
chain rule.

**Why not the old cubic.** The earlier factor `1 − 3r² + 2r³` (smoothstep) is
acceleration-continuous (`v'(0)=v'(1)=0`) but leaves a nonzero path-time jerk:
`v''(r) = −6 + 12r`, i.e. `v''(0) = −6 ≠ 0` and `v''(1) = 6 ≠ 0`. That is a jerk
discontinuity at the seam and at rest. The quintic removes it.

### 3.3 The 0.5 position factor

```
∫₀¹ v dr = ∫₀¹ (1 − 10r³ + 15r⁴ − 6r⁵) dr
         = 1 − 10/4 + 15/5 − 6/6
         = 1 − 2.5 + 3 − 1
         = 0.5
```

`ramp_position_factor(1) = r − 2.5r⁴ + 3r⁵ − r⁶ │_{r=1} = 0.5`. This is **correct,
not a bug**: a deceleration from full rate to rest covers exactly half the
distance that constant full-rate travel would cover over the same ramp duration.
(It is the area under a curve that starts at 1 and ends at 0 — by symmetry of the
quintic about `r=0.5`, exactly one half.) The default `ramp_s` is the deceleration
time, `kDefaultDecelerationTimeS = 0.5 s` (motion_interrupt.hpp).

### 3.4 Per-joint feasibility / stop-time extension

The configured `deceleration_time_s` sets the *nominal* ramp. Each joint's peak
deceleration over the ramp is bounded by `per_joint_max_deceleration_rad_s2`. If
the nominal ramp would exceed a joint's limit, the ramp is **extended** (slower
stop) so every joint stays within its limit — the stop is never made more
aggressive than the joint allows. (`effective_ramp_s` in
`soft_on_path_stop_planner.cpp`.)

### 3.5 Effective velocity under scaling

With the JTC speed-scaling slider at scale `s`, the *commanded* path-time rate is
`s · path_time_rate`. The stop is computed from the **effective** (scaled) state
so the decel ramp begins from the velocity the arm is actually moving at, then
unit scale (`s = 1`) is asserted atomically with the preempting goal so the
controller does not re-scale the stop trajectory underneath the kernel. The
slider is frozen/greyed for the duration of the stop (ManyForge owns the
timeline).

---

## 4. Tests (catalogue)

> The live `hard_deceleration` and degradation probes, the latency benchmark, and
> the 20-case scaling matrix are tracked under Chunk E/F and are filled in as they
> land. The continuity and capability asserts below exist now.

| Test | File | Proves | How to run |
|---|---|---|---|
| `RampIsAccelerationAndJerkContinuousAtSeamAndTerminus` | `manyforge_core/test/test_soft_on_path_stop_planner.cpp` | `v',v'' = 0` at `r=0,1`; no jerk step at the seam | core C++ suite (manyforge-dev image) |
| `SoftOnPathPathRampAdvertisedEvenWithJtcSpeedScalingInputEnabled` | `manyforge_roboplan/test/test_ros2_capability.cpp` | soft_on_path advertised with speed-scaling on + valid config (5R) | roboplan C++ suite (ros image) |
| `test_stop_routes_through_controlled_stop_not_pause` | `manyforge_behavior/tests/test_cycle_manager.py` | `cycle.stop()` reaches `kernel.stop()`, not `kernel.pause()` | behavior pytest |

Live verification performed: **manyforge-only** lane — operator Stop mid-motion
transitions `executing → stopping → stopped` over the configured ~0.5 s via the
controlled path (vs. an instant pause before the cycle-layer fix). **isaac_sim**
JTC path — pending live Isaac stream (see runbook).
