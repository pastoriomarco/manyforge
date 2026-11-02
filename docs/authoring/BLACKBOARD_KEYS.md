> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Blackboard keys

Blackboard keys hold a Program's working data while it runs. Formulas used
with keys are described in [Expressions](EXPRESSIONS.md).

## 1. Keys versus parameters

| | Parameters | Blackboard keys |
| --- | --- | --- |
| What they are | The Program's **configuration**: speeds, tolerances, dimensions, motion defaults | The run's **working data**: values produced, consumed and adjusted while the Program runs |
| Who sets them | The author; each Scenario can override them before Run | Nodes, the operator, the key's `initial:` value, or a formula |
| When they change | Only before Run; fixed for the whole run | At any time during the run |
| Written by nodes | Never | Yes (Set value, Get frame pose, Get object pose, Offset pose, perception and planning results, …) |
| Referenced in formulas as | `param.<name>` | `key.<name>` |

Use a parameter for "how this Program is tuned" and a key for "what is
happening in this run", including values the operator adjusts while it runs.

## 2. Declaring a key

Keys are declared in the Program:

```yaml
blackboard:
  keys:
    - key: pallet_index
      type: int64
      access: read_write
      persistence: persistent
      initial: 0
      description: Next pallet slot to fill
    - key: grasp_z
      type: float64
      access: read
      persistence: run
      expression: "-param.tube_length / 2 + param.grasp_offset"
```

| Field | Meaning |
| --- | --- |
| `key` | Name, unique in the Program; dots group related keys (`perception.grasp_plan`) |
| `type` | Value type (`bool`, `int64`, `float64`, `string`, a pose, or a structured type with `fields`) |
| `access` | `read`, `write` or `read_write`: which node and operator operations are allowed |
| `persistence` | How long the value lives (section 3) |
| `initial` | Optional starting value, checked against `type` |
| `expression` | Optional formula making this a **computed key** (section 5) |
| `allowed_frames` | Optional, for pose keys: the frames the pose may be expressed in (section 6) |
| `description` | Optional text shown in the editor |

## 3. How long a value lives (`persistence`)

| Persistence | Lifetime | Starting value at Run |
| --- | --- | --- |
| `run` | One Program run | `initial:`, otherwise empty |
| `persistent` | Survives runs, Reset (unless Reset reloads the initial scene) and restarts | The stored value, otherwise `initial:`, otherwise empty |

A key with no value yet can still be written; a node that **reads** it
before any value exists fails, naming the key. Starting values are loaded by
the Program runner itself, so a key declared `read` still receives its
`initial:` or stored value.

## 4. Persistent keys

- Only the **values** of persistent keys are stored, by the Program runner,
  per Scenario and key name. Declarations, initial values and formulas stay
  in the Program, and running never changes the Program.
- Stored values belong to the installation, not to the Scenario folder:
  Save As, Export or copying a Scenario never carries machine state such as
  "the pallet is at layer 3".
- A write is stored before it becomes visible, and only when the value
  changes. If it cannot be stored, the write fails and the previous value
  stays.
- A stored value is kept while the key stays persistent with the same type
  and fields. Changing `initial:`, the description or the access keeps it.
  It is dropped, with a notice, after an Apply that removes the key or
  changes its type or fields; the key then starts again from `initial:`.
- A stored value that the applied Program no longer accepts (for example a
  pose outside a narrowed `allowed_frames`) is dropped with a notice when the
  run starts, and the run still starts from `initial:`.
- If the runner missed an Apply, the values are kept and the page shows the
  fault "Applied Program not seen by stored values"
  (`applied_program_not_observed`) so the cause can be found and fixed.
- A Reset with **Reload initial scene** also resets every persistent key,
  whatever its access, to `initial:` (or empty). Its confirmation lists the
  stored values it resets (current value → new value), and each reset is
  recorded in the stored-value edits with that Reset. If they cannot be
  reset, the Reset fails by name before the Scene is reloaded and the stored
  values stay; Reset again to retry. The keys are reset before the Scene
  reloads: if the Scene reload then fails, Reset again to finish it (after New
  or Load instead, the keys stay reset although the Scene was not reloaded).
  A Reset without that option keeps them.
- A persistent key cannot be a computed key.

## 5. Computed keys

