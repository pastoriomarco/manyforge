> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Expressions

Language version: **ManyForge expression language v1.** This page and the
conformance file
[`manyforge_deployment/resources/expression/conformance.v1.yaml`](../../manyforge_deployment/resources/expression/conformance.v1.yaml)
together define v1; where they differ, the conformance file wins. A change to
the language is a new version, never a silent change to v1. How expressions
relate to blackboard keys is in [Blackboard keys](BLACKBOARD_KEYS.md).

## 1. What an expression is

An expression computes one value from other values, inside the Program, so a
formula is written once and never repeated elsewhere (for example in an HMI).

- An expression **only computes**. It never writes anything, never calls
  hardware and has no side effects.
- Writing a computed result into a blackboard key is done by the **Set value**
  node.

Typical uses:

- a part dimension that drives several poses:
  `-param.tube_length / 2 + param.grasp_offset`;
- palletizing: `(key.index % param.columns) * param.pitch_x`;
- a condition: `key.measured_force > param.limit && !key.override_enabled`.

## 2. Where expressions are used

1. **Computed key.** A blackboard key declared with `expression:` instead of
   `initial:`. Its value is always the expression's result; nodes and the
   operator cannot write it.

   ```yaml
   blackboard:
     keys:
       - key: grasp_z
         type: float64
         access: read
         persistence: run
         expression: "-param.tube_length / 2 + param.grasp_offset"
   ```

2. **Field expression.** Any single number inside a node parameter (for
   example one coordinate of a pose or an offset) can be an expression instead
   of a literal. Stored as described in section 9; shown as `f(x)` in the
   editor (section 10).

3. **Set value node.** Its value can be an expression, for example
   `key.index + 1` for a counter.

## 3. Values and types

| Type | Written as | Notes |
| --- | --- | --- |
| `bool` | `true`, `false` | |
| `int64` | `42`, `-3` | 64-bit signed integer |
| `float64` | `0.25`, `1e-3`, `2.0` | 64-bit IEEE number; must stay finite |
| `string` | `"lite6_arm"` | double quotes; only `\"` and `\\` escapes |
| pose (read only) | a pose key or parameter | fields in section 5 |

Strict rules, with no hidden conversions:

- Numbers are written strictly: no leading zeros, no `.5` or `1.`, no unary
  `+`; the most negative `int64` is written `-9223372036854775807 - 1`.
- `int64` and `float64` may be mixed; the result is `float64`.
- `bool` never converts to or from a number; `string` never converts to or
  from anything.
- A whole pose is never the result of an expression; only its numeric fields
  can be read.

## 4. Names

Every reference names its source explicitly:

| Form | Refers to |
| --- | --- |
| `param.<name>` | a Program parameter, for example `param.tube_length` |
| `key.<name>` | a blackboard key, for example `key.index`, `key.perception.grasp_plan` |

- Names are spelled exactly as declared in the same Program.
- Dotted key names (`perception.grasp_plan`) are matched by the **longest
  declared key name**; anything after it is a field (section 5).
- An unknown name is an error.
- Reading a key that has no value yet (no `initial:` and not yet written) fails
  the node, naming the key.

## 5. Pose fields (read only)

For a pose `P` (for example `key.part_pose`), values are read **in the pose's
own frame** (the frame written in the pose: the cell, a robot frame or a
Scene object). Expressions never convert between frames.

| Field | Meaning | Unit |
| --- | --- | --- |
| `P.x`, `P.y`, `P.z` | position | metres |
| `P.roll`, `P.pitch`, `P.yaw` | orientation as roll/pitch/yaw | degrees |
| `P.qx`, `P.qy`, `P.qz`, `P.qw` | orientation as a quaternion | — |

A pose stores exactly one orientation form: `rpy_deg` (the usual authoring
form) or `xyzw` (for example a measured robot pose). Both views above are
computed when read and never stored. Reading `roll`/`pitch`/`yaw` from an
`rpy_deg` pose returns exactly the stored numbers; from an `xyzw` pose it is a
conversion, so at pitch ±90° roll and yaw are not unique and angles are
wrapped to −180°…180°. For trigonometry, convert first: `sin(rad(key.p.yaw))`.
An `xyzw` value must be a unit quaternion within 1e-6, otherwise reading the
pose fails; `qx`…`qw` return the stored numbers.

A formula or binding can replace a single orientation number only in an
`rpy_deg` pose; quaternion components depend on each other and are typed or
bound as a whole.

Keys with declared record fields (for example a planning result) expose those
fields by name: `key.perception.grasp_plan.approach_pose.z`.

## 6. Operators

Highest precedence first. Operators on one line group left to right, except
`? :`, which groups right to left.

