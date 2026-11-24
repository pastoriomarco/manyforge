# Collision rules

ManyForge checks every planned and validated move for collisions. A
**collision rule** says how close two parts may come, or that they may touch.
This page describes the rules, where they are written, and what is checked
without any rule.

## Where rules are written

| Rules | Where | They apply to |
| --- | --- | --- |
| **Cell rules** | The Deployment: `collision_rules`, the **Collision rules** section of the Deployment editor | Every move in the cell |
| **Move rules** | A motion node: its `collision_rules` parameter, the **Collision rules** table in the node's properties | That move only |

Move rules exist on Move to joint target, Move to pose, Follow Cartesian
path, Move until signal and Probe search, and on the planning nodes Plan pick
sequence and Plan place sequence. A Program has no rules of its own: a rule
that should hold for one move is written on that move.

No node needs a rule. A pick or a probe that should touch its object gets a
contact rule on the move that touches it; without one, the contact is refused
like any other collision.

## A rule

```yaml
collision_rules:
  - rule_id: everything            # the row without selections: clearance only
    clearance_m: 0.001
  - rule_id: tip_on_part
    a: [{kind: active_tcp_contact_bodies}]
    b: [{kind: scene_object, blackboard_key: trial.probe_object}]
    contact: {max_penetration_m: 0.002, end_from_fraction: 0.8}
```

