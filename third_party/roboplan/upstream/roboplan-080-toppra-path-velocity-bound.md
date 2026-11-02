# RoboPlan 0.8.0 per-grid-point path-velocity bound for TOPP-RA

Status: feature gap confirmed by source inspection; the local patch is written
against the exact patched 0.8.0 closure but has not yet been built or run
(M9 build); not yet submitted upstream

ManyForge tracking ID: `RP-UP-080-001` (plan item RP-3)

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.8.0`, commit `2a6b447e648b1a3337f10cbab995cda0c0a71d55`,
  tree `56319fede36b09817b343eefdf43215ee0173f95`
- Base for the local patch: that source plus the carried series 0010 through
  0015 (`patches.sha256`). It touches only `roboplan_toppra`; the upstream
  form does not depend on the earlier patches (0011 shares the file).
- Dependency: TOPPRA 0.6.10 (`toppra::LinearConstraint` x-bounds).

## Minimal reproduction of the gap

`PathParameterizerTOPPRA::generate` builds a fixed constraint list (joint
velocity and acceleration limits scaled by `TOPPRAOptions`). A caller cannot
add a speed limit that is not a joint limit, such as a tool-frame Cartesian
speed cap. Its only option is to time the path within the joint limits, then
stretch the whole trajectory uniformly until the frame's peak speed fits, so a
move that is fast only briefly is slowed along its whole length (ManyForge,
Lite 6 drop move: 2.2 s within joint limits, 4.1 s after the uniform stretch).

## Root cause and impact

- TOPP-RA expresses any bound on the path speed as an x-bound
  (x = (ds/dt)^2) per grid point; a Cartesian speed cap is
  ds/dt <= v_max / ||J(q(s)) q'(s)||, so it fits exactly that form.
- RoboPlan exposes no way to pass such a bound, and its path object and grid
  are private to `generate`.
- Impact: callers with Cartesian speed caps get trajectories that are slower
  than necessary wherever the cap does not bind.

## Smallest patch

`patches/0016-toppra-velocity-bound-hook.patch`:

1. `TOPPRAOptions::max_path_velocity`, an optional
   `std::function<double(q, dq_ds)>` returning the largest allowed ds/dt at a
   grid point (infinity for none). Unset keeps today's behaviour.
2. A private `PathVelocityBound` TOPP-RA constraint, added to the constraint
   list only when the hook is set, evaluates the hook at every grid point with
   the expanded group positions and the path tangent (planar joints in their
   body frames) and writes the x-bound, capped at TOPP-RA's own 1e8 ds/dt
   ceiling.
3. A focused `RoboPlanToppraTest.PathVelocityBoundCapsSpeedOnlyWhereItBinds`:
   a joint-space speed-norm cap at half the unbounded peak holds at every
   sample (2% between-grid tolerance), and the bounded trajectory is longer
   than the unbounded one but shorter than the unbounded one stretched
   uniformly to the same cap.

The bound holds at TOPP-RA's grid points on its standard grid; between grid
points it is not enforced.

ManyForge use: the candidate worker sets the hook for every joint move to hold
the active TCP at 0.98 of the vendor TCP speed limit (linear; angular too for
Cartesian time-optimal moves), then checks the trajectory samples and
stretches uniformly only if a sample exceeds the limit.

## Focused regression and sanitizer evidence

Required before qualification, at the M9 build:

1. the new focused test passes in Release and under ASan/UBSan;
2. the existing TOPP-RA suite passes unchanged (the hook is unset there);
3. both production Dockerfiles check the manifest and apply 0010 through 0016,
   and the recorded build-content digest of `roboplan_0_8_0_foundation`
   matches the patched tree;
4. the ManyForge candidate-computer and Cartesian trajectory tests pass
   against the patched closure, and the planning benchmark records durations,
   peak TCP speed and peak jerk before and after.

## Compatibility and performance risk

- Additive: `TOPPRAOptions` gains one trailing member defaulting to empty;
  aggregate initialisation by position is unchanged. `TOPPRAOptions` layout
  changes, so all consumers rebuild as one exact generation, which ManyForge
  already requires. No Python binding is added.
- Cost: one hook call per grid point (a few hundred for typical paths); the
  ManyForge hook is one forward-kinematics pass per call.

## Intended upstream contract and removal predicate

Upstream contract (any equivalent is fine, e.g. a list of extra
`toppra::LinearConstraintPtr`):

```cpp
// Largest allowed ds/dt at a grid point, given group positions and dq/ds.
std::function<double(const Eigen::VectorXd& q, const Eigen::VectorXd& dq_ds)>
    TOPPRAOptions::max_path_velocity;
```

Remove the local patch when an upstream release provides an equivalent
per-grid-point path-velocity bound (or extra-constraint hook) in
`PathParameterizerTOPPRA::generate`, the ManyForge worker sets it through that
API, and the focused TOPP-RA and ManyForge candidate-computer rows pass
without the patch.
