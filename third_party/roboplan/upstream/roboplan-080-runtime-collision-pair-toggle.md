# RoboPlan 0.8.0 runtime collision-pair activation on `Scene`

Status: feature gap confirmed by source inspection; the local patch is written
against the exact patched 0.8.0 closure and has not yet been built or run; not
yet submitted upstream

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.8.0`, commit `2a6b447e648b1a3337f10cbab995cda0c0a71d55`,
  tree `56319fede36b09817b343eefdf43215ee0173f95`
- Base for the local patch: that source plus the carried series 0010 through
  0016 (`patches.sha256`). It extends patch 0015's per-pair margins; the
  upstream form depends on neither.
- Dependencies: `pin==4.1.0`, `libpinocchio==4.1.0`, `coal==3.0.3`,
  `libcoal==3.0.3`.

## Minimal reproduction of the gap

`Scene::setCollisions(body1, body2, enable)` is the only way to allow a
collision. It removes or re-adds pairs in `GeometryModel::collisionPairs` and
reassigns the collision data (`refreshCollisionData`). Toggling a pair off and
on therefore:

- moves the re-added pair to the end of the pair list, so the first colliding
  pair a caller reports can change;
- resets every per-pair `collisionRequests` entry (patch 0015 margins) to zero;
- bumps the geometry version, invalidating every `SceneContext`.

A caller that flips `GeometryData::activeCollisionPairs` instead cannot reach
the planners: `getCollisionData()` is `const`, and every `SceneContext` (RRT
per plan, simple IK per solve) builds a fresh `GeometryData` with all pairs
active.

## Impact

ManyForge allows a grasp's contact pairs only while that grasp's plan runs.
Without runtime activation its planner worker rebuilds the whole scene
(~67 ms for a Lite 6 cell) whenever the contact policy differs from the
previous plan.

## Smallest patch

- `Scene::setCollisionPairsActive(pairs, active)`: resolves body names like
  `setCollisions` (a bad name leaves the scene unchanged) and sets
  `activeCollisionPairs` for the resolved pairs present in the list. Pairs
  absent from the list stay removed. No rebuild, no version bump.
- `SceneContext` copies `activeCollisionPairs` with the collision requests.

Pinocchio's narrow-phase (`computeCollisions`), distance, broadphase callback
and broadphase AABB inflation (`BroadPhaseManagerTpl::update`, recomputed on
every update) all honour `activeCollisionPairs`, so no further change is
needed.

## Focused regression

`RoboPlanSceneTest.TestSetCollisionPairsActive`: a sphere colliding with the
robot stops colliding (Scene fast and debug paths, new SceneContext) when its
pairs are deactivated, collides again when reactivated; the pair list, the
sphere's margins and the geometry version are unchanged; a bad body name
returns an error and changes nothing.

## Sanitizer evidence

Pending a build; the patch adds no allocation or index arithmetic beyond
`findCollisionPair`, whose out-of-range result is skipped.

## Compatibility risk

Additive API. `SceneContext` now inherits deactivated pairs; before the patch
no caller could deactivate one, so existing behaviour is unchanged.

## Intended upstream contract

`Scene` offers runtime activation of existing collision pairs that keeps pair
order and per-pair requests, and contexts created afterwards see it.

## Removal predicate

Delete after an upstream release provides equivalent runtime activation that
reaches `SceneContext`-based planners and the focused Scene, worker-protocol
and candidate-client rows pass without it.