| Precedence | Operators | Operand types | Result |
| --- | --- | --- | --- |
| 1 | `( … )`, function call, `.field` | | |
| 2 | unary `-`, `!` | number; bool | same type; bool |
| 3 | `*`, `/`, `%` | numbers | `/` always `float64`; `%` integers only |
| 4 | `+`, `-` | numbers | `int64` if both `int64`, else `float64` |
| 5 | `<`, `<=`, `>`, `>=` | numbers | bool |
| 6 | `==`, `!=` | two values of the same type, or two numbers | bool |
| 7 | `&&` | bools | bool, stops early |
| 8 | `\|\|` | bools | bool, stops early |
| 9 | `cond ? a : b` | bool; `a` and `b` of the same type, or two numbers | that type; `float64` when one number is `float64` |

- `==` on `float64` compares exactly; for a tolerance write
  `abs(a - b) <= tol`. An `int64` and a `float64` compare by exact value,
  without rounding the integer.
- Errors, never silent results: `int64` overflow in
  any operation (including `-`, `abs`, `floor`, `ceil`, `round`); division or
  `%` by zero, including by `0.0`; `%` with a negative operand; a non-finite
  result.
- A constant part that always fails is reported while editing even where it
  could not be reached (for example `false && 1 / 0 > 0`).
- There is no assignment, no string concatenation, no loops and no
  user-defined functions.

## 7. Functions

| Function | What it does | Example → result |
| --- | --- | --- |
| `abs(x)` | Distance from zero: drops the minus sign. Result has the same type as `x`. | `abs(-0.02)` → `0.02` |
| `min(a, b)` | The smaller of two numbers. | `min(3, 7)` → `3` |
| `max(a, b)` | The larger of two numbers. | `max(3, 7)` → `7` |
| `clamp(x, lo, hi)` | Keeps `x` inside the range `lo`–`hi`: below `lo` gives `lo`, above `hi` gives `hi`. Error if `lo > hi`. | `clamp(1.4, 0, 1)` → `1` |
| `floor(x)` | Rounds down to the whole number at or below `x`; result is `int64`. | `floor(2.7)` → `2`, `floor(-2.2)` → `-3` |
| `ceil(x)` | Rounds up to the whole number at or above `x`; result is `int64`. | `ceil(2.1)` → `3` |
| `round(x)` | Rounds to the nearest whole number; exact halves go away from zero. Result is `int64`. | `round(2.5)` → `3`, `round(-2.5)` → `-3` |
| `sqrt(x)` | Square root; result is `float64`. Error if `x < 0`. | `sqrt(9)` → `3.0` |
| `sin(x)`, `cos(x)`, `tan(x)` | Sine, cosine and tangent of an angle **in radians**. | `cos(0)` → `1.0` |
| `atan2(y, x)` | The angle, in radians, of the direction from the origin to the point (`x`, `y`); between −π and π. | `atan2(1, 1)` → `0.785…` (45°) |
| `deg(x)` | Converts radians to degrees. | `deg(3.14159…)` → `180.0` |
| `rad(x)` | Converts degrees to radians. | `rad(90)` → `1.5707…` |

`%` (remainder, section 6) gives what is left after dividing whole numbers:
`7 % 3` → `1`. With `floor(a / b)` it splits a counter into columns, rows and
layers (section 12).

The list is closed; adding a function is a spec change.

## 8. Checking and evaluation

**While editing** (backend validation; the page shows the diagnostics):

- syntax error, with the character position;
- unknown `param.`/`key.` name;
- wrong type for the destination (for example a `bool` result for a number
  field, or a number result for a condition);
- a cycle between computed keys (`a` uses `b`, `b` uses `a`);
- a constant part that already fails, such as `1 / 0` or `sqrt(-1)`;
- limits: at most 1024 characters, 64 levels of nesting, 512 tokens, 256
  syntax-tree nodes and 256 evaluation steps.

**While running:**

- all expressions a node needs are evaluated **once, when the node attempt
  starts**, from one snapshot of parameters and keys, so a node never sees a
  half-updated set of values;
- exception: **Wait for value** re-reads the keys it watches on every tick,
  from a fresh atomic snapshot, so it can see the write it is waiting for;
- computed keys are evaluated within that same snapshot; a computed key that
  would fail matters only if the expression actually reads it (an untaken
  `? :` branch or a short-circuited `&&`/`||` does not fail);
- a run-time failure (division by zero, non-finite result, `int64` overflow,
  missing key value) fails that node with reason
  `expression_evaluation_failed`, naming the expression and the cause; after
  the inputs are fixed, Retry evaluates again.

## 9. Storage in the Program file

Where a value comes from is recorded in the node's `value_sources`, keyed by
the value's **path**. An entry can say that the value comes from a
parameter, from a key, or from a formula. The value itself keeps its normal
shape; the literal stored there is the one restored when the source is
removed.

