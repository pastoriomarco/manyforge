# RoboPlan 0.8.0 TimeOptimal Cartesian timing with a path-velocity bound

Status: feature gap confirmed by source inspection; the local patch is written
against the exact patched 0.8.0 closure and has not yet been built or run; not
yet submitted upstream

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.8.0`, commit `2a6b447e648b1a3337f10cbab995cda0c0a71d55`
- Base for the local patch: that source plus the carried series 0010 through
  0017 (`patches.sha256`). It needs patch 0016 (`TOPPRAOptions::max_path_velocity`).

## Minimal reproduction of the gap

`CartesianPathPlanner::timeOptimal` builds its own `TOPPRAOptions` from
`CartesianPlannerOptions` (dt, mode, scales, blend deviation). A caller cannot
pass a per-grid-point path-velocity bound, so a TimeOptimal Cartesian plan
ignores any tool speed limit (`max_linear_speed` applies to Bounded only).

## Impact

ManyForge `time_optimal` Cartesian moves (CT-1) must hold the vendor TCP speed
limit. Without the bound the whole trajectory is stretched uniformly
afterwards, slowing the move along its whole length.

## Smallest patch

`CartesianPlannerOptions::toppra_max_path_velocity` (a `std::function`, empty
by default), copied into `TOPPRAOptions::max_path_velocity` in `timeOptimal`.

## Focused regression

ManyForge candidate-computer time_optimal Cartesian rows (peak TCP speed at
the samples within the limit). Upstream form: a TimeOptimal plan with a bound
on the tip speed stays under it at the grid points.

## Sanitizer evidence

Pending the M9 build; the patch copies one option.

## Compatibility risk

Additive option, empty by default; existing behaviour unchanged.

## Intended upstream contract

TimeOptimal Cartesian timing accepts the same path-velocity bound as
`PathParameterizerTOPPRA`.

## Removal predicate

Delete after an upstream release provides an equivalent option and the
candidate-computer time_optimal rows pass without it.
