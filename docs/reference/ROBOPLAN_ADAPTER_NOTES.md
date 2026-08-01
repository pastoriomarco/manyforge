# RoboPlan Adapter Runtime Notes

> **Current `dev` status:** this is historical baseline behavior until native
> restoration. `manyforge_roboplan` currently contains only retained-model
> staging and offline import qualification; its former runtime planner,
> projection, execution, gripper, Python, and dynamic-collision adapters have
> been destructively removed.
> Present-tense adapter behavior below is the captured baseline unless a
> paragraph explicitly identifies retained offline staging or qualification.

This document records backend-specific behavior in the ManyForge RoboPlan
adapter that affects runtime semantics. It is deployment-facing documentation,
not a development plan.

## Detached Static Object Collision Filter

Historical baseline status: active temporary workaround in
`manyforge_roboplan`.

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

Historical baseline status: active RoboPlan adapter convention in
`manyforge_roboplan`.

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

This conversion is load-bearing for collision correctness. Without it, an attached
object can be checked at a different pose from the one shown in Composer's viewer.
That can fail closed, as a false gripper/object collision, or fail open, as a
missed collision between the carried object and an external obstacle.

## Support Contacts

Historical baseline status: active temporary workaround in
`manyforge_roboplan`, implementing a
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

For `mode: contact_release`, the common semantic rule is:

- an active object/support pair may be in collision at the first
  validation waypoint;
- start contact only makes the pair eligible for a release proof; it is not an
  assumption that the next motion will lift or separate the object;
- no other collision pair is allowed because of the support-contact phase;
- no new colliding pair may appear;
- after separation, re-contact fails validation;
- the support pair must be separated by the final validation waypoint;
- when `enforce_max_start_penetration=true`, `max_start_penetration_m` bounds
  first-waypoint penetration depth if structured signed-distance diagnostics are
  available;
- setting `enforce_max_start_penetration=false` disables only that initial-depth
  envelope, not release, separation, or re-contact validation.

The configured evidence mode then selects how progress is proved:

- `release_evidence_mode: signed_penetration` is the precise policy. For each
  allowed pair that is currently colliding, it checks penetration depth and
  rejects worsening/non-improving release as configured.
- `release_evidence_mode: binary_contact` is the lightweight policy. It checks
  colliding pair identity/count only, permits no new pair, requires strictly
  decreasing contact evidence, and requires no contact at the final waypoint.
  It does not claim to measure worsening depth within an unchanged mesh-feature
  contact. `enforce_max_start_penetration` must be false in this mode.

`mode: bounded_penetration` is a third policy, not a release-only exception. It
allows any explicitly named pair while its penetration is within
`max_penetration_m`, including a pair that was not colliding at the first
waypoint. It does not require the pair to separate by the end. Unexpected pairs
or an authorized pair beyond its bound still fail.

All modes first obtain the colliding pair set. Signed penetration/distance is
queried only for pairs already reported colliding, at both pre-dispatch
validation and runtime monitoring. ManyForge does not perform an all-pairs
minimum-clearance scan and does not expose the retired all-pairs clearance
parameter. Its exact v1 spelling remains pinned only in rejection tests and
historical baseline evidence.

RoboPlan diagnostic output may report raw geometry names instead of ManyForge
scene-object IDs. During support-contact discovery and trajectory validation,
ManyForge resolves each raw pair through the active collision-body mapper and
uses the object-normalized pair only when both bodies are known scene objects.
This allows raw names such as an attached object's internal shape index to match
`graspable <-> ground`, while a robot-link collision such as `ground <-> 0`
remains blocking if `0` normalizes to a robot body or cannot be resolved to the
configured supported object.

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
1. Optional `metadata.support_contacts` entries remain as explicit policy
   hints/overrides for known support objects, but ordinary pick/release
   behavior should not require hard-coded program pairs.
1. Trajectory validation temporarily re-enables the discovered support pairs,
   checks the generated path, and restores the planner projection after the
   validation scope.