```yaml
parameters:
  goal:
    position_m: [0.30, 0.10, 0.0]
    orientation: {xyzw: [1.0, 0.0, 0.0, 0.0]}
  target_reference:
    type: pose
    pose: {position_m: [0.0, 0.0, 0.0], orientation: {xyzw: [0, 0, 0, 1]}}
value_sources:
  goal.position_m[2]:
    kind: expression
    expression: "-param.tube_length / 2 + param.grasp_offset"
  target_reference.pose:
    kind: blackboard_key
    reference: fixture_pose
```

- A path starts at the parameter name and uses `.name` for fields and
  `[index]` for list positions; a path of just the parameter name sources the
  whole parameter. Names are lowercase letters, digits, `_` and `-`, starting
  with a letter; indices have no leading zeros.
- Kinds: `program_parameter`, `blackboard_key` and `blackboard_field` (a
  whole value of matching type, the last one read from a key's declared
  field) and `expression` (a single number, or a bool or string where the
  field is one; formulas are not accepted for prompt-template variables).
- A source overrides the value typed at its path without deleting it;
  removing the source shows the typed value again.
- A map entry (for example one prompt-template variable) can be supplied by
  a source alone, without a typed value.
- Entries cannot overlap: a value bound as a whole cannot also have entries
  for its own fields.
- Some values never come from keys or formulas: which robot a node moves and
  its base and TCP frames, providers and other selections, and names of
  keys. A pose's own frame and a reference frame may come from a key, within
  the frames the Program declares (and the key's `allowed_frames`, if set).
- An orientation may take single numbers from formulas or keys only in
  roll/pitch/yaw form (`orientation.rpy_deg[i]`); a quaternion is typed or
  bound as a whole.
- The validators refuse a path that no longer names a value of the right
  type. Inserting, deleting or reordering list items in the editor (for
  example Cartesian path waypoints) moves their entries with them.

## 10. Editor behavior

- A number field computed by an expression shows an **`f(x)` marker**, and
  its content is shown as read-only preview text rather than an editable
  number.
- The marker sits inside the field, at its trailing edge, so switching
  between a typed value and a formula never moves the layout. Long formulas
  and chips are shortened with an ellipsis; the full formula and its last
  preview value are in the tooltip.
- **Keyboard:** the standard context-menu keys (**Shift+F10** or the
  **Menu** key) open the same menu on the focused field; arrows, Enter and
  Escape work inside it. Keyboard access adds nothing visible.
- **Right-click** on any value (a whole pose, a nested pose, or a single
  number) offers where it comes from: **Type values**, **Use key…**,
  **Use parameter…** (only type-matching choices) and, for numbers,
  **Add formula** / **Edit formula** / **Remove formula**. Returning to
  **Type values** restores the stored literal.
- A whole value taken from a key or parameter is shown as **one compact
  chip** in the same space as its fields (for example `key.my_pose`), with
  the current value in the tooltip; clicking it changes the choice. Fields of
  one pose may mix sources, each showing its own marker.
- A pose editor shows the other orientation form read-only under the one
  being edited (roll/pitch/yaw under a quaternion, and the reverse).
  Switching the stored form converts the current orientation exactly; it
  never resets the orientation. While a roll/pitch/yaw number has a formula
  or key, switching to a quaternion is disabled.
- **Clicking `f(x)`** opens the formula editor: a text box with the backend's
  diagnostics shown under it, at their position, while typing; the result
  type; and a preview value computed from parameter defaults and key
  `initial:` values when they exist, labelled "preview from authoring
  values", never shown as a live runtime value. Saving is disabled while there are
  errors.
- Computed keys are edited the same way in Params, next to the key.
- The editor lists available `param.` and `key.` names with their types, to
  insert without typing.

## 11. Where expressions are evaluated

One engine evaluates expressions everywhere: when the Program is checked in
Composer, when a Deployment is prepared, and when the Program runs. So a
formula that checks while editing behaves the same at run time. Errors carry
a code, a message and the character position they refer to.

## 12. Worked examples

| Goal | Expression |
| --- | --- |
| grasp height from tube length | `-param.tube_length / 2 + param.grasp_offset` |
| palletizing column offset | `(key.index % param.columns) * param.pitch_x` |
| palletizing row offset | `floor(key.index / param.columns) % param.rows * param.pitch_y` |
| palletizing layer height | `floor(key.index / (param.columns * param.rows)) * param.layer_height` |
| next counter value (Set value) | `key.index + 1` |
| approach 2 cm above an object | `key.part_pose.z + 0.02` |
| choose a speed | `key.fragile ? param.slow_speed : param.normal_speed` |
| force check | `key.measured_force > param.limit && !key.override_enabled` |
| angle from a degree parameter | `rad(param.approach_angle_deg)` |