A rule has an id (unique in its list), two selections `a` and `b`, and one
value. It holds every pair made of one part from `a` and one part from `b`.
A move rule may name a spring slider in place of `a` and the value
([Springs](#springs)).
A list has at most 64 rules; `a` and `b` each have 1 to 16 selections.

### Values

A rule has exactly one of these values:

| Value | Composer | Meaning |
| --- | --- | --- |
| `clearance_m` | **Clearance** | The two parts stay at least this far apart, 0 to 0.05 m. 0 means they may come arbitrarily close but not touch. |
| `contact` | **May touch** | The two parts may touch. |
| `release` | **Release (starts in contact, must separate)** | The two parts touch when the move starts and must come apart during it. |

`contact` has two optional settings:

- `max_penetration_m` (**Deepest penetration**): how deep the modelled parts
  may overlap. Without it the depth is not checked.
- `end_from_fraction` (**Only at the end of the move**, 0 to 1, default 0.8):
  the parts may touch only from this fraction of the move on. Contact at the
  start of the move or before the fraction is refused. A rule without it uses
  0.8. `end_from_fraction: 0` (the box cleared) means the parts may touch
  during the whole move, from its start.
- `max_compression_step_m`: the pair is a spring; see [Springs](#springs).
  It needs `max_penetration_m`.

### Springs

A spring-loaded part, for example a rod pushed in by the part it holds, is
modelled at rest: its compression is the overlap of the parts. The move that
presses it in carries a rule that allows that overlap.

A generated tool's `spring_slider`
([Generated tools](ADDING_A_COMPONENT.md#parameters)) is named by a move
rule in place of `a` and the numbers:

```yaml
  - rule_id: tube_into_endplate
    spring_slider: {component_id: tube_holder, element_id: holder_rod}
    b: [{kind: scene_object, scene_resource_id: endplate}]
    contact: {end_from_fraction: 0}
```

- The a side is the slider's collision body and every object the slider
  carries (attached to the slider's frame).
- The slider and its carried objects are one spring: its compression is the
  deepest overlap of any of them with the b side.
- `max_penetration_m` is the slider's `stroke_m`, `max_compression_step_m`
  its own. Both come from the tool.
- `contact` is optional and may hold only `end_from_fraction` (default as
  for a `contact` rule). `b` is required.
- Move rules only. A component that is not a generated tool, or an element
  that is not a spring slider, refuses the move
  (`collision_rule_spring_slider_unresolved`).

Any other spring part has a `contact` rule on the pair with
`max_penetration_m` = the spring's stroke and `max_compression_step_m`, the
largest increase of the overlap between two samples. Each pair is then its
own spring:

```yaml
  - rule_id: probe_on_part
    a: [{kind: component, component_id: probe}]
    b: [{kind: scene_object, scene_resource_id: part}]
    contact: {max_penetration_m: 0.008, max_compression_step_m: 0.002, end_from_fraction: 0}
```

A move with a spring rule is checked at samples at most 1 mm of tool travel
apart: ManyForge splits every segment of the path into the same number of
steps, so each waypoint keeps its fraction of the move. From one sample to
the next the compression may grow by at most the step limit, also where the
parts first touch; it may shrink freely. Pressing straight in and sliding
over a flat face stay within it; a step or an obstacle on the path makes the
compression jump and the move is refused (`contact_compression_step_exceeded`).
The look-ahead applies the same limit, scaled by how many 1 mm steps lie
between its samples. The direction of the press is not checked.

A spring whose part, pushed back, would leave the tool's own envelope gets
a fixed collision body over its whole stroke: `swept_space: true` on a
generated tool's `spring_slider`
([Generated tools](ADDING_A_COMPONENT.md)). That body is an ordinary
collision body, checked against everything like the tool body; it is not
part of the spring rule, so it must not reach the part the spring presses.

While a move or the monitor of the current position checks a spring rule
and the parts overlap, ManyForge publishes the compression with the robot's
live state: the rule, the a side's body, the depth and the direction that
pushes it back. The 3D viewer draws the a side's parts (a component's link,
or a carried object) shifted back by that depth:

- under a pair rule, each overlapping a side body by its pair's depth, out
  of the b side;
- under a slider rule, the slider and every object it carries, touching or
  not, together by the spring's compression, against the slider's axis.

Only the drawing moves: collision geometry, overlays and checks keep the
overlap model. When the parts are apart, or no move or monitor checks them,
nothing is shifted.

`release` has these settings; each has the default shown:

| Setting | Default | Meaning |
| --- | --- | --- |
| `release_evidence_mode` | `signed_penetration` | `signed_penetration` (**Precise release**) measures how deep the parts overlap. `binary_contact` (**Lightweight release**) only checks whether they touch; the depth settings below then do not apply and must be left at 0 or `false`. |
| `enforce_max_start_penetration`, `max_start_penetration_m` | `true`, 0 | The deepest overlap allowed at the start of the move. `false` turns only this check off. |
| `release_tolerance_m` | 0.00001 | How much deeper one sample may be than the one before. |
| `require_strictly_decreasing_penetration`, `min_release_progress_m` | `true`, 0 | The overlap must shrink, by at least this much in total until the parts are apart. |
| `require_separation_by_end` | `true` | The parts must be apart when the move ends. |
| `allow_recontact` | `false` | Whether the parts may touch again after they came apart. |

A pair under a release rule that does not touch at the start may not come
into contact during the move.

Rule values are plain numbers. They are not expressions and cannot read the
blackboard.

### Selections

| `kind` | Composer | Selects | Other keys |
| --- | --- | --- | --- |
| `component` | **Component** | Every collision body of a component | `component_id` |
| `collision_body` | **Collision body** | One collision body of a component, for example `gripper_collision` | `component_id`, `collision_body_id` |
| `link` | **Link** | One link, by its name in the cell model. The Composer lists the links of the chosen component once the cell is applied | `component_id`, `link` |
| `model_group` | **Model group (SRDF)** | The links of one SRDF group of a component's model that carry collision bodies, for example the arm's planning group. Deployment rules only | `component_id`, `group` (the group's name in the model's SRDF, one of the component's planning groups) |
| `attached_object` | **Object carried by** | Every Scene object the component carries at that moment; with `collision_body_role_id`, only the objects carried by that collision body, for example a spring slider | `component_id`, optional `collision_body_role_id` |
| `scene_objects` | **All Scene objects** | Every Scene object that is not carried | none |
| `scene_object` | **Scene object** | One Scene object. Move rules only | `scene_resource_id`, or `blackboard_key` (a key that holds a Scene object; `field` names one field of a structured value) |
| `active_tcp_contact_bodies` | **Contact bodies of the move's TCP** | The contact bodies of the TCP the move uses ([TCPs](TCPS.md#contact-bodies)). Move rules only | none |

A model group holds the links it has in MoveIt: the links it lists, the
child link of every joint it lists, the links of its chains and the links of
its subgroups. A group that lists joint1 to joint6 of an arm therefore holds
link1 to link6, but not the base link.

A component, collision body, link or Scene object that the cell does not
have refuses the move (`collision_rule_selection_unresolved`); a model group
the component's model does not have, or whose links carry no collision body,
refuses the Apply with the same code. An
`attached_object` selection may select nothing: the rule then holds no pair
until the component carries an object.

## The first rule

The Deployment's first rule has no selections (**Everything else:
clearance**, `rule_id: everything`). It holds every pair of a robot part and
a Scene object, where a carried object counts as part of the robot that
carries it. Its value is a clearance, 1 mm in a new Deployment. The clearance
also covers the small deviation of the executed path from the planned one
between waypoints. 0 turns it off: moves are then refused only when parts
touch.

The first rule does not hold pairs between two robot parts (a robot's own
links, two robots, a robot and its tool). Those are checked for contact, with
no clearance, unless a rule with selections gives them one.

A robot checks only pairs with at least one of its own parts: the links its
joints move, the tools mounted on them and the objects it carries. Planning,
the start-state check and the look-ahead of its moves leave every other pair
to the robot that owns those parts. For example, when one robot presses the
tube it carries into a Scene object under a contact rule of its own move, a
second robot's next move is not refused for that overlap. A pair of one of the
robot's own parts and anything else, another robot's links or carried object
included, is checked as usual.

A move may have its own row without selections. It replaces the first rule's
clearance for that move. Left out, the move keeps the Deployment's value.

## How cell rules and move rules combine

When a move is admitted, ManyForge works out one value for every checked pair,
once, from the parts that exist at that moment:

1. each pair gets the value of the cell rule that holds it;
2. a move rule with selections replaces the value of exactly its pairs;
3. the move's row without selections replaces the value of the pairs the
   first rule still holds.

A move rule replaces the whole value: a contact rule on a pair removes its
clearance for that move. Two move rules that hold the same pair refuse the
move (`collision_rules_overlap`; the message names the pair and both rules).

Two cell rules with selections that hold the same pair are refused already
by the Deployment Preview and Apply, with the same code. There the members
are compared by link; the object a component carries and the Scene objects
are compared as names ("object carried by `gripper`", "Scene objects"), since
which objects they are is known only per move.

## Preview and confirm

The Deployment Preview lists every cell rule the Apply adds, removes or
changes, with the members of its two selections resolved against the cell
model before and after the Apply: for example "now `gripper/left_pad`,
`gripper/right_pad` ↔ object carried by `gripper`; was ...". A rule whose
authored row stays the same is listed too when its members change, for
example when a component's model changes.

Confirming the Preview confirms these rules. The Apply request carries their
ids in `confirmed_collision_rule_ids` next to the Preview's `scope`; an Apply
whose Preview changes a rule the list leaves out ends with
`collision_rule_confirmation_required` before any component changes. Loading
a Scenario is its own confirmation and needs no list.

## Rules on pick and place sequences

On Plan pick sequence and Plan place sequence each rule also names its
`leg`: `approach`, `descend` or `retreat`. The rule applies to that leg only.
For a pick, the lift is the retreat leg.

```yaml
collision_rules:
  - rule_id: fingers_on_part
    leg: descend
    a: [{kind: collision_body, component_id: lite_gripper, collision_body_id: gripper_collision}]
    b: [{kind: scene_object, scene_resource_id: graspable}]
    contact: {max_penetration_m: 0.002, end_from_fraction: 0.8}
  - rule_id: lift_from_ground
    leg: retreat
    a: [{kind: attached_object, component_id: lite_gripper}]
    b: [{kind: scene_object, scene_resource_id: ground}]
    release: {max_start_penetration_m: 0.03, release_tolerance_m: 0.001}
```

The sequence plans its legs with these rules. The motion nodes that later
execute the planned poses are moves of their own: a move that touches or
lifts needs the same rule on its own node.

A pick allows no contact by itself. Without a `contact` rule on the descend
leg the gripper may not touch the part, and a pick whose gripper must touch
it before the attach is refused as a collision.

## What needs no rule

- **Pairs the models never check.** Pairs a robot's or tool's SRDF disables
  and the `collision_exclusions` of an attachment are not checked at all, and
  no rule changes that.
- **Bodies that never move, against each other.** Scene objects that are not
  carried and the component bodies no joint moves (a robot base or pedestal,
  for example) are static: two static bodies are never checked, and no rule
  gives them a value. A static body against a moving link, a carried object
  or a runtime object is checked as usual.
- **The gripper and the object it holds.** The attach node (Attach carried
  object) lists in `touch_bodies` the collision bodies of the carrier frame's
  component (the gripper, or a tool mounted on the robot) that may touch the
  object. This holds from the attach to the detach and is part of
  the attachment, not a rule.
- **Backing away.** A pair that is closer than its clearance when the move
  starts may move apart; it may not come closer.
- **A carried object resting on something at the start.** When a carried
  object touches a Scene object at the start of a move, ManyForge allows that
  contact as a strict release: touching without overlap, coming apart
  steadily, apart at the end, no new contact. A contact or release rule on the
  pair replaces this.

An object that rests in or on a holder with some overlap, or that must stay
in contact while it is pulled out, needs a rule on the move that lifts it: a
`release` rule with a starting depth, or a `contact` rule with a depth limit
and `end_from_fraction: 0` (the whole move). A place that ends on a surface
needs a `contact` rule on the last place move; later moves keep the normal
clearance to that surface.

## During the move

The runtime collision checks stop a running move when parts touch that no
rule and no attachment allows to touch. They use no clearance. For a contact
rule with a depth limit they also check the depth; for a release rule they
follow the release. The look-ahead check also applies a contact rule's
"only at the end" fraction and a spring's step limit; the monitor of the
current position checks a spring's depth only.

### Depth between two meshes

The depth of a pair is how far one part is inside the other. When both parts
are triangle meshes, it is measured from the vertices of each mesh that lie
inside the other, so both meshes must be closed surfaces: every edge shared by
two triangles, all wound the same way. A mesh exported inside-out is turned
the right way when it is loaded. The precision follows the triangulation: the
depth is exact at the vertices and can read slightly low between them, never
high; triangulate finer where the depth matters. A pair with a primitive shape
on at least one side is measured exactly, as before.

A depth that needs a mesh that is not closed, or that would need more than
256 vertices of one mesh inside the other, is not measured: the move stops
with the reason `mesh_not_closed` or `mesh_depth_vertex_cap_reached` in its
details. Plain touching and clearance checks still work with such a mesh.
The Deployment Preview lists every cell rule with a depth (a depth limit, a
spring or a precise release) and every Mock input with `trip_depth_m` that
selects a mesh that is not closed, with the mesh; the Apply is not refused.

## When a move is refused

| Code | Meaning |
| --- | --- |
| `validation_collision` with `classification: clearance` | The path comes closer than the pair's clearance. The details name the pair, the distance and the rule (`collision_rule_id`). |
| `validation_collision` | Parts touch that nothing allows to touch. |
| `contact_penetration_exceeded` | A pair under a contact rule overlaps deeper than `max_penetration_m`. The details name the rule, the pair, the depth and the limit. |
| `contact_compression_step_exceeded` | A spring's overlap grew by more than `max_compression_step_m` from one sample to the next. The details name the rule, the pair, the depth and the limit. |
| `validation_collision` with `reason: spring_sample_budget_exceeded` or `spring_sample_densification_failed` | A move with a spring rule needs more than 100 000 samples, or a tool position for its samples is unavailable. |
| `initial_sample_in_contact` | A pair under a contact rule with an end fraction above 0 already touches at the start of the move. |
| `grasp_contact_before_terminal_window` | Such a pair touches before the fraction is reached. |
| `collision_rules_overlap` | Two rules hold the same pair. For cell rules, the Deployment Preview and Apply refuse it. |
| `collision_rule_confirmation_required` | The Apply changes collision rules its request did not confirm. Preview again and confirm. |
| `collision_rule_selection_unresolved` | A selection names a component, body, link, model group or Scene object the cell does not have. |
| `invalid_collision_rule` | A rule is malformed: no or more than one value, a number out of range, a duplicate id. |

A release that fails is reported with the reason of the failed check, for
example a pair that got deeper, did not come apart by the end, or touched
again. See also [Troubleshooting](../operations/TROUBLESHOOTING.md).

## Scenarios saved before collision rules

Older Scenario files used a per-robot `collision_clearance_m`, a `clearance_m`
on motion nodes, Program contact policies and support contacts on Scene
objects. They are not loaded any more. Convert them once:

```bash
python3 scripts/convert_scenario_od46.py path/to/my.scenario
```

The script rewrites `program.yaml` and `deployment.yaml` together and prints
what it changed. Support contacts become rules on single moves: a release
rule on the lift and a contact rule on the last place move. Check these two
moves in the Composer after the conversion: moves in between now keep the
normal clearance to the support. When the script cannot convert something it
names it and writes nothing.
