# RoboPlan adapter notes

Developer and advanced reference. It describes how ManyForge uses RoboPlan for
motion planning, where RoboPlan's own collision checks are used, and which
behavior belongs to ManyForge rather than to RoboPlan.

ManyForge pins RoboPlan 0.8.0 (commit
`2a6b447e648b1a3337f10cbab995cda0c0a71d55`, `ROBOPLAN_PINNED_REF` in
`scripts/setup-docker.sh`), built from source in `Dockerfile.ros-lyrical-full`
with the patches listed in `third_party/roboplan/patches.sha256`. Patches 0016
to 0019 carry the time-scaling behaviour described below (TOPP-RA path-speed
bound, runtime contact-pair toggle, TimeOptimal Cartesian speed bound, curve
speed cap in Bounded Cartesian timing). See
[`third_party/roboplan/README.md`](../../third_party/roboplan/README.md) for
the source closure and what each patch does.

## For users: collision rules

How close two parts may come, and which parts may touch, is set by collision
rules: cell rules in the Deployment and move rules on motion nodes
([Collision rules](../authoring/COLLISION_RULES.md)). When the robot picks an
object that rests on a support (a table, a tray, the ground), the object and
its support may touch at the start of the lift. ManyForge allows that contact
by itself while the carried object moves away from the support. An object
that starts with some overlap, or a place that ends on a surface, needs a
rule on that move, for example on the lift:

```yaml
collision_rules:
  - rule_id: lift_from_ground
    a: [{kind: attached_object, component_id: lite_gripper}]
    b: [{kind: scene_object, scene_resource_id: ground}]
    release: {max_start_penetration_m: 0.03}
```

