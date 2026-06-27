# RoboPlan Adapter Runtime Notes

This document records backend-specific behavior in the ManyForge RoboPlan
adapter that affects runtime semantics. It is deployment-facing documentation,
not a development plan.

## Detached Static Object Collision Filter

Status: active temporary workaround in `manyforge_roboplan`.

ManyForge scene resources can represent authored world objects that are useful
for visualization, context, and later behavior-tree actions. Two detached scene
objects may touch or overlap in the authored scene without making the robot's
current joint state invalid. For example, a table mesh and a product mesh may
have tiny contact or penetration at import time; robot motion should still be
allowed until the robot or an attached object would collide with something.

RoboPlan currently exposes whole-scene collision validation through
`Scene::hasCollisions(q)`. That query reports every active pair in the Pinocchio
collision model, including detached object vs detached object pairs. If two
static authored objects overlap, RoboPlan can reject the robot start state even
though neither object is moving.

Until RoboPlan provides a filtered robot-state collision query or pair policy,
the ManyForge adapter applies this containment rule:

- detached scene object vs detached scene object collision pairs are disabled
  in the RoboPlan collision projection;
- robot link vs detached scene object pairs remain enabled;
- attached scene object vs detached scene object pairs remain enabled;
- attached scene object vs robot link pairs remain controlled by the object's
  explicit `touch_links` metadata;
- when an object is attached to a robot link, any disabled static pairs
  involving that object are re-enabled;
- when an object is detached back to the world, static pairs involving that
  object are disabled again.

This is intentionally an adapter projection rule. It does not change the
ManyForge semantic scene model, object IDs, authored poses, frame semantics, or
behavior-tree attach/detach semantics.

## Frame-Relative Attached Object Placement

Status: active RoboPlan adapter convention in `manyforge_roboplan`.

ManyForge stores scene-object poses in the semantic frame named by the object
state. For a detached world object, that frame is usually `universe`. For an
attached object, it can be a robot frame such as `grasp_frame` or a link frame.
This matches the authoring model used by behavior-tree attach/detach nodes:
after an object is attached, its tracked pose is relative to the attachment
frame.

RoboPlan's scene API accepts a `parent_frame` name, but the underlying
Pinocchio `GeometryObject::placement` is evaluated in the parent joint frame,
not directly in the named frame. A non-root frame can have a fixed offset from
its parent joint. For example, the UR10e + Robotiq `grasp_frame` is offset from
the gripper base joint. Passing ManyForge's `T_frame_object` directly to
`Scene::updateGeometryPlacement(object, "grasp_frame", T_frame_object)` places
the collision geometry in the wrong RoboPlan world pose.

The adapter therefore projects non-world object poses as:

```text
T_parent_joint_object = T_parent_joint_frame * T_frame_object
```

before calling RoboPlan `add*Geometry` or `updateGeometryPlacement`.

This conversion is load-bearing for collision safety. Without it, an attached
object can be checked at a different pose from the one shown in Composer's viewer.
That can fail closed, as a false gripper/object collision, or fail open, as a
missed collision between the carried object and an external obstacle.

## Support Contacts

Status: active temporary workaround in `manyforge_roboplan`, implementing a
backend-neutral ManyForge scene-resource contract.

When an attached object is intentionally resting on a support object at the
start of a release or retreat motion, the adapter discovers that start contact
from the current collision state. ManyForge scene resources may also declare
`metadata.support_contacts` as explicit policy hints/overrides. A typical
Isaac-imported scene has a graspable product mesh with a small collider
penetration into the table collider. That contact should not reject the start
state if the object is about to lift away, but it must not become a blanket
allowed collision.

The release rule only becomes active once the object is attached to a robot
link. Before attachment, the pair is still a detached scene-object pair and is
treated as static scene quality. This matters for approach motions: the robot
may move toward a box that is resting on a table without being required to
separate the box from the table before the attach step has happened.

The ManyForge semantic rule is:

- an active object/support pair may be in collision at the first
  validation waypoint;
- start contact only makes the pair eligible for a release proof; it is not an
  assumption that the next motion will lift or separate the object;
- no other collision pair is allowed because of the support-contact phase;
- the support pair may remain in contact only until it first separates;
- after separation, re-contact fails validation;
- the support pair must be separated by the final validation waypoint;
- `max_start_penetration_m` is recorded for policy intent and diagnostics, but
  the RoboPlan adapter cannot enforce contact depth until RoboPlan exposes
  structured contact distances/depths.

This mirrors the shape of MoveIt-style planning-scene usage: the semantic scene
is unchanged, while validation applies a request-scoped collision policy. MoveIt
provides per-query collision checks with an Allowed Collision Matrix and contact
maps. RoboPlan currently exposes collision pair enablement through scene-level
`setCollisions(...)`, so the adapter emulates the scoped policy as follows:

1. When an object is attached, the adapter dynamically discovers its current
   contacts with detached scene objects. Only those attached-object/support
   start-contact pairs are disabled in the RoboPlan collision projection so
   planner start-state and RRT validity checks can begin from the supported
   state.
2. Optional `metadata.support_contacts` entries remain as explicit policy
   hints/overrides while RoboPlan lacks structured contact-depth APIs, but
   ordinary pick/release behavior should not require hard-coded program pairs.
3. Trajectory validation temporarily re-enables the discovered support pairs,
   checks the generated path, and restores the planner projection after the
   validation scope.
4. The same validation runs for freshly planned and cached trajectories; cache
   hits skip planning, not validation.

Because RoboPlan only exposes scene-level pair enablement, the execution-time
collision monitor observes the same projection as the planner. That means the
current adapter verifies the planned trajectory's support-contact release, but
it cannot independently detect a runtime deviation that re-enters only that
support pair after validation. This is one of the reasons the workaround must be
removed once RoboPlan exposes request-scoped collision policy/contact queries.

