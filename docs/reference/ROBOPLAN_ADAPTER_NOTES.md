# RoboPlan adapter notes

Developer and advanced reference. It describes how ManyForge uses RoboPlan for
motion planning, where RoboPlan's own collision checks are used, and which
behavior belongs to ManyForge rather than to RoboPlan.

ManyForge pins RoboPlan 0.7.0 (commit
`099dfa5ccd80b191eb3d6da9f4795f808a63913f`; see
[`third_party/roboplan/README.md`](../../third_party/roboplan/README.md)).

## For users: support contacts

When the robot picks an object that rests on a support (a table, a tray,
the ground), the object and its support may touch or overlap slightly at the
start of the lift. ManyForge allows that contact only while the carried object
moves away from the support. It does this automatically for contacts it
finds at the start of a motion. You can also declare a support explicitly for
an object: select the object in the Composer **Scene** panel and use its
**Support contacts** card, or add `metadata.support_contacts` to the object in
the Program's `scene`:

```yaml
metadata:
  support_contacts:
    - support_object_id: ground
      release_evidence_mode: signed_penetration
      enforce_max_start_penetration: true
      max_start_penetration_m: 0.03
```

A support contact is not a general "ignore this collision" switch. Any other
collision, a new contact, or the object going back into its support still
fails validation. The modes and fields are described in
[Support contacts](#support-contacts) below.

An object row may also set `metadata.mass_kg` (kg, 0 to 1000). The mass is
added to the robot's controller payload while the object is attached; it is 0
when absent.

## Who decides collisions

`manyforge_collision` is the collision authority. Pre-dispatch trajectory
validation and runtime monitoring run in ManyForge's own collision engine
against the published collision world and collision policy. Its result decides
whether a trajectory is dispatched and whether a running motion is stopped.

RoboPlan generates candidate trajectories. ManyForge calls RoboPlan's
`Scene::hasCollisions` only to reject a planner start state or goal state that
is already in collision (`roboplan_candidate_computer.cpp`). RoboPlan's
planners also use the same planner scene internally while they search. Neither
check can admit a trajectory for dispatch: every candidate, including a cached
one, is validated by `manyforge_collision` first. A cache hit skips planning,
not validation.

Validation stays strict in these cases:

- robot links against world objects are always checked;
- a carried object against world objects is checked, except for a support
  contact that satisfies its release or bounded-penetration rule;
- a carried object against robot links is checked, except for the links its
  attachment declares as touch links.

## Planner scene

Each planning request runs in a separate candidate-worker process
(`roboplan_candidate_worker_main.cpp`), started by
`process_contained_candidate_client.cpp`. The worker builds a fresh RoboPlan
scene for that request from the published ManyForge projection:

1. it adds every collision geometry to the scene;
2. it disables, with `Scene::setCollisions(a, b, false)`, the pairs that the
   effective ManyForge collision policy allows: fixed-object overlap pairs,
   touch-link pairs, support-contact pairs, and the pairs of the active
   contact-grasp policy for the current operation;
3. it places each geometry in its frame (see the next section).

Nothing is re-enabled or restored afterwards. The scene is discarded with the
worker.

### Frame-relative placement

ManyForge stores an object pose relative to the frame named in its state. A
fixed object usually uses a cell or component frame; a carried object uses a
robot frame such as a grasp frame or a link frame.

RoboPlan's scene API accepts a parent frame name, but Pinocchio evaluates
`GeometryObject::placement` in the frame's parent joint, not in the named frame.
A non-root frame can have a fixed offset from its parent joint (a grasp frame
offset from the gripper base joint, for example). Passing the frame-relative
pose directly would put the collision geometry at the wrong place.

The worker therefore places every geometry as:

```text
T_parent_joint_object = T_parent_joint_frame * T_frame_object
```

before calling `Scene::updateGeometryPlacement`.

This conversion matters for correctness. Without it, a carried object could be
planned at a different pose from the one Composer shows. That can fail closed,
as a false gripper/object collision, or fail open, as a missed collision
between the carried object and an obstacle.

## Fixed-object overlap

Two fixed scene objects may touch or overlap in the authored scene, for
example a product mesh that slightly penetrates a table at import. That
overlap should not make the robot's state invalid, because neither object
moves.

This is a general ManyForge collision-policy rule, not a RoboPlan setting.
When the effective collision policy is projected for a logical-scene revision
(`RetainedCollisionWorldProjector::project_effective_policy`), ManyForge adds
these pairs to `allowed_static_static_pairs`:

- every pair of collision shapes between two fixed (not carried) scene
  objects;
- every pair between a fixed scene object and a static component body, that
  is, a robot or equipment body that no moving joint carries (a robot base or
  pedestal, for example).

The policy is recomputed for each logical-scene revision. When an object is
attached to the robot it is carried, not fixed, so its pairs are checked
again; when it is released it becomes fixed again. Robot-link and carried-
object pairs are never added to this list. `manyforge_collision` and the
planner scene use the same effective policy.

## Support contacts

A support contact applies only while the object is carried. Before the attach
step the object and its support are both fixed, so their contact is covered by
the fixed-object rule and an approach motion does not have to separate them.

Support contacts reach the collision policy in two ways:

- **Declared:** `metadata.support_contacts` on the carried object names a
  `support_object_id` and the mode fields below. The projector expands it to
  every shape pair of the object and the support.
- **Discovered:** the execution kernel finds contacts between a carried
  object and a fixed scene object at the start of a motion and adds them as
  support contacts with default fields (`execution_kernel.cpp`,
  `discover_dynamic_carried_start_contacts` and
  `dynamic_carried_start_contact_pairs`). It does this before planning, from the
  observed start state, and again after a failed validation, from the
  trajectory's first waypoint, before validating once more. Declared entries
  take precedence for the same pair. The defaults are strict: contact at the
  start is allowed only as touching (zero start penetration), and the pair must
  separate monotonically and stay separated. Discovery fails the motion if
  declared plus discovered contacts would exceed 64 pairs.

`mode` selects the rule. Authored files accept `contact_release` (the
default) and `bounded_penetration`. The Composer card names the choices
**Precise release**, **Lightweight release** and **Bounded contact**;
**Contact process** is shown as reserved and is not accepted.

### `contact_release`

- the pair may be in collision at the first validation waypoint;
- start contact only makes the pair eligible for release; it does not assume
  the next motion will separate it;
- no other collision pair is allowed because of the support contact, and no
  new colliding pair may appear;
- with `require_separation_by_end: true` (default) the pair must be separated
  at the final waypoint;
- with `allow_recontact: false` (default) contact after the first separation
  fails;
- with `enforce_max_start_penetration: true` (default),
  `max_start_penetration_m` (default 0) bounds the first-waypoint depth.
  Setting it to `false` disables only that start check, not the release,
  separation or re-contact checks.

`release_evidence_mode` selects how progress is proved:

- `signed_penetration` (default, **Precise release**): for each allowed pair
  that is colliding, the engine measures penetration depth.
  `release_tolerance_m` (default 1e-5) is the allowed worsening between
  samples, `min_release_progress_m` (default 0) the required total reduction
  when `require_strictly_decreasing_penetration` (default true) is set.
- `binary_contact` (**Lightweight release**): checks only which pairs collide.
  It permits no new pair, requires the contact evidence to decrease, and
  measures no depth. The depth fields do not apply: leave them unset or
  neutral; other values are rejected.

### `bounded_penetration`

**Bounded contact** allows the named pair while its penetration stays within
`max_contact_penetration_m`, including a pair that first collides later in the
trajectory. The pair does not have to separate. Unexpected pairs, or the named
pair beyond its bound, still fail. The release fields do not apply.

### Validation and monitoring

The same policy is used for pre-dispatch validation and for runtime
monitoring. Signed distance is requested only for selected pairs that are
already reported colliding; missing or non-finite required depth fails closed
as an inconclusive result.

The runtime monitor starts after the trajectory has passed validation and
execution has begun. Its default tick is 50 ms (20 Hz,
`ExecutionKernelConfig::collision_monitor_period_ns`); a Deployment can set
another value with its **Collision monitor period**
(`collision_monitor_period_ms`). It is not a grace
period for support contacts: the first observation initializes each pair's
state, then precise release enforces its start bound and penetration progress,
lightweight release enforces pair-set progress, and bounded contact enforces
the depth bound.

Support contacts are narrower than touch links. Touch links name robot links
that a carried object may touch while grasped. Support contacts name world
objects that the carried object must leave. Using touch links to hide a
support collision would remove real carried-object collisions from the check.

## Robot collision geometry

Robot and equipment collision bodies come from the model's URDF `<collision>`
geometry as loaded by Pinocchio and Coal. The exported descriptors keep the
loaded shape kind and carry quality labels such as `primitive`, `convex`,
`collision_mesh` or `backend_compound`. There is no automatic box (AABB)
fallback for robot links. Turn on **Collision geometry** in the Composer
viewer to see the geometry that is checked.

## Current limits

- Validation checks trajectory waypoints; there is no continuous
  (swept-volume) collision check.
- Signed depth is computed only for precise release and bounded contact, and
  only for colliding pairs.
- Performance depends on projection and publication of the collision world
  plus the ManyForge engine; RoboPlan's own checks cannot admit a dispatch.

## Source locations

| Topic | Location |
|---|---|
| RoboPlan pin and carried patches | `third_party/roboplan/README.md`, `scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`) |
| Effective collision policy (fixed-object pairs, touch links, declared support contacts) | `manyforge_roboplan/src/retained_collision_world_projector.cpp`, `RetainedCollisionWorldProjector::project_effective_policy` |
| Static component bodies | `manyforge_collision/src/manyforge_collision_engine.cpp`, `select_retained_static_component_bodies` |
| Allowed pairs sent to the worker | `manyforge_roboplan/src/process_contained_candidate_client.cpp`, `project_worker_model` |
| Planner scene build, `setCollisions`, frame-relative placement | `manyforge_roboplan/src/roboplan_candidate_worker_main.cpp`, `make_scene` |
| Start/goal `hasCollisions` checks | `manyforge_roboplan/src/roboplan_candidate_computer.cpp` |
| Start-contact discovery | `manyforge_core/src/execution_kernel.cpp`, `discover_dynamic_carried_start_contacts`, `with_dynamic_carried_start_contacts` |
| Support-contact types and defaults | `manyforge_core/include/manyforge_core/types/collision.hpp` (`SupportContactPolicyEntry`), `manyforge_core/include/manyforge_core/types/scene_edit.hpp` (`SupportContactDTO`) |
| Authored `metadata.support_contacts` parsing | `manyforge_deployment/src/initial_scene_projection.cpp` |
| Support-contact field validation | `manyforge_scene/src/logical_scene_owner_transactions.cpp` |
| Release and bounded-contact evaluation | `manyforge_core/src/collision_policy_evaluator.cpp` |
| Monitor period | `manyforge_core/include/manyforge_core/execution_kernel.hpp` |
| Composer card | `manyforge_composer/frontend/src/components/ProgramSupportContactEditor.tsx` |
| Example | `examples/isaac-lite6-trocar-foundationpose.scenario/program.yaml` |

## Upstream simplifications

These RoboPlan features would let ManyForge drop adapter code. They would not
change the collision authority, which stays with `manyforge_collision`.

- A per-request collision policy, equivalent to MoveIt's Allowed Collision
  Matrix for one query, instead of scene-level `setCollisions` on a
  per-request scene.
- Structured contact diagnostics with stable body pairs and penetration depth.
- A frame-relative geometry add/update API that composes
  `T_parent_joint_frame * T_frame_object` internally. Until RoboPlan provides
  or documents one, ManyForge must keep passing parent-joint-relative
  placements.