A rule is not a general "ignore this collision" switch. It holds exactly its
pairs; any other collision still fails validation. How the values are
evaluated is described in [Contact and release rules](#contact-and-release-rules)
below.

An object row may also set `metadata.mass_kg` (kg, 0 to 1000). The mass is
added to the robot's controller payload while the object is attached; it is 0
when absent.

## Who decides collisions

RoboPlan checks every planned path's start, goal, edges and shortcuts against
the retained planner scene and collision policy, keeping the collision
clearance. Before dispatch, `manyforge_collision`, ManyForge's own collision
engine, validates every cached trajectory, every fresh one whose move has a
contact or release rule (pairs the planner switches off, see below) and, by
default, every other fresh one
too (`collision_evidence=pre_dispatch_validation`). That check covers the
trajectory's timed waypoints only, with the same clearance; it samples nothing
between them. A cache hit skips planning, not validation, and a refused
trajectory, cached or fresh, is re-planned within the authoritative-validation
attempts. The robot's Deployment can turn the check off for fresh plans
without a contact or release rule (**Check fresh plans before dispatch**,
`collision_check_fresh_plans`, default on, `kDefaultCollisionCheckFreshPlans`;
cell context, so changing it restarts nothing): such a plan is then dispatched
on the planner's check (`collision_evidence=planning`), and changes to the
scene after planning are caught by the runtime checks during motion. Limit checks (joint, speed) run on every
trajectory. Runtime monitoring runs in the same engine against the published
collision world and policy and decides whether a running motion is stopped.

**Clearance.** Planned and validated moves keep, for every pair, the
clearance of the collision rule that holds the pair. The Deployment's first
rule (**Everything else: clearance**, default 1 mm,
`kDefaultCollisionClearanceM`) holds every pair of a robot body or carried
object and a Scene object. It also covers the executed spline's small
deviation from the planned path between waypoints. A motion's own row without
selections (0 to 0.05 m) replaces it for that move; 0 turns it off
(contact-only checks). Rules with selections set the clearance of exactly
their pairs, robot against robot included. IK, the runtime monitor and the
lookahead check use no clearance.

Validation stays strict in these cases:

- robot links against world objects are always checked, except for the pairs
  of a contact rule;
- a carried object against world objects is checked, except for the pairs of a
  contact or release rule and the automatic release at the start of a move;
- a carried object against robot links is checked, except for the collision
  bodies its attach node lists as `touch_bodies`.

## Planner block

Joint goals and joint pose moves are planned with RoboPlan's RRT. Each
robot's Deployment runtime configuration sets the default (the Deployment
editor's **Planner search**, **Planner optimize**, **Planner N**, **Collision
check step** and **Planner node budget**); a motion node's optional `planner`
parameter overrides single members for that move (unset members keep the
Deployment's). Cartesian paths and linear pose moves do not search: Follow
Cartesian path ignores the block, and a linear pose move refuses a `planner`
parameter.

| Member | Deployment field | Values (default) |
| --- | --- | --- |
| `search` | `planner_search` | `rrt`, `rrt_connect` (`rrt_connect`) |
| `optimize` | `planner_optimize` | `first`, `rrt_star`, `best_of_n` (`first`) |
| `n` | `planner_n` | 2 to 32 plans for `best_of_n` (4) |
| `collision_check_step_rad` | `collision_check_step_rad` | (0, 0.2] rad (0.05) |
| `max_nodes` | `planner_max_nodes` | 100 to 1,000,000 tree nodes (5000) |

`first` returns the first path found. `rrt_star` rewires the tree and spends
the node and time budget on the cheapest path. `best_of_n` plans `n` times
with distinct seeds (the Deployment seed plus the run index) and keeps the
shortest path after shortcutting; the `n` plans share the move's planning
timeout (a plan that starts with none left is skipped). A re-plan after a
refused trajectory uses the finer of `collision_check_step_rad` and 0.01 rad.

## Cartesian paths

A Follow Cartesian path move traces straight segments between its waypoints.

**Linear pose moves.** Move to pose, Move until signal and Probe search have a
`motion_type`: `joint` (default) solves IK for the goal and plans in joint
space, so the tool does not move on a line; `linear` moves the tool on the
straight line from where the move starts to the goal. The planning step takes
the tool pose by forward kinematics from the start state it plans from (the
robot's observed state when the move is planned, also when a stopped move is
resumed), so no waypoint is authored for the start. The line is planned
exactly as a two-waypoint Cartesian path below: RoboPlan's Cartesian planner
follows it within the move's tolerances, with no collision-avoiding deviation
and no joint-space fallback. A line that cannot be followed fails with the
Cartesian errors (for example `cartesian_path_tolerance_exceeded` or
`cartesian_tracking_stalled`); collisions are checked as for every Cartesian
path (see [Who decides collisions](#who-decides-collisions)). A linear move takes
`max_translation_step_m`, `max_rotation_step_rad`, `max_angular_speed_rad_s`,
`max_linear_acceleration_mps2`, `max_angular_acceleration_rad_s2` and
`cartesian_timing` with Follow Cartesian path's defaults; a joint move refuses
them, and a linear move refuses `planner`. A relative Move to pose
(`target_reference`, `offset_reference`) resolves its goal as a joint one
does.

A linear move that takes over a robot that is still moving is joined to the
running motion like a joint move: the line is planned from the switch point,
and a quintic connector runs from the robot's moving state onto the line. The
connector is accepted only when it is proven within the robot's joint
position, velocity, acceleration (and set jerk) limits, as for joint moves,
and when every sampled connector pose, plus the motion still possible between
samples, lies within the move's `translation_tol_m` and `rotation_tol_rad` of
a pose on the line (position and orientation interpolated at one parameter).
Otherwise planning returns `native_moving_cartesian_blend_proof_unavailable`
and the takeover falls back to a spring-back or a Stop, after which the line
starts from rest (see [Motion takeover planning
budget](#motion-takeover-planning-budget)). The joined trajectory starts with
the running motion's prefix, so it carries no Cartesian waypoint milestones or
fidelity metrics. A Follow Cartesian path never takes over a robot that is
still moving: the Kernel refuses it at admission with
`cartesian_path_moving_takeover_refused`, and the running motion continues
(see [Interrupts](../authoring/INTERRUPTS.md)).

**Corners.** A waypoint's corner is exact by default: the tool stops there
when the path turns by more than about 10° (RoboPlan's corner tolerance), or
when the rotation per metre changes sharply. A blend radius rounds it: the
node's `blend_radius_m` (metres, default 0) applies to every waypoint, and a
waypoint's own `blend_radius_m` overrides it. The rounded corner is the
circular arc tangent to both segments, starting and ending at the blend radius
from the waypoint, clamped to half of each adjacent segment so neighbouring
corners never overlap. The orientation changes smoothly across the arc, so the
tool runs through it without stopping and does not pass through the waypoint
itself. Radius 0, a straight-through waypoint and a full reversal keep the
exact corner. The Program's `blend_radius_m` setting is the default for new
moves. The radius is applied by ManyForge before RoboPlan sees the path
(`densify_cartesian_path`), so it works the same for both timings below.
With `constant_speed`, RoboPlan resolves the path at half the move's
translation and rotation tolerances. When a corner is rounded, ManyForge
tightens both for the resolution by one common factor so the tightest arc is
resolved at least every twentieth of its radius (0.5 mm on a 10 mm arc, the
same 0.05 rad of turn per step as the arc's own points); the joint
path then follows the arc instead of a coarse polygon. The finer step applies
to the whole move, so planning resolves more points on the straight segments
too. The path is still checked against the move's own tolerances.

**Timing.** `cartesian_timing` picks how the path is timed:

| Value | Timing |
| --- | --- |
| `constant_speed` | RoboPlan Bounded timing: the tool speeds up, holds 0.98 of the TCP linear (and angular) speed limit along each smooth run and slows down; it stops at exact corners. On a rounded corner it slows only as far as the curve needs (lateral acceleration v²/r within half the move's linear acceleration limit, patch 0019). For process paths. |
| `time_optimal` | RoboPlan TimeOptimal timing (TOPP-RA): as fast as the joint limits allow; the tool speed varies along the path and is bounded at 0.98 of the TCP linear and angular speed limits (patch 0018). For transfer moves. |

A move without `cartesian_timing` uses its robot's Deployment runtime
configuration `cartesian_timing` (Deployment editor **Cartesian timing**,
default `constant_speed`). Both timings keep the path within the move's
translation and rotation tolerances; a path that cannot be held fails with
`cartesian_path_tolerance_exceeded`.

## Time-scaling and the TCP speed limit

The TCP speed limit is checked on the active TCP only and is never raised by
ManyForge (the robot controller enforces its own limit at that TCP).

- **Joint goals and joint pose moves:** timed by the robot's Deployment `motion_timing`
  (Deployment editor **Motion timing**, default `smooth`; one value per
  robot, no per-move override):

  | Value | Timing |
  | --- | --- |
  | `smooth` | TOPP-RA times the path within the full joint limits, without the TCP bound; the final uniform stretch below then slows the whole move until the active TCP is at 0.98 of its linear speed limit at the samples. Gentle starts and stops; slower. |
  | `fast` | TOPP-RA times the path within the nominal acceleration share, with a per-grid-point path-speed bound that holds the active TCP at 0.98 of its linear speed limit (`kTcpSpeedTargetFraction`, patch 0016). Only the stretches near the limit slow down; the acceleration switches abruptly there (Lite 6 peak joint jerk in the hundreds to thousands of rad/s³ without jerk limits). |

- **`time_optimal` Cartesian paths:** the `fast` bound on TOPP-RA's grid, for
  both the linear and the angular TCP limit (patch 0018). Cartesian paths and
  linear pose moves follow `cartesian_timing` only; `motion_timing` does not
  apply to them.
- **Check:** for joint and pose goals and `time_optimal` paths, the TCP speed
  is checked at the trajectory samples only. With the bound inside TOPP-RA, a
  sample above the limit stretches the whole trajectory uniformly back to the
  0.98 target; with `smooth`, the stretch brings the sample peak to the 0.98
  target. Exceedances between samples are accepted; the robot's own
  controller enforces its limit.
- **Known limitation (`time_optimal` Cartesian):** TimeOptimal Cartesian timing
  works on the piecewise path without the smooth fit TOPP-RA uses for joint
  moves, so the controller's interpolation between the 10 ms samples can
  overshoot the planned TCP speed by up to about 1.4× (joint moves: about
  1.06×), briefly above the vendor TCP limit when the samples are at the 0.98
  target. `constant_speed` (the default) stays well below the limit. Use
  `constant_speed` where the TCP limit must hold between samples; after 0.2.0 we
  evaluate a fix: our own RoboPlan patch giving `time_optimal` the same smooth
  fit, or a fix requested upstream.
- **`constant_speed` Cartesian paths:** Bounded timing cruises at 0.98 of the
  limits; the peak tool speed and acceleration are then certified over the
  whole trajectory (every piece is sampled first to set the peak, then refined
  to within 2%), and a ratio above 1 stretches the trajectory uniformly.

Every trajectory then gets one final uniform stretch, if needed, for the TCP
check above and the joint limits with a 20% acceleration and jerk reserve for
live speed scaling (`kNominalScalingHeadroom`). When the motion sets per-joint
jerk limits (`per_joint_dynamics_limits`, `max_jerk_rad_s3`), that stretch
also brings the peak jerk within 80% of the limit, with either motion timing.
RoboPlan itself has no jerk-limited timing.

## Planner scene

Planning runs in a separate candidate-worker process
(`roboplan_candidate_worker_main.cpp`), started by
`process_contained_candidate_client.cpp`. Each request has a deadline; a
cancelled or overdue request ends the worker with `SIGKILL` through its pidfd,
and the next request starts a fresh worker. The worker builds its RoboPlan
scene from the published ManyForge projection and keeps it until the projected
scene changes:

1. it adds every collision geometry to the scene;
2. it disables, with `Scene::setCollisions(a, b, false)`, the pairs that the
   Scene's collision policy allows: fixed-object overlap pairs and the grip
   pairs of the current attachments (pairs the composed SRDF
   disables, including mount exclusions, are never checked; see
   [Mount collision exclusions](#mount-collision-exclusions));
3. it places each geometry in its frame (see the next section).

The pairs of the move's contact and release rules travel with each plan and
IK request, like the clearance margins (per-pair margins, RoboPlan patch
0015). The worker switches them off for that request with
`Scene::setCollisionPairsActive` (RoboPlan patch 0017) and switches the
previous request's pairs back on, so a change of rules keeps the worker, the
pair order and the margins. Depth limits and the end-of-move fraction are not
planning constraints: pre-dispatch validation decides them.

### Frame-relative placement

ManyForge stores an object pose relative to the frame named in its state. A
fixed object usually uses a cell or component frame; a carried object uses a
robot frame such as a grasp frame or a link frame.

The worker passes that frame-relative pose and the frame name to
`Scene::updateGeometryPlacement`. RoboPlan composes the frame's fixed offset
from its parent joint itself (Pinocchio evaluates `GeometryObject::placement`
in the parent joint), so a grasp frame offset from the gripper base joint
places the geometry correctly.

This conversion matters for correctness. Without it, a carried object could be
planned at a different pose from the one Composer shows. That can fail closed,
as a false gripper/object collision, or fail open, as a missed collision
between the carried object and an obstacle.

## Mount collision exclusions

When a tool component is mounted on a robot (a Deployment `attachments[]`
row), the composed cell SRDF disables every pair between the two rigid link
groups that meet at the mount (reason `Adjacent`). Pairs further from the
mount that can never touch, for example the gripper fingers against the arm's
`link4`, are listed on the attachment row:

```json
"collision_exclusions": [
  {"child_link": "lite_gripper_left_finger", "parent_link": "link4"}
]
```

`child_link` is a link of the tool's model, `parent_link` a link of the
parent's model. Composition disables each pair like the mount pairs (reason
`Never`), so neither the planner nor `manyforge_collision` checks it. Apply
checks only that each link exists (`component_attachments` otherwise); a
wrong pair hides a real collision. The Components page edits them in the
mounted tool's **Collision exclusions with parent** field; the Lite 6 and
UF850 examples carry the pairs for their grippers. See
[`authoring/GRIPPERS.md`](../authoring/GRIPPERS.md).

## Fixed-object overlap

Two fixed scene objects may touch or overlap in the authored scene, for
example a product mesh that slightly penetrates a table at import. That
overlap should not make the robot's state invalid, because neither object
moves.

This is a general ManyForge collision-policy rule, not a RoboPlan setting.
The effective collision policy names the static bodies of the collision world
(`static_body_ids`), and two static bodies are never checked against each
other: one rule by body class, as MoveIt's default allowed-collision entries
are, not a list of pairs. When the effective collision policy is projected for
a logical-scene revision
(`RetainedCollisionWorldProjector::project_effective_policy`), the static
bodies are:

- every collision shape of a fixed (not carried) scene object;
- every static component body, that is, a robot or equipment body that no
  moving joint carries (a robot base or pedestal, for example).

The list grows with the number of bodies, not of pairs: 200 fixed objects are
200 entries. The policy is recomputed for each logical-scene revision. When an
object is attached to the robot it is carried, not static, so its pairs are
checked again; when it is released it becomes fixed again. A static body
against a moving robot link, a carried object or a runtime object is always
checked. `manyforge_collision` and the planner scene never check a pair of two
static bodies, since neither belongs to the moving group; a validation or
monitor result that reports one classifies it `allowed_static_static`.

## Contact and release rules

At admission the execution kernel resolves the Deployment's cell rules and the
move's rules against the move's collision world, once
(`resolve_collision_rules`). The result has two parts: the clearance margins
(pair, clearance, rule) that the planner and the validation queries use, and
the pairs under a contact or release rule (pair, rule, value), which the kernel
installs into the move's collision policy next to the Scene projector's
static bodies and grip pairs. A pair that two cell rules with selections
or two move rules hold is refused (`collision_rules_overlap`); a selection
that names nothing in the world is refused
(`collision_rule_selection_unresolved`), except `attached_object`, which may
be empty.

The execution host resolves a cell rule's `collision_body` and `model_group`
selections to the physical collision bodies of the cell before the kernel
sees them: a model group selects the bodies whose links its expanded SRDF
group (as the component's command target uses it) holds. The Manager checks
cell rules for overlapping pairs again when it previews and applies a
Deployment (`cell_collision_rules_overlap`, by link, with carried objects and
Scene objects as symbols), and lists the rule changes in the Apply impact's
`collision_rules`.

A rule on a carried object applies only while the object is carried. Before
the attach step the object and its support are both fixed, so their contact is
covered by the fixed-object rule and an approach motion does not have to
separate them.

**Automatic start release.** The execution kernel finds contacts between a
carried object and a fixed scene object at the start of a motion and adds them
as release pairs with default settings (`execution_kernel.cpp`,
`discover_dynamic_carried_start_contacts` and
`dynamic_carried_start_contact_pairs`; rule id `automatic_start_release`). It
does this before planning, from the observed start state, and again after a
failed validation, from the trajectory's first waypoint, before validating
once more. A pair that a contact or release rule already holds is left to
that rule. The defaults are strict: contact at the start is allowed only as
touching (zero start penetration), and the pair must separate monotonically
and stay separated. Discovery fails the motion if authored plus discovered
release pairs would exceed 64 pairs.

### `release`

- the pair may be in collision at the first validation waypoint;
- start contact only makes the pair eligible for release; it does not assume
  the next motion will separate it;
- a pair that starts clear may not come into contact, and no other collision
  pair is allowed because of the rule;
- with `require_separation_by_end: true` (default) the pair must be separated
  at the final waypoint;
- with `allow_recontact: false` (default) contact after the first separation
  fails;
- with `enforce_max_start_penetration: true` (default),
  `max_start_penetration_m` (default 0) bounds the first-waypoint depth.
  Setting it to `false` disables only that start check, not the release,
  separation or re-contact checks.

`release_evidence_mode` selects how progress is proved:

- `signed_penetration` (default, **Precise release**): for each rule pair
  that is colliding, the engine measures penetration depth.
  `release_tolerance_m` (default 1e-5) is the allowed worsening between
  samples, `min_release_progress_m` (default 0) the required total reduction
  when `require_strictly_decreasing_penetration` (default true) is set.
- `binary_contact` (**Lightweight release**): checks only which pairs collide.
  It permits no new pair, requires the contact evidence to decrease, and
  measures no depth. The depth fields do not apply: leave them unset or
  neutral; other values are rejected.

### `contact`

- With `end_from_fraction: 0` the pair may collide at any sample of the move,
  including the first, and does not have to separate. With
  `max_penetration_m` every colliding sample must stay within that depth;
  without it the depth is not checked.
- With an `end_from_fraction` above 0 (0.8 when the rule gives none) the pair
  may collide only from that fraction of
  the move on: a collision at the first sample (`initial_sample_in_contact`)
  or before the fraction (`grasp_contact_before_terminal_window`) is refused.
  The fraction is taken from the path's own fractions when the trajectory has
  them, else from the sample index.
- A depth above the limit is refused with `contact_penetration_exceeded`
  (rule, pair, depth, limit). Unexpected pairs still fail.

### Validation and monitoring

The same pairs and values are used for pre-dispatch validation and for runtime
monitoring. Signed distance is requested only for pairs whose rule needs it (a
depth limit, or a precise release) and that are already reported colliding;
missing or non-finite required depth fails closed as an inconclusive result.

The runtime monitor starts after the trajectory has passed validation and
execution has begun. Its default tick is 50 ms (20 Hz,
`ExecutionKernelConfig::collision_monitor_period_ns`); a Deployment can set
another value with its **Collision monitor period**
(`collision_monitor_period_ms`). It is not a grace
period for release pairs: the first observation initializes each pair's
state, then precise release enforces its start bound and penetration progress,
lightweight release enforces pair-set progress, and a contact rule with a
limit enforces the depth. The monitor does not apply `end_from_fraction`; the
look-ahead check applies it together with the depth.

Rules on a carried object and its support are narrower than the attach node's
`touch_bodies`. `touch_bodies` name gripper bodies that a carried object may
touch while grasped. A release or contact rule names a world object that the
carried object must leave or may rest on. Using `touch_bodies` to hide a
support collision would remove real carried-object collisions from the check.

## Robot collision geometry

Robot and equipment collision bodies come from the model's URDF `<collision>`
geometry as loaded by Pinocchio and Coal. The exported descriptors keep the
loaded shape kind and carry quality labels such as `primitive`, `convex`,
`collision_mesh` or `backend_compound`. There is no automatic box (AABB)
fallback for robot links. Turn on **Collision geometry** in the Composer
viewer to see the geometry that is checked.

## Motion takeover planning budget

A Deployment that sets `policies.motion_takeover_preparation_budget_s` allows
a new motion to take over a running one as a moving replacement. Planning the
replacement is capped by `policies.motion_takeover_planning_budget_s`, or by
4/5 of the preparation budget when that is absent; the rest is left for
validation, the join and controller delivery. A takeover that cannot complete
as a moving replacement falls back to a spring-back or a Stop. The Program
runtime panel then shows a warning with the planning cap that applied, for
example "Spring-back fallback on move · planning timeout during
spring_back_planning (planning cap 400 ms · prep 534 of 500 ms)".

## Current limits

- Validation checks trajectory waypoints; there is no continuous
  (swept-volume) collision check.
- Signed depth is computed only for precise release and for contact rules
  with a depth limit, and only for colliding pairs.
- Performance depends on projection and publication of the collision world
  plus the ManyForge engine; RoboPlan's own checks cannot admit a dispatch.

## Source locations

| Topic | Location |
|---|---|
| RoboPlan pin and carried patches | `third_party/roboplan/README.md`, `third_party/roboplan/patches.sha256`, `scripts/setup-docker.sh` (`ROBOPLAN_PINNED_REF`) |
| Planner block types and ranges | `manyforge_core/include/manyforge_core/types/planner_settings.hpp`, `manyforge_roboplan/include/manyforge_roboplan/roboplan_planner_options.hpp` |
| Robot and cell runtime defaults (planner block, clearance, check step, fresh-plan check, Cartesian timing, Real Stop lead-in, TCP offset sync; C++ and the GUI's "Default") | `manyforge_core/resources/robot_defaults.tsv`, generated by `manyforge_core/tools/generate_robot_defaults.py` (`scripts/regen.py`) |
| RRT, best_of_n, Cartesian timing and TCP time-scaling | `manyforge_roboplan/src/roboplan_candidate_computer.cpp` (`densify_cartesian_path`, `prepare_candidate_time_law`) |
| Linear pose moves (start pose by FK, two-waypoint line) | `manyforge_core/include/manyforge_core/types/motion_spec.hpp` (`LinearPoseMotionDTO`, `linear_pose_path`), `manyforge_roboplan/src/roboplan_candidate_computer.cpp` (`compute`) |
| Pre-dispatch validation and the fresh-plan check setting | `manyforge_core/src/execution_kernel.cpp` (`collision_check_fresh_plans`, `collision_checked_by_planning`) |
| Mount collision exclusions | `manyforge_deployment/src/cell_composition.cpp`, `compose_cell_occurrences` |
| Scene collision policy (static bodies, grip pairs of the attachments) | `manyforge_roboplan/src/retained_collision_world_projector.cpp`, `RetainedCollisionWorldProjector::project_effective_policy` |
| Collision rule types, resolution at admission | `manyforge_core/include/manyforge_core/types/collision_rule.hpp`, `manyforge_core/include/manyforge_core/collision/collision_rule_resolver.hpp` (`resolve_collision_rules`) |
| Static component bodies | `manyforge_collision/src/manyforge_collision_engine.cpp`, `select_retained_static_component_bodies` |
| Allowed pairs sent to the worker | `manyforge_roboplan/src/process_contained_candidate_client.cpp`, `project_worker_model`, `native_contact_pairs` |
| Planner scene build, `setCollisions`, frame-relative placement, per-request contact pairs | `manyforge_roboplan/src/roboplan_candidate_worker_main.cpp`, `make_scene`, `apply_contact_pairs` |
| Start/goal `hasCollisions` checks | `manyforge_roboplan/src/roboplan_candidate_computer.cpp` |
| Start-contact discovery | `manyforge_core/src/execution_kernel.cpp`, `discover_dynamic_carried_start_contacts`, `with_dynamic_carried_start_contacts` |
| Contact and release values and defaults | `manyforge_core/include/manyforge_core/types/collision.hpp` (`CollisionContactRule`, `CollisionReleaseRule`, `CollisionPairContactRule`) |
| Release and contact evaluation | `manyforge_core/src/collision_policy_evaluator.cpp` |
| Monitor period | `manyforge_core/include/manyforge_core/execution_kernel.hpp` |
| Composer rule table | `manyforge_composer/frontend/src/components/CollisionRulesTable.tsx` |
| Example | `examples/sim_lite6_foundationpose_trocar_pick.scenario/program.yaml` |

## Upstream simplifications

These RoboPlan features would let ManyForge drop adapter code. They would not
change the collision authority, which stays with `manyforge_collision`.

- A per-request collision policy, equivalent to MoveIt's Allowed Collision
  Matrix for one query, instead of scene-level `setCollisions` on a
  per-request scene.
- Structured contact diagnostics with stable body pairs and penetration depth.
