> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Interrupts

Interrupt rules react when a condition becomes true, for example "a part was
detected" or "the light curtain opened". A node rule stops its own move; a
global rule either Stops motion in its scope (`stop`) or only publishes its
state (`notify`). Any active global rule that can't watch its signals holds
its scope until it can. The [Concepts](#concepts) section below explains the
model: what a rule can read, the two kinds of rules, and how they react.
Sections 1 to 7 are the reference for writing conditions and rules.

## Concepts

### Signals

A **signal** is a named value that a component publishes, for example
`part_present` on an I/O box or `curtain_blocked` on a station. Each signal
has a source (the component that publishes it) and a freshness budget: the
time within which a new reading must arrive for the value to count as
current. Signals that rules read are `true` or `false`.

A signal's id is `<component_id>/<signal_name>`. The I/O banks of a machine
(for example an xArm's control box and tool I/O) are I/O members of the
machine, so their signals are `<member_id>/<signal_name>`.

You can use a signal in several places:

- a Program reads it (Get input, Check input) or waits for it (Wait for
  input);
- the GUI shows its current value and source (Tool & I/O panel);
- interrupt rules watch it.

**I/O** is the physical part of the signals: the digital inputs and outputs
of devices. Not every signal is I/O. For example, the geometric contact
detector publishes a contact signal computed from the Scene, with no
physical input behind it.

**Changed outside.** For every output with read-back that isn't
controller-owned, ManyForge holds the boolean signal
`<signal_id>.changed_outside` (in a condition: signal name
`<output>.changed_outside`). It becomes `true` when the device reports a
value that ManyForge didn't write; it becomes `false` when the component is
prepared, after an accepted ManyForge write to that output, or with the node
**Clear outside change** (`io.clear_outside_change`). Mock outputs report the
written value back, so the signal exists on every lane.

Interrupt rules only **read** signals. Writing an output (Set output) is a
Program action, never the effect of a rule.

### Blackboard keys as rule inputs

The second kind of input is a **blackboard key** that the Program writes
(see [Blackboard keys](BLACKBOARD_KEYS.md)). A rule can compare signals and
keys together, with `==`, `!=`, `<`, `<=`, `>`, `>=` combined by AND, OR and
NOT (section 2).

- A key that a rule reads must declare `initial:`, so that the rule has a
  value from the start.
- Between Runs, with an Applied Program, rules see each referenced run-scoped
  key's declared `initial` after Reset or a cold Apply; persistent keys use
  their stored value, or their declared `initial` when none is stored. A
  finished Run's keys keep their last value until Reset.
- Every write to such a key is sent to the rule evaluation at once; the rule
  does not wait for a periodic check.
- A formula key (`expression:`) cannot be read by a rule: it has no
  `initial:` and is never written. Compute the value with a **Set value**
  node into a normal key instead.

The usual pattern: a part of the Program tree computes a value and writes it
to a key; a rule reacts to that key.

### Interrupt rules

An **interrupt rule** is a condition plus a reaction. There are two kinds,
and they are fully separate:

| | Node rule | Global rule |
| --- | --- | --- |
| Where you author it | On a node in the Program: **Move until signal** (`motion.move_until_signal`) or **Probe search** (`motion.probe_search`) | In the Deployment, under `policies.interrupt_rules` |
| When it is active | Only while that node's move runs | While the rule is active (`active_at_start`, or the node **Set rule active**), for every motion in its scope |
| Motion effect | Stops that move | `stop`: Stops motion in scope, then holds the scope while the condition holds; `notify`: none |
| What it decides | The node's result (success or failure) | No Program result; it publishes its state in `interrupt_state.<rule_id>` |

A global rule reacts in one of two ways (`on_trip`, section 4):

- `stop`: the same Stop as the operator's Stop, on the rule's scope. Then,
  while the condition still holds, the rule **holds** its scope: Resume,
  Execute and new motion in scope are refused with `rule_holds`. When the
  condition clears, the hold clears by itself, with no acknowledgement.
  `after_stop` chooses what happens to the Program run: `hold` (default)
  holds the run like the operator's Stop, so it continues only with
  **Resume**; `fail_node` fails the stopped motion node and the run goes on.
- `notify`: the rule only publishes its state key for Program nodes to read.
  A trip doesn't Stop, hold or refuse anything.

A rule trip is an operational stop: never a component fault and never a cell
latch.

### How rules are evaluated

- Sources publish readings to the rule evaluator. Each new reading, and
  each write to a key a rule reads, starts a new evaluation of the rules that
  read it.
- A write to a key a rule reads, by the tree, the operator's runtime
  blackboard control or the API, is evaluated before the write returns, in
  write order, one evaluation per write. The tree's next step sees the
  result, and a value written and reset at once still trips the rule.
- Reaction time depends on the source's publishing interval and on system
  scheduling. The publishing rate alone is not a response-time guarantee.
- Each global rule's state change is streamed to the Program runner and
  appears as `interrupt_state.<rule_id>`, for checks, waits and conditions.
  The rule does not wait for that copy before acting, so a node can see the
  change slightly after the rule acted.

### Which input to use

| | Component signal | Key written by the Program |
| --- | --- | --- |
| When it is active | Always, with or without a Program or Run | Only while a Program that declares the key is applied |
| Freshness | Checked: a source that stops reporting is detected | Not checked: the key keeps its last value |
| If it fails | The rule can't watch: it Stops moving robots in its scope and holds the scope until the source reports again | A write that can't be delivered stops the Run; the rule can't watch and holds its scope until the next Run or Reset. Otherwise the value is only as current as the part of the tree that writes it |
| How current | The source's publishing rate | The loop of the part of the tree that writes it |
| Use it for | Protection that must always act (light curtain, door, part sensor) | Program-phase logic (counts, modes, "this step is done") |

A rule that reads both kinds does not act while no Program is applied
(section 4). If part of it must always act, write that part as a separate
rule that reads signals only.

### Worked example: a counted tray holds the loader

A loader robot places parts into a tray of 12 slots. When the tray is full,
the loader must not start another place move until the operator has swapped
the tray. The Program counts the parts; a global rule reacts to the count.

1. The Program declares the counter, with `initial:` so a rule can read it:

   ```yaml
   blackboard:
     keys:
       - key: tray.count
         type: int64
         access: read_write
         persistence: run
         initial: 0
         description: Parts placed in the current tray
   ```

1. After each place move, a **Set value** node writes `tray.count` from the
   formula `key.tray.count + 1`.
1. A parallel branch of the tree waits for the input `tray_swapped` (**Wait
   for input**), then sets `tray.count` back to `0`.
1. The Deployment adds a global rule on the loader's execution group:

   ```yaml
   policies:
     interrupt_rules:
       - rule_id: tray_full
         condition: {compare: {operand: {key: tray.count}, op: ">=", value: 12}}
         scope: {execution_groups: [loader]}
         on_trip: stop
         after_stop: hold
         active_at_start: true
   ```

What happens: the 12th Set value write reaches the rule at once and the rule
trips. The Program run is held, like the operator's Stop; a loader move that
was running is stopped. While `tray.count` is 12 or more, **Resume** is
refused with `rule_holds` and shown disabled. When the operator swaps the
tray, the branch resets the counter, the rule clears, and Resume is
available again.

With `after_stop: fail_node` instead, the run keeps going: a stopped loader
move fails its node with `interrupt_fault`, and every new loader move fails
at once with `rule_holds` until the counter is reset, so the tree's own
Fallback handles it.

The tray counter is Program logic, so a key is the right input. A light
curtain in the same cell would use a signal in its own signal-only rule, so it
also protects motion when no Program is applied.

### What is not an interrupt rule

These checks also stop motion, but through their own owners. They are not
configured as interrupt rules and do not appear in `interrupt_state.*`:

- **Grasp lost:** when a carried object leaves its bound, the **Attach object**
  node's `on_grasp_lost` settings choose whether to Stop, raise an alarm, set a
  key, or detach the object. The system signal is published on every loss; see
  [Grasp lost as a Program event](#grasp-lost-as-a-program-event).
- **Runtime collision monitor:** a collision it finds stops the motion
  through the normal Stop path. The monitor also stops the motion when the
  robot's measured position stops arriving or becomes too old (for example
  after a robot controller fault). That Stop is reported as **Robot position
  feedback lost** (`runtime_monitor_observation_lost`), not as a collision; it
  is otherwise handled like a runtime collision Stop.
- **Component faults:** when a robot or I/O device faults, or its own stream
  goes stale, the component shows `fault`. An enabled I/O device whose ROS
  inputs lose every sender faults too; with an `inputs` budget, so does one
  that goes silent ([Add an I/O device](ADDING_A_COMPONENT.md#add-an-io-device)). A fault while anything executes,
  or an unhealthy component after a Stop, latches the cell; the latch blocks
  Program runs, nodes, interrupt evaluation, manual motion and manual I/O
  until every component is healthy and idle again, and clears by itself.

### Grasp lost as a Program event

The grasp-lost check compares the measured pose of an attached object with the
bound on its **Attach object** node (`grasp_lost_translation_m`,
`grasp_lost_rotation_rad`). The source must measure the carried offset (for
example, `continuous_relative_to_attached`). A commanded-only Mock pose does
not measure object loss. Mock can exercise a writable key through the runtime
blackboard control, but it cannot trigger the measured-loss system signal or
qualify the grasp-loss detector.

The Attach object node's `on_grasp_lost` settings control the response:

| Setting | Default | Effect on loss |
| --- | --- | --- |
| `raise_alarm` | `true` | Raise the `scene.carried_object_lost` alarm. When true, `stop_motion` must also be true. |
| `stop_motion` | `true` | Request the Stop for the carrier. The carrier motion's node fails (`carried_object_lost`) as an ordinary failure, so the Program's Fallback can run. |
| `set_key` | `null` | Optional declared writable `bool` key. The Runner writes `true` once for this attachment's loss. |
| `detach` | `true` | Detach the object at its observed pose. |

If ManyForge cannot publish the grasp-lost signal, it raises the alarm and Stops regardless of these settings; the alarm names the publication error.

The default is alarm + Stop + detach. A Program-handled response can set
`raise_alarm: false`, `stop_motion: false`, name a writable key in `set_key`,
and keep `detach: true`. The loss also raises the system input signal
`carried_object_lost.<command_target_id>` for the gripper target. It is
read-only and declared by the Deployment automatically. It becomes `true` on
loss and becomes `false` on the next attach by that gripper, a Program detach,
or cell Reset. Detach-on-loss does not clear it.

For an explicit Program-managed detach, set `detach: false` and a writable
`set_key` on the attachment. A parallel Program branch can wait for that key
and run a **Detach object** node for the carried part. The explicit detach
updates the controller payload after the detach commits. The key event can
arrive before a detach commits, and the parallel branch is scheduled by the
Program; this pattern does not guarantee an immediate payload update or stop
motion. Automatic grasp-loss detach also updates the controller payload after
commit. See [TCPs](TCPS.md#attach-to-the-active-tcp).

The Runner's key mirror is event-driven; it writes `true` for the loss and
does not write `false`. Reset the key explicitly with a **Set value** node
before the next carry. Give the key `initial: false` so a Deployment rule can
read it (section 1). The system signal is the immediate input for motion and
Deployment rules; the optional key is a separate Program value mirror.

**Mock key simulation:** use
[`examples/mock_lite6_part_lost_recovery.scenario`](../../examples/mock_lite6_part_lost_recovery.scenario)
to exercise the key-triggered global rule and recovery. Its Deployment rule
`part_lost_guard` reads the writable key `part_lost`, with `on_trip: stop`,
`after_stop: fail_node` and scope `{execution_groups: [carry]}`. Mock does not
measure grasp loss, so start a Run, wait until `pick_retreat` is running,
then in the **Program run** panel open **Advanced: runtime blackboard, stored
values and audit**, choose `part_lost`, enter `true`, and click **Write
current-run value**. This simulates the key value; it does not publish a
grasp-loss event, change the read-only `carried_object_lost.<target>` signal,
or qualify the measured-loss detector.

The rule Stops `pick_retreat`; its node fails with `interrupt_fault` and the
run continues. The example's Fallback checks that `part_lost` is still true,
resets it to `false` (which clears the rule), moves to the safe pose,
detaches `graspable` (the manual write did not detach it) and skips the item;
the Run continues with the next cycle. If the key is written while no carry
move runs, the next carry move fails at once with `rule_holds` and the same
Fallback runs. The `carry` group resolves to exact Applied motion subjects
(component, command target, control domain, binding and revision); the hold
refuses every motion whose subject matches, including recovery and manual
moves on the same target bindings, so the Fallback clears the key before it
moves.

The same current-Run write is available through
`POST /api/program/runs/{program_run_id}/blackboard-values`; include the live
Run's `program_run_id`, `expected_owner_epoch`, and `expected_run_revision`,
plus `key` and the JSON boolean `value: true`, and the ordinary request
attribution fields (`request_id`, `request_owner_epoch`, `source_label`, and
`correlation_id`).

**Isaac measured-loss example:**
[`examples/sim_ur10e_grasp_lost_recovery.scenario`](../../examples/sim_ur10e_grasp_lost_recovery.scenario)
uses the UR10e light scene's `continuous_relative_to_attached` source for
`/mac_n_cheese_centered`. Run it in Isaac, then cause the attached cheese
box's measured pose to leave the configured translation or rotation bound
while `pick_retreat` is running. The intended result is that the signal
trips, the Fallback moves to the safe pose, and the item is skipped; this
recovery is not yet verified on this build. Writing a key manually is not
measured-loss evidence.

In that node-level pattern, `pick_retreat` is a **Move until signal** that
reads the gripper's `carried_object_lost` resource role and uses
`on_signal: failure`. That field chooses the node result; it does not name a
recovery subtree. The node fails as an ordinary failure (no operator hold, no
alarm), so the Fallback runs. The **Fallback** has the carry Sequence first
and a second sequence guarded by **Check input** for that same signal to be
`true`; it moves to the safe pose and skips the item. The system signal
remains true until the next attach by that gripper, a Program detach, or cell
Reset. Detaching because of the loss does not clear it. Do not combine the
node-level rule and a global `stop` rule on that same signal: the global rule
can pre-empt the node's `on_signal` result.

If an object rotates in the fingers but remains in contact, a continuing move
may instead be stopped by the runtime collision monitor. While no Run is active,
there is no current-Run blackboard to receive the key write; during a later Run,
a loss on an attachment from an earlier Run can set its `set_key` if the current
Program declares that key as a writable `bool`. The key means a loss happened
during the current Run.

### Choosing what a trip does

| Outcome | How to author it |
| --- | --- |
| (a) Keep moving; only raise a signal or key for the tree or another rule to catch | `on_grasp_lost: {stop_motion: false, raise_alarm: false, set_key: <key>}`, or a global rule with `on_trip: notify` |
| (b) Stop, then the node succeeds | Move until signal / Probe search with `on_signal: success` |
| (c) Stop, then the node fails and the tree's Fallback runs at once | `on_signal: failure`, a global rule with `on_trip: stop` and `after_stop: fail_node`, or `on_grasp_lost: {stop_motion: true}`, each with the node's default `recovery_policy` |
| (d) Stop and hold the node for the operator | As (c), plus the node's `recovery_policy: {allow_intervention: true, max_retries: N}`: the Run holds with Retry from the stopped state (at most `N` times; `0` is unlimited), Continue (the node fails, then the Fallback runs) and Reset |
| (e) Stop and hold the run until the condition clears | A global rule with `on_trip: stop` and `after_stop: hold` (the default); the operator Resumes once the rule no longer holds |
| (f) Stop the whole cell | A global `stop` rule with scope `everything`: a cell Full Stop |

(b) to (d) hold only when the Stop held at its first stage. An escalated
Stop, a lost source, or an operator Stop joining it keeps the operation held
for the operator. A Stop requested through the public Stop API is always an
operator Stop.

### Interrupts are operational

Interrupts are operational stops, not safety functions. Real emergency stops
belong to the cell's safety controller.

## 1. What a condition can read

A condition compares values. Each comparison reads one of these:

- **A signal**: a true/false input signal of a component (see
  [Signals](#signals)).
  - In a node rule, choose an input of the node's robot, written
    `{signal: {target_role: arm, resource_role: part_present}}`.
  - In a global rule, choose an input of a Deployment component, written
    `{signal: {controllable_component_id: station_io, signal_name: part_present}}`.
    For a machine's I/O bank, `controllable_component_id` is the I/O
    member's id. `signal_name: <output>.changed_outside` reads the
    changed-outside signal of that output.
- **A user key**: a blackboard key the Program declares, with type `bool`,
  `int64`, `float64` or `string`. It must declare `initial:`, so the rule
  has a value to compare from the start (`false` and `0` are valid).
  Otherwise the rule is refused with `interrupt_key_initial_required`.
- **A system key**: `node_outcome.<node>` or `interrupt_state.<rule>`
  (section 6). These never need `initial:`, because the system defines their
  start value. A global rule cannot read `interrupt_state.*` keys (section 4);
  node rules can.

Program parameters cannot be read by a condition. They never change during a
run.

In the editor, signals and keys are offered in a grouped list:

- signals first;
- then your keys, by namespace;
- then **Node outcomes**, listed by node name;
- then **Interrupt states**, listed by rule name (node rules only).

For a system key, you also pick the value from a list: the key's allowed
values (section 6). There is nothing to type.

## 2. Condition syntax

A condition is exactly one of these four forms:

| Form | Meaning |
| --- | --- |
| `{compare: {operand: ..., op: ..., value: ...}}` | One comparison: the operand (a signal or `{key: <name>}`), an operator and a fixed value |
| `{all: [C, ...]}` | True when every condition in the list is true (AND); at least one entry |
| `{any: [C, ...]}` | True when at least one condition in the list is true (OR); at least one entry |
| `{not: C}` | True when C is false |

- Operators are `==`, `!=`, `<`, `<=`, `>` and `>=`.
- `bool` and `string` values only use `==` and `!=`; a signal compares with
  `true` or `false`.
- The value is always a fixed literal of the operand's type: no formulas, and
  no conversion. `1` does not match a `float64` written as `1.0` by accident,
  and `"true"` is not `true`.
- Limits for each condition: 64 parts in total and 8 levels of nesting.

For richer logic, compute the value with a **Set value** node (see
[Expressions](EXPRESSIONS.md)) into a key with `initial:` and compare that
key.

Example: stop when the part sensor turns off, unless the cell is in setup
mode.

```yaml
condition:
  all:
    - compare: {operand: {signal: {controllable_component_id: station_io, signal_name: part_present}}, op: "==", value: false}
    - compare: {operand: {key: cell.setup_mode}, op: "==", value: false}
```

## 3. Node rules: Move until signal and Probe search

A node rule belongs to one move. It is armed when the move starts and removed
when the move ends. Its settings are node parameters and are always fixed
values (they cannot come from a key):

| Parameter | Meaning | When omitted |
| --- | --- | --- |
| `interrupt_condition` | The condition (section 2), required | |
| `on_signal` | Node result when the condition trips: `success` or `failure` | `success` |
| `on_no_signal` | Node result when the move ends without a trip | `success` for Move until signal, `failure` for Probe search |
| `priority` | 0 to 100 | 0 |
| `stop_entry_stage_id` | The Stop stage to start from (section 5) | the robot's first stage |
| `success_through_stage_id` | The last Stop stage that still counts as a clean trip (section 5) | only the entry stage counts |
| `stop_mode` | This node's Stop mode: `spring_back_hold`, `soft_on_path`, `hard_deceleration` or `spring_back_fast`, using the robot's configured limits | required |
| `activation_mode` | `require_inactive_at_arm` (the signal must be off when the move starts), `fresh_edge_after_arm` (only a new change after the start counts) or `trip_if_already_active` | `require_inactive_at_arm` (Probe search allows only this one) |

What happens:

- The node's `stop_mode` selects its primary mode; it does not change the
  Deployment's operator Stop. The robot's configured deceleration, jerk,
  settle limits and Stop-stage chain still apply. Only already-enabled
  fallback modes below the selected primary are retained; the hard final
  stage remains mandatory. A controlled mode requires configured controlled
  Stop limits: a hard-only profile cannot supply them.

- **The condition trips**, and the robot stops at a stage up to
  `success_through_stage_id`: the node result follows `on_signal`. Its
  outcome key becomes `signal_trip`.
- **The robot stops only at a later stage**: the node fails through its
  normal failure handling (an intervention hold if its recovery policy allows
  one).
- **The move ends without a trip**: the node result follows `on_no_signal`.
  Its outcome key becomes `no_signal_trip`.
- **The stop failed or cannot be proven, or the signal source was lost**: the
  node fails.

In the Composer node editor, **When the signal trips** sets these fields in
one Draft edit:

- **Stop and succeed**: `on_signal: success`.
- **Stop and fail (run the Fallback)**: `on_signal: failure`, with
  intervention not allowed in the node's `recovery_policy`.
- **Stop and ask the operator (Retry / Continue / Reset)**:
  `on_signal: failure`, with `recovery_policy.allow_intervention: true`. The
  retry limit shown with it is `recovery_policy.max_retries`.

Values that match none of these show as **Custom**. On the **Attach object**
node the same dropdown sets `on_grasp_lost`:

- **Stop and raise the alarm (default)**: the default settings (alarm, Stop
  and detach, no key).
- **Keep moving, set a signal**: `stop_motion: false`, `raise_alarm: false`
  and the chosen `set_key`; `detach` is kept.

A key created from the key picker is written to `set_key` only. To stop the
whole cell, use a global `stop` rule with scope `everything` (section 4).

## 4. Global rules (Deployment)

A global rule watches its condition while it is active:

```yaml
policies:
  interrupt_rules:
    - rule_id: light_curtain          # unique; publishes interrupt_state.light_curtain
      condition: {compare: {operand: {signal: {controllable_component_id: station_io, signal_name: curtain_blocked}}, op: "==", value: true}}
      scope: all_motion
      priority: 0
      on_trip: stop                   # stop | notify
      after_stop: hold                # optional, stop only: hold | fail_node
      stop_entry_stage_id: software_hard   # optional, stop only
      active_at_start: true           # optional, default false
```

| Field | Meaning | When omitted |
| --- | --- | --- |
| `rule_id` | Unique in the Deployment: a lower-case letter, then lower-case letters, digits, `_`, `.`, `-`; at most 128 bytes. It also names the rule's state key `interrupt_state.<rule_id>` | required |
| `condition` | The condition (section 2) | required |
| `scope` | Which motions the rule Stops and holds (below) | required |
| `priority` | 0 to 100 | 0 |
| `on_trip` | `stop` or `notify` (below) | required |
| `after_stop` | `hold` or `fail_node`, only with `on_trip: stop` (below) | `hold` |
| `stop_entry_stage_id` | The Stop stage the rule's Stop starts from (section 5); only with `on_trip: stop` | the robot's first stage |
| `active_at_start` | Whether the rule is active when a run starts and after Load | `false` |

A global rule has no `on_signal`, `on_no_signal` or
`success_through_stage_id`. Unknown fields are refused. The Deployment's
**Global interrupt rules** section edits the same fields; `after_stop` and `active_at_start` show
their defaults until set.

**Scope:**

- `all_motion`: every arm, in a Program or moved by hand, with or without a
  Run.
- `{execution_groups: [left, right]}`: the robots of these execution groups
  of the Applied Program (sorted, no duplicates). Not available when no
  Program is applied.
- `everything`: the cell Full Stop: all motion and all other commands stop,
  including outputs and grippers. While such a rule holds, output writes are
  refused too.

**Activation.** An inactive rule doesn't watch, trip, Stop or hold; its state
is `inactive`. The node **Set rule active** (`rules.set_active`, parameters
`rule_id` and `active`) activates or deactivates one rule of the Deployment
and succeeds at once; a `rule_id` that isn't in the Deployment fails the node
with `rule_unknown`. Activating a rule whose condition already holds trips it
at once. Every rule returns to its `active_at_start` at the end of every run
(completed, failed or stopped), at Reset, and after every Apply (Load and New
included). A rule meant to guard the cell at all times needs
`active_at_start: true`.

**What a trip does (`on_trip`):**

- `stop`: the same Stop as the operator's Stop on the rule's scope (scope
  `everything`: the Full Stop), starting from `stop_entry_stage_id`. Then,
  while the condition holds, the rule holds its scope: Resume, Execute and
  new motion in scope (also manual motion) are refused with `rule_holds`,
  naming the rule, and the GUI shows them disabled. When the condition is no
  longer true the hold clears by itself; there is nothing to acknowledge.
  - `after_stop: hold` (default): the Program run is held like the operator's
    Stop, also when nothing in the rule's scope was moving. It continues only
    with **Resume**, which waits until the rule no longer holds. If the run
    ends from there, its end is `stopped` with `stopped_by: rule` and the
    `rule_id`.
  - `after_stop: fail_node`: the stopped motion node fails with
    `interrupt_fault` and the run continues, so the Program's own failure
    handling runs (for example a Fallback that moves the robot). A new motion
    in scope fails at once with `rule_holds` while the rule holds.
  - A Robot Control jog the rule stops ends `stopped`.
- `notify`: a trip only publishes the rule's state through the read-only
  `interrupt_state.<rule_id>` key. It doesn't Stop, hold or refuse anything.
  Program nodes can read the key with **Check value** or **Wait for value**.

**Can't watch.** Any active rule, `stop` or `notify`, whose signals are
stale, invalid or missing, or whose setup failed, can't watch. Until it can
watch again it holds its scope (`rule_holds`), and a robot moving in its
scope is stopped (`interrupt_source_unavailable`). Its state is withdrawn:
the Program has no current `interrupt_state.<rule_id>` value, so it can't
read a stale `clear` or `tripped`. A fresh, evaluable reading clears this by
itself. It is never a latch and needs no operator action. The Global rules
panel shows the rule as "can't watch (its scope is held)", with the catalog
reason and its details.

For example, a Deployment rule can publish `interrupt_state.tray_full` with
`on_trip: notify`. Put an ordinary work branch and a handler branch in a
Program **Parallel** node (`success_policy: all`); start the handler branch
with **Wait for value** on `interrupt_state.tray_full`, comparing it with
`tripped`, then continue to an existing handler node such as **Set output**
for an indicator. On the trip, the rule only publishes the read-only state
key: the other branch keeps running, and the Program does not write a key for
the rule. A brief trip can clear before the handler branch reads it. This is
not a motion stop.

**Readiness.** Every hold appears in the run readiness
(`GET /api/program/readiness`) as a `rule_holds` reason with its `rule_id`,
its `scope` (`everything`, `all_motion` or `execution_groups`) and, for
execution groups, the `component_ids`. A hold blocks Execute and Resume only
when it meets the Program's motion; the GUI disables the Run/Resume of each
motion group it meets.

**Reading user keys.** A global rule may compare keys of the Program:

- The key must be declared by the Program, with `initial:`.
- With no Program applied, the **whole rule** waits: it doesn't watch, trip
  or hold, and shows "waiting for a Program that declares <key>". There is no partial
  evaluation, also when the rule also reads signals.
- A rule that reads both signals and keys doesn't act while no Program is
  applied. If it must always act, put the signals in a separate signal-only
  rule.
- Applying a Program that does not declare a key a global rule reads is
  refused with `global_rule_key_undeclared`, naming the rule and the key.

**Rule states are for the Program.** A global rule cannot read
`interrupt_state.*` keys; such a condition is refused with
`global_rule_state_operand_unsupported`, and the editor does not offer them.
Read rule states in the Program instead: in check and wait nodes, or in a
node rule on Move until signal / Probe search.

## 5. Stop stages

Each robot has an ordered chain of qualified Stop stages, shown in the GUI
as:

1. Controlled stop (including spring-back);
1. Controller goal cancel;
1. Software hard stop.

`stop_entry_stage_id` chooses where an interrupt's Stop starts. If a stage
fails or refuses, the Stop continues with the next stage in this order. You
pick a stage from the robot's published chain; its ids are available after
Apply. Until then, the first stage applies.

For a node rule, `success_through_stage_id` is the last stage, at or after
the entry stage, at which the trip still counts as a clean trip (section 3).

## 6. System keys and their values

The system writes these keys. You can read them anywhere a key can be read
(conditions, computed keys, **Use key…** bindings, Check value), except that a
global rule cannot read `interrupt_state.*` keys. You can never declare or
write them.

### `node_outcome.<node>`

Every motion node has one: Move to joint target, Move to pose, Follow
Cartesian path, Move until signal and Probe search. `<node>` is the node's
name in the tree.

| Value | Meaning |
| --- | --- |
| `none` | The node has no result yet in this Run |
| `completed` | The move finished |
| `failed` | The move failed |
| `preempted` | Another motion took over the robot during this move |
| `superseded` | Another motion took over the robot before this move was sent |
| `refused_by_priority` | The move was refused: a higher-priority motion holds the robot |
| `signal_trip` | Move until signal / Probe search only: the condition tripped |
| `no_signal_trip` | Move until signal / Probe search only: the move ended without a trip |

- The key is `none` when a Run starts and after Reset.
- It changes only when the node ends. It keeps that value until the node
  ends again; it is not cleared when the node starts again.

### `interrupt_state.<rule_id>`

One per global rule.

| Value | Meaning |
| --- | --- |
| `inactive` | The rule is not active (section 4, Activation) |
| `clear` | The rule is active and its condition is false |
| `tripped` | The rule is active and its condition is true |

- When it has a value, the key is current also between Runs, while a Run is
  held or halted, and when no Run exists.
- It has no value until the rule's sources have reported for the first time
  (the Global rules panel shows "pending"), while a key rule waits for a
  Program, and while the rule can't watch its signals (section 4). A fresh,
  evaluable reading restores it.
- Reset doesn't change the condition; it returns the rule to its
  `active_at_start`, which can change the key between `inactive` and the
  rule's current state.

### Writing "not completed" correctly

Before a node runs, its outcome is `none`. So
`node_outcome.pick != completed` is already **true** before `pick` has run.
If "not completed" should only count after the node ran, also exclude
`none`:

```yaml
all:
  - compare: {operand: {key: node_outcome.pick}, op: "!=", value: completed}
  - compare: {operand: {key: node_outcome.pick}, op: "!=", value: none}
```

- Before `pick` runs: `none`, so the condition is false.
- After `pick` completes: `completed`, so the condition is false.
- After `pick` fails, is preempted, and so on: the condition is true.

The editor shows a hint when a `!=` comparison on a node outcome is not also
guarded against `none`. It is only a hint; your condition is saved as
written.

### Reserved names

`node_outcome` and `interrupt_state` belong to the system. A blackboard key,
a key namespace or a Program parameter cannot be named `node_outcome` or
`interrupt_state`, or start with `node_outcome.` or `interrupt_state.`. The
editor refuses such a name when you create or rename it, and Program
validation refuses it too.

## 7. Signal sources

**Liveness.** A rule needs to know that its signal is current, even when the
value does not change. Each input's source kind tells how:

| Source | Liveness |
| --- | --- |
| Mock input | Always current |
| ros2_control GPIO | Periodic |
| Geometric contact | Periodic |
| ROS topic declared **periodic** (with a minimum rate) | Current while readings arrive within the expected time |
| ROS topic declared **change only** | Unproven between changes |
| A machine's I/O bank on the real lane (for example the xArm control box and tool I/O) | Periodic: current while readings arrive within the model's `input_maximum_age_ms` |
| Changed outside (`<output>.changed_outside`) | Always current (held by ManyForge) |

A change-only input cannot be used in any interrupt condition: the editor
marks it "liveness unproven" and Apply refuses it
(`io_signal_liveness_unproven`). Read it with a Get input, Check input or
Wait for input node instead.

**First reading.** Until every signal a global rule reads has a fresh
reading, the rule can't watch: motion in its scope is refused with
`rule_holds`, naming the rule, and the Global rules panel shows the rule as
"pending" until its state is known. Sources report right after their
component is enabled, so this is normally short. User keys don't cause it.

**Source loss.** If a signal source of a global rule is lost (or its readings
stop arriving in time), the rule can't watch (section 4): a robot moving in
its scope is stopped, motion in scope is refused with `rule_holds`, and both
clear by themselves once the source reports again. This applies only to
losses the liveness model can detect.

For a node rule, losing its source fails the node.