The execution monitor is not a 50 ms "grace period" for support contacts. The
kernel starts monitoring after a trajectory has passed validation and execution
has begun; the default monitor tick period is 50 ms / 20 Hz. Support-contact pairs are
already disabled in the RoboPlan planner projection at that point, so the live
monitor does not require the initial attached-object/support contact to separate
before the first tick. The planned trajectory must have proven release during
validation. The live monitor can still stop on other non-support collisions that
appear during execution, but it does not currently maintain phase state such as
"support pair has separated, now reject re-contact" at runtime.

This is intentionally narrower than `touch_links`. Touch links describe robot
links that an attached object may touch while grasped. Support contacts describe
world objects that the carried object must leave. Expanding touch links to hide
support collisions is not acceptable because it can move a real carried-object
collision out of the checked world.

## Safety Boundaries

The workaround must not hide collisions involving motion:

- planning still rejects robot link collisions against detached world objects;
- planning still rejects attached-object collisions against detached world
  objects unless the pair was dynamically discovered as an attached start
  support contact, or explicitly declared as support-contact metadata, and the
  generated trajectory satisfies the support-contact release rule;
- planning still rejects attached-object collisions against robot links unless
  they are explicitly listed as touch links;
- static-static overlap should be handled as a scene-quality diagnostic, not as
  a hard motion-validity error.

The current RoboPlan adapter does not enforce numeric penetration tolerance
because RoboPlan does not expose structured contact depth through the API used
here. It enforces pair identity and motion phase. Numeric depth belongs in the
future RoboPlan/MoveIt-style contact API tracked in the specs open points.

## Robot Collision Mesh Preservation and AABB Proxy Warnings

Status: active RoboPlan adapter behavior in `manyforge_roboplan`.

Robot collision snapshots preserve backend mesh geometry when the model loader
can expose a mesh resource. This is the preferred path: the validation and
runtime-monitoring collision world should represent the URDF `<collision>`
geometry, not a broad box approximation of thin links such as gripper fingers.

If the adapter cannot preserve/load a mesh-like geometry and must fall back to a
conservative AABB proxy, the exported `CollisionGeometryDescriptor` is marked
with both:

- `conservative_proxy`
- `warning_conservative_aabb_proxy`

Treat that warning as an explicit degraded-geometry signal. A collision reported
against such a body may be a real collision, but it may also be a false positive
caused by the proxy being larger than the real collision mesh. Operators should
enable the Composer collision-mesh overlay and inspect the warning labels before
tuning poses around a suspected proxy collision. Maintainers should prefer
fixing mesh preservation or unsupported geometry handling over moving waypoints
to work around a proxy.

Current gaps to keep visible:

- support-contact release is validated against the planned waypoints, not
  continuously against swept volumes;
- runtime monitoring uses the planner projection, so support-pair re-contact
  after validation is not independently detected unless it also creates another
  checked collision pair;
- contact depth, penetration trend, and final separation tolerance are not
  enforceable until RoboPlan exposes structured contact diagnostics;
- performance still depends on RoboPlan/Pinocchio `hasCollisions(q)` over the
  whole projected scene for each validation waypoint, although support-pair
  projection is no longer toggled per waypoint.

## Source Locations

- `manyforge_roboplan/src/roboplan_planning_context.cpp`
  - `scene_placement_from_tracked_locked`
  - `parent_joint_from_frame_matrix_locked`
  - `apply_static_collision_policy_for_object_locked`
  - `erase_static_collision_policy_for_object_locked`
  - `recompute_support_contact_policy_locked`
  - `set_support_contact_pairs_for_validation_locked`
  - mesh objects are added through RoboPlan's `Scene::addMeshGeometry` API,
    available in the RoboPlan 0.4.0 release pin used by
    `scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`)
- `manyforge_roboplan/include/manyforge_roboplan/roboplan_planning_context.hpp`
  - `disabled_static_collision_pairs_`
  - `disabled_support_contact_pairs_`
- `manyforge_roboplan/test/test_native_backends.cpp`
  - `DetachedStaticObjectOverlapDoesNotBlockRobotState`
- `manyforge_roboplan/test/test_native_e2e.py`
  - `test_attached_object_collision_pose_uses_named_frame_not_parent_joint`
  - `test_attached_object_still_collides_with_world_obstacles_at_carried_pose`
  - `test_declared_support_contact_reaches_validation_and_requires_release`

## Removal Criteria

Remove this adapter workaround when all of the following are true:

- RoboPlan exposes an API or collision policy that validates robot/attached
  motion state without treating detached static object overlap as start-state
  failure.
- ManyForge can call that API without manually disabling detached-object pairs.
- RoboPlan exposes a per-request collision policy equivalent to MoveIt's
  Allowed Collision Matrix query surface, or a richer API that lets ManyForge
  allow support pairs only for the start/release phase of one motion.
- RoboPlan exposes structured contact diagnostics, including at least body
  pairs and preferably contact distance/depth, so `max_start_penetration_m` can
  be enforced rather than recorded.
- Regression coverage proves that overlapping detached scene objects do not
  block robot start-state validation, while attached-object and robot-link
  collisions still block planning.
- This document, the source comments, and the corresponding specs-repo
  open-point are updated with the RoboPlan issue or PR reference.

The frame-relative placement conversion can be removed only if RoboPlan exposes
or documents a frame-relative object update/add API that composes
`T_parent_joint_frame * T_frame_object` internally. Until then, ManyForge must
continue to pass parent-joint-relative placements into RoboPlan.
