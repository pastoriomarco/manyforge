# RoboPlan 0.8.0 Bounded Cartesian timing with a curvature speed cap

Status: defect confirmed by the patch's own test on the patched 0.8.0 closure
(Thor arm64); not yet submitted upstream

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.8.0`, commit `2a6b447e648b1a3337f10cbab995cda0c0a71d55`
- Base for the local patch: that source plus the carried series 0010 through
  0018 (`patches.sha256`). It touches only `roboplan_cartesian_planning`.

## Minimal reproduction of the defect

`CartesianPathPlanner::timeBounded` caps each sample's path speed from the
commanded Cartesian maxima, the joint velocity limits and the joint q'' term.
It never looks at the tool path's own curvature. A Bounded plan along a 10 mm
quarter circle between two straight legs (`max_linear_speed` 0.15 m/s,
`max_linear_acceleration` 0.4 m/s²) crosses the arc at 0.098 m/s. The tool's
acceleration reaches 0.97 m/s², 2.4 times the limit; at the full commanded
speed the lateral acceleration v²/r would be 2.25 m/s².

## Impact

ManyForge rounds corners before RoboPlan (blend radius), so that
constant_speed Cartesian moves run through corners without stopping. Our
frame-dynamics check sees the arc's lateral acceleration (11.3 times the
limit on the benchmark U path with 10 mm corners) and stretches the whole
trajectory uniformly (3.76 times), so the straight legs slow down too.

## Smallest patch

In `timeBounded`, each waypoint inside a run gets the radius r of the circle
through it and its two neighbours. Every sample whose span reaches into that
waypoint's neighbouring spans takes the curvature. The per-sample speed cap
then gets the same treatment as the joint q'' term just above it: the speed
is capped so that the lateral acceleration v²/r takes at most half of
`max_linear_acceleration` (v ≤ √(0.5 · a · r)), and the path acceleration
gets the rest. Waypoints at a run's stops are skipped, because the tool
passes them at rest. Straight runs, stops and samples without curvature keep
their caps.

The half split keeps a positive path acceleration on curves. With the full
limit, a run that starts at rest on a curve could never speed up.

## Focused regression

`CartesianPlannerTest.BoundedModeSlowsOnlyOnCurves` (in the patch): the arc
speed stays within √(a·r) and above half of it, so there is no stop. The
finite-difference tool acceleration stays within 1.1 × the linear limit, and
the straight legs still reach `max_linear_speed`. On unpatched 0.8.0 the test
fails with 28 failures (arc speed 0.098 m/s against 0.066; acceleration up to
0.97 m/s² against 0.44). All 16 tests of `test_cartesian_path_planner` pass
with the patch.

## Sanitizer evidence

See the image row `manyforge-ros-lyrical-full:m9-rp08c` in the external
delta ledger.

## Compatibility risk

Bounded plans with curved runs get slower on the curves, which is the
intended correction. Paths whose interior waypoints are collinear are
unchanged.

## Intended upstream contract

In Bounded mode, `max_linear_acceleration` bounds the tool's whole linear
acceleration, including the lateral part on curves, not only the tangential
part.

## Removal predicate

Delete after an upstream release bounds the curvature acceleration in Bounded
Cartesian timing, and both the RoboPlan curve test and the candidate-computer
constant_speed rounded-corner rows pass without the patch.
