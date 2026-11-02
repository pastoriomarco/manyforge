# RoboPlan 0.7.0 per-pair collision margin (security margin) on `Scene`

Status: feature gap confirmed by source inspection; the local patch is written
against the exact patched 0.7.0 closure but has not yet been built or run
(merge 6 build); not yet submitted upstream

ManyForge tracking ID: `RP-UP-070-001`

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.7.0`
- Commit: `099dfa5ccd80b191eb3d6da9f4795f808a63913f`
- Tree: `3ce46cce8b51b21a0eb7b803858cb0750524e5be`
- Base for the local patch: that source plus the carried series 0010 through
  0014 (`patches.sha256`), as materialized by `scripts/setup-docker.sh` into
  `.docker/roboplan`. Patch 0010 turns `collision_model_data_` into an
  `std::optional`; the upstream form of this change does not depend on it.
- Dependencies: `pin==4.1.0`, `libpinocchio==4.1.0`, `coal==3.0.3`,
  `libcoal==3.0.3`.
- Affected surface:
  - `roboplan_core/include/roboplan/core/scene.hpp`: `getCollisionData()` is
    `const` and `broadphase_manager_` is private;
  - `roboplan_core/src/core/scene_context.cpp`: every `SceneContext` builds a
    fresh `pinocchio::GeometryData` whose `collisionRequests` carry the coal
    default `security_margin = 0`.

## Minimal reproduction of the gap

There is no supported way to make `Scene::hasCollisions` treat two bodies
closer than a chosen clearance as colliding:

- `RRTOptions` has only `collision_check_step_size`.
  `RobotBodyFilterOptions::padding` applies to point-cloud filtering, and the
  oink `safety_margin` applies to IK barriers.
- `coal::CollisionRequest::security_margin` exists per pair in
  `GeometryData::collisionRequests`, but the Scene exposes that data only
  through a `const` accessor.
- Even a caller that writes the Scene's requests cannot reach the planners:
  RRT checks through a per-call `SceneContext` (`roboplan_rrt/src/rrt.cpp`),
  whose `GeometryData` is freshly default-constructed.

Measured consequence (ManyForge M55 F-1, Lite 6 + gripper, coal 3.0.3): a
gripper finger grazed a wall with 0.09 mm penetration over a 0.0019 rad band
of joint 2. Sampled edge checks missed it at 0.05, 0.01 and 0.002 rad steps;
with a 3 mm margin on the robot-to-wall pairs it was caught at the default
density.

## Root cause and impact

- The collision margin is a per-pair coal request field. RoboPlan never
  writes it and gives callers no mutable path to it.
- Pinocchio's `BroadPhaseManager` already inflates each object's AABB by
  `(break_distance + security_margin) / 2` when the manager is built, so a
  margin set before the build is honoured by the broadphase as well as the
  narrow phase.
- Cost: on the scene above, `pinocchio::computeCollisions` takes 15.5 us per
  configuration with no margin and 15.6-15.7 us with a 3-5 mm margin. A
  signed-distance query over all active pairs, the alternative, takes about
  13 ms.
- Impact: without a margin, planned paths may skim fixtures, and a sampled
  check is never rigorous against a contact band thinner than its step. With
  a margin `m`, a sampled check whose samples move no robot point more than
  `2m` apart cannot miss a real contact.

## Smallest patch

`patches/0015-roboplan-070-scene-collision-pair-margins.patch`:

1. `Scene::setCollisionPairMargins(std::span<const double> margins_m)` checks
   one finite, non-negative value per `collisionPairs` entry, writes
   `collisionRequests[k].security_margin`, and rebuilds the broadphase
   manager. It returns `tl::expected<void, std::string>` like
   `Scene::setCollisions`.
2. The `SceneContext` constructor copies the Scene's `collisionRequests`
   before it builds its own broadphase manager, so RRT, path checks and every
   other context-based planner see the margins.
3. A focused `RoboPlanSceneTest.TestCollisionPairMargins` case: a sphere well
   clear of the robot does not collide with margin 0 and collides with
   a 5 m margin on its pairs only, through `hasCollisions`, the verbose path,
   and a new `SceneContext`; a size mismatch and a negative value are refused.

The rebuild bumps the geometry version, so a `SceneContext` created before the
call refuses to answer, as it does after any other collision-pair change.
Geometry and collision-pair mutators recreate the collision data and therefore
reset every margin to zero; callers set margins after the scene is complete.

ManyForge use: the candidate worker builds its scene, then sets per-pair
margins on each plan request from the Kernel's clearance policy (3 mm on the
robot-to-environment pairs the collision policy classifies as blocking, 0 for
self, static, allowed, support-contact and contact-grasp pairs and for the
move's target object). IK requests use margin 0. The ManyForge collision
engine applies the same margins to its own `GeometryData` for validation; its
runtime state monitor keeps margin 0.

## Focused regression and sanitizer evidence

Required before qualification, at the merge 6 build:

1. `RoboPlanSceneTest.TestCollisionPairMargins` passes in Release and under
   ASan/UBSan;
2. the existing Scene, SceneContext, path-utils and RRT suites pass unchanged
   (all margins stay zero unless set);
3. both production Dockerfiles check the manifest and apply 0010 through 0015,
   and the recorded build-content digest of `roboplan_0_7_0_ua1_foundation`
   matches the patched tree;
4. the ManyForge worker-protocol, candidate-client and kernel clearance tests
   pass against the patched closure.

## Compatibility and performance risk

- The API is additive. Callers that never set margins see no change: every
  margin stays at the coal default 0.
- The C++ ABI of `Scene` is unchanged in layout; the new member function
  requires every RoboPlan package and consumer to be rebuilt as one exact
  generation, which ManyForge already requires.
- No Python binding is added; the upstream form may mirror the setter.
- A margin adds no measurable per-check cost (see above). A rebuild of the
  broadphase manager per changed margin set is a one-off cost per plan
  request, comparable to `setCollisions`.

## Intended upstream contract and removal predicate

Upstream contract (any equivalent is fine):

```cpp
// Sets coal security_margin per collision pair (index = GeometryModel collision pair index),
// then rebuilds the broadphase manager so AABB inflation reflects it.
tl::expected<void, std::string> Scene::setCollisionPairMargins(std::span<const double> margins_m);
```

optionally with a single-value convenience setter and a read-back getter. The
margins apply to `hasCollisions`, `hasCollisionsAlongPath`, `SceneContext`, and
every planner that uses them.

Remove the local patch when an upstream release provides an equivalent
per-pair margin that reaches `SceneContext`-based planners, the ManyForge
worker sets it through that API, and the focused Scene, RRT, worker-protocol
and kernel clearance rows pass without the patch.

## Upstream submission plan

1. Reduce the change to its upstream form (no dependency on patch 0010) with
   the focused test and the suggested Python binding.
2. Attach the F-1 measurement summary above and the broadphase consistency
   evidence (same results through the broadphase and the verbose path).
3. Carry the exact local patch until an upstream release satisfies the
   removal predicate.
