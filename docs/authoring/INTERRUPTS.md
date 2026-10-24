> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Interrupts

An interrupt stops a robot's motion when a condition becomes true, for
example "a part was detected" or "the light curtain opened". ManyForge has
two kinds of interrupt rules, and they are fully separate:

| | Node rule | Global rule |
| --- | --- | --- |
| Where you author it | On a node in the Program: **Move until signal** (`motion.move_until_signal`) or **Probe search** (`motion.probe_search`) | In the Deployment, under `policies.interrupt_rules` |
| When it is active | Only while that node's move runs | All the time, for every motion in its scope |
| What it stops | That move | Every motion in its scope |
| What it decides | The node's result (success or failure) | Nothing in the Program directly; it publishes its state in `interrupt_state.<rule_id>` |

Interrupts are operational stops, not safety functions. Real emergency stops
belong to the cell's safety controller.

Blackboard keys are described in [Blackboard keys](BLACKBOARD_KEYS.md).

## 1. What a condition can read

A condition compares values. Each comparison reads one of these:

- **A signal**: a digital or analog input.
  - In a node rule, choose an input of the node's robot, written
    `{signal: {target_role: arm, resource_role: part_present}}`.
  - In a global rule, choose an input of a Deployment component, written
    `{signal: {controllable_component_id: station, signal_name: part_present}}`.
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
- `bool` and `string` values only use `==` and `!=`.
- The value is always a fixed literal of the operand's type: no formulas, and
  no conversion. `1` does not match a `float64` written as `1.0` by accident,
  and `"true"` is not `true`.
- Limits for each condition: 64 parts in total and 8 levels of nesting
  (at most 63 distinct inputs follow from the grammar; contract 137).

For richer logic, compute it in a computed key (see
[Expressions](EXPRESSIONS.md)) and compare that key.

Example: stop when the part sensor turns off, unless the cell is in setup
mode.

```yaml
condition:
  all:
    - compare: {operand: {signal: {controllable_component_id: station, signal_name: part_present}}, op: "==", value: false}
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
| `stop_mode` | Selects this node's Stop mode independently of the robot's default, using its configured limits | required |
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

## 4. Global rules (Deployment)

A global rule watches its condition all the time and stops every motion in
its scope:

```yaml
policies:
  interrupt_rules:
    - rule_id: light_curtain          # unique; publishes interrupt_state.light_curtain
      condition: {compare: {operand: {signal: {controllable_component_id: station, signal_name: curtain_clear}}, op: "==", value: false}}
      scope: all_motion
      priority: 0
      idle_behaviour: latch           # optional
      latch_release: operator         # optional
      stop_entry_stage_id: software_hard   # optional
```

| Field | Meaning | When omitted |
| --- | --- | --- |
| `rule_id` | Lower-case name, unique in the Deployment (letters, digits, `_`, `.`, `-`; at most 128 bytes). It also names the rule's state key `interrupt_state.<rule_id>` | required |
| `condition` | The condition (section 2) | required |
| `scope` | What it stops (below) | required |
| `priority` | 0 to 100 | 0 |
| `idle_behaviour` | `latch` or `ignore` (below) | see below |
| `latch_release` | `operator` or `auto_on_clear` (below) | `operator` |
| `stop_entry_stage_id` | The Stop stage to start from (section 5) | the robot's first stage |

A global rule has no `on_signal`, `on_no_signal` or
`success_through_stage_id`. Unknown fields are refused.

**Scope:**

- `all_motion`: every arm, in a Program or moved by hand, with or without a
  Run.
- `{execution_groups: [left, right]}`: the robots of these execution groups
  of the Applied Program (sorted, no duplicates). Not available when no
  Program is applied.
- `everything`: the existing Full Stop. All motion and all other commands
  stop, including outputs and grippers.

**When it trips:**

1. The motion in scope stops, starting from the chosen stage.
1. The rule latches. A latched rule blocks new motion in its own scope only.
1. Its state key `interrupt_state.<rule_id>` changes to `tripped`, then
   `latched`.

**Idle behaviour**, when the condition trips while nothing in the scope
moves:

- A moving robot is always stopped, whatever the setting.
- Omitted: the rule latches while a Program Run is in progress, and is
  ignored when there is no Run.
- `latch`: the rule always latches.
- `ignore`: the rule never latches while nothing moves. It records one
  information row in the history.

**Clearing a latch:**

- `latch_release: operator` (default): the operator clears the latch in the
  GUI with the cell recovery action, once the condition is no longer true.
- `latch_release: auto_on_clear`: the latch clears by itself when the
  condition is no longer true.
- A Program Reset never clears a global rule's latch.

**Reading user keys.** A global rule may compare keys of the Program:

- The key must be declared by the Program, with `initial:`.
- With no Program applied, the **whole rule** waits: it is inactive and
  shows "waiting for a Program that declares <key>". There is no partial
  evaluation, also when the rule also reads signals.
- A rule that reads both signals and keys is marked: it doesn't act while no
  Program is applied. If it must always act, put the signals in a separate
  signal-only rule.
- Applying a Program that does not declare a key a global rule reads is
  refused with `global_rule_key_undeclared`, naming the rule and the key.
- User keys never cause the startup wait of section 7. Only real signal
  sources do.

**Rule states are for the Program.** A global rule cannot read
`interrupt_state.*` keys; such a condition is refused with
`global_rule_state_operand_unsupported`, and the editor does not offer them.
Read rule states in the Program instead: in check and wait nodes, computed
keys, or a node rule on Move until signal / Probe search.

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
| `clear` | The condition is false and the rule is not latched |
| `tripped` | The condition is true |
| `latched` | The rule tripped and is still latched |

- The key is always current: also between Runs, while a Run is held or
  halted, and when no Run exists. Every change is kept in the history.
- It has no value until the rule's sources have reported for the first time.
  After that it always follows the rule.
- Reset does not change it. Only the operator, or `auto_on_clear`, clears a
  latch.

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
| ros2_control GPIO, UR I/O states | Periodic |
| ROS topic declared **periodic** (with a minimum rate) | Current while readings arrive within the expected time |
| ROS topic declared **change only** | Unproven between changes |

A change-only source cannot be used in any interrupt condition: the editor
marks it "liveness unproven" and Apply refuses it. Read it with an I/O wait,
read or check node instead.

**Startup wait.** Until every signal source a global rule reads has reported
for the first time, motion in that rule's scope is refused with
`global_interrupt_source_not_ready`, naming the rule; the Global rules panel
shows the rule as "pending" until its current state is known. Sources report right after they
are enabled, so this wait is normally short. It does not latch. User keys do
not cause this wait.

**Source loss.** If a signal source of a global rule is lost (or its readings
stop arriving in time):

- The rule latches, also when nothing in its scope moves.
- Motion in scope that is running is stopped.
- Only the operator can clear this latch, once the source reports again and
  the condition is false. `auto_on_clear` does not apply to a loss latch.

For a node rule, losing its source fails the node.