A key with `expression:` always holds the formula's current result, for
example `grasp_z` above. It must be `bool`, `int64`, `float64` or `string`,
declared `access: read` and `persistence: run`, without `initial:`. Nodes and
the operator cannot write it. It is evaluated whenever a node reads it, from
the same values the node reads, so changing `param.grasp_offset` or a key it
uses changes every value derived from it. Computed keys that read each other
in a cycle are refused when the Program is checked.

## 6. Reading and writing keys

**Reading:** any value in a node, at any depth, can come from a key of
matching type: right-click it and choose **Use key…** (for example a move
goal ← `key.place_pose`). A number can be a formula using keys:
`key.index + 1`, `key.part_pose.z + 0.02`.

**Writing, from the tree:**

| Node | Category | What it writes |
| --- | --- | --- |
| Set value | Logic | Any key, from a typed value, a parameter, another key or a formula |
| Get frame pose | Model | A robot frame's pose |
| Get object pose | Scene | A Scene object's pose, in the cell frame or a chosen frame |
| Offset pose | Geometry | A pose plus an offset, for example to place a Scene object |
| Get input | IO | A digital input's state |
| Perception, assistant and planning nodes | various | Their results |

**Checking and waiting:** Check value and Wait for value (Logic) compare a
key using operators that suit its type; Wait for value waits until the value
matches, or within an optional timeout. Check condition succeeds when a
boolean (typed, from a key or a formula) is true. Check pose distance and
Check pose bounds (Geometry) test poses.

**Poses and frames:** a pose carries its frame: the cell, a frame of one of
the robots (shown as robot · frame), or a Scene object. A pose key may limit
the frames its values may use with `allowed_frames`; writes in any other
frame are refused. The motion node's target robot and base frame, and its TCP
selector, are node settings. **Current** resolves from the robot's Active TCP.
Set TCP's entry selector may take a `tcp_entry` value directly from a key
([TCPs](TCPS.md#tcps-in-programs)); that structured value is not read through
an expression. When a move
uses a frame of something that moves (another robot, a carried object), that
frame is read once when the motion starts, and the target stays fixed from
then on.

**Writing, by the operator** (Program run details → Advanced):

- Runtime blackboard: write a value, with an editor for the key's type, for
  keys declared `write` or `read_write`, while the run is running or paused
  on a failure. Computed keys are not offered.
- Stored values: set or reset to initial any writable persistent key, with or
  without a run; with no run, the last accepted edit wins. **Clear stored
  values** resets every writable persistent key of the Scenario at once and
  is not available while a run holds the values. `read` keys are not
  changed by the operator.

## 7. System keys

The system writes three families of read-only keys: `node_outcome.<node>`
(each motion node's last outcome, `none` until it ends in this Run),
`interrupt_state.<rule_id>` (each Deployment global rule's state: `inactive`,
`clear` or `tripped`; no value while the rule can't watch its signals) and
`active_tcp.<target_role>` (the [Active TCP](TCPS.md#the-active-tcp) of the
robot bound to that role, as a `tcp_entry`). You never declare or write
these keys, and no key, namespace or Program parameter may use the names
`node_outcome`, `interrupt_state` or `active_tcp`. The first two values are
listed in [Interrupts](INTERRUPTS.md#6-system-keys-and-their-values).

`active_tcp` is a structured key. The runtime uses it for a move's **Current**
selector, and the run view displays it. Robot Control reads the robot's status.
Set TCP can also use
a `tcp_entry` key as its entry selector. An Active TCP value cannot be used in
an expression, computed key, Check/Wait condition, interrupt condition or
`bbkey` publication; those consumers remain scalar-only. If the robot has no
Active TCP, the key has no value and a node that needs it fails, naming the
key.

## 8. Editing in Composer

| Change | How it is applied |
| --- | --- |
| Create or delete a key; change its type, access, persistence, `initial:`, formula or `allowed_frames` | A Program edit: Draft, then **Apply** / **Discard** / **Revert** like any other Program change |
| Write a runtime value; set a stored value; **Reset to initial**; **Clear stored values** | A value edit: checked, applied immediately with **Confirm** / **Cancel**, and recorded |

If a node and the operator write the same key at nearly the same time, the
last write the runner accepted wins and both are recorded. A request based on
an outdated run state is refused with its reason, and a request sent twice is
applied only once.