1. The same validation runs for freshly planned and cached trajectories; cache
   hits skip planning, not validation.

RoboPlan's scene-level pair enablement is confined to its planner projection.
Authoritative pre-dispatch validation and runtime monitoring use the independent
`manyforge_collision` engine with an immutable world-and-policy publication.
That policy contains the support-contact entries, so the runtime query session
tracks the selected signed-penetration, binary-contact, or bounded-penetration
evidence independently of the planner's disabled pair. A future RoboPlan
request-scoped contact API can remove the planner-projection workaround, but it
must not replace or weaken these backend-neutral ManyForge admission checks.

The execution monitor is not a 50 ms "grace period" for support contacts. The
kernel starts monitoring after a trajectory has passed validation and execution
has begun; the default monitor tick period is 50 ms / 20 Hz. The first runtime
observation initializes mode-specific state. Precise release enforces its start
envelope and penetration progress; lightweight release enforces pair-set/count
progress without distance; bounded penetration enforces the pair's absolute
depth bound. Missing or non-finite required penetration evidence fails closed
as an inconclusive runtime query. The planned trajectory must separately have
passed the same policy during authoritative pre-dispatch validation.

This is intentionally narrower than `touch_links`. Touch links describe robot
links that an attached object may touch while grasped. Support contacts describe
world objects that the carried object must leave. Expanding touch links to hide
support collisions is not acceptable because it can move a real carried-object
collision out of the checked world.

## Execution Boundaries

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

RoboPlan's planner projection does not certify numeric penetration. The
independent ManyForge collision engine enforces signed progress and bounded
penetration when selected. RoboPlan request-scoped contact diagnostics remain a
desirable upstream simplification, not a prerequisite for current authority.

## Robot Collision Mesh Preservation and AABB Proxy Warnings

Historical baseline status: active RoboPlan adapter behavior in
`manyforge_roboplan`.

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
- runtime monitoring and pre-dispatch validation use the independent
  `manyforge_collision` engine against immutable authority-published snapshots;
  RoboPlan collision is diagnostic/planner-internal only;
- signed depth/progress is required only for precise release and bounded-
  penetration modes, and only on colliding pairs. Missing required evidence
  fails closed rather than falling back to RoboPlan diagnostics;
- performance depends on snapshot projection/publication plus the selected
  ManyForge collision engine. RoboPlan `hasCollisions(q)` may still guide
  candidate generation or diagnostic comparison but cannot admit dispatch.

## Source Locations

- `manyforge_roboplan/src/roboplan_projection_backend.cpp`
  - `scene_placement_from_tracked_locked`
  - `parent_joint_from_frame_matrix_locked`
  - `apply_static_collision_policy_for_object_locked`
  - `erase_static_collision_policy_for_object_locked`
  - `recompute_support_contact_policy_locked`
  - `set_support_contact_pairs_for_validation_locked`
  - mesh objects are added through RoboPlan's `Scene::addMeshGeometry` API,
    available in the reviewed RoboPlan 0.5.1 commit used by
    `scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`); the exact source and
    checked patch manifest is `third_party/roboplan/README.md`
- `manyforge_roboplan/include/manyforge_roboplan/roboplan_projection_backend.hpp`
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
- RoboPlan exposes structured contact diagnostics, including stable body pairs
  and preferably penetration depth, so its planner projection needs fewer
  temporary pair-policy workarounds. ManyForge remains the dispatch/runtime
  collision authority even when that API exists.
- Regression coverage proves that overlapping detached scene objects do not
  block robot start-state validation, while attached-object and robot-link
  collisions still block planning.
- This document, the source comments, and the corresponding specs-repo
  open-point are updated with the RoboPlan issue or PR reference.

The frame-relative placement conversion can be removed only if RoboPlan exposes
or documents a frame-relative object update/add API that composes
`T_parent_joint_frame * T_frame_object` internally. Until then, ManyForge must
continue to pass parent-joint-relative placements into RoboPlan.
