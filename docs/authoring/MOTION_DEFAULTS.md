# Motion defaults

A Program sets its speed and path settings once, as **motion defaults**, and
its motion nodes inherit them. This page covers how nodes inherit them, how to
override them per node, speeds that change while the Program runs, and what
copying nodes keeps.

## 1. The four motion-default fields

These node fields can inherit a Program value:

| Field | Meaning | Ceiling |
| --- | --- | --- |
| `velocity_scale` | Velocity scaling, above 0 up to 1 | Yes |
| `acceleration_scale` | Acceleration scaling, above 0 up to 1 | Yes |
| `translation_tol_m` | Cartesian translation tolerance, in metres | No |
| `blend_radius_m` | Corner blend radius, in metres (Follow Cartesian path only) | No |

The motion nodes that carry them are Move to joint target, Move to pose,
Follow Cartesian path, Move until signal and Probe search.

Fields with the same name inside a node's other settings are separate values.
They never inherit: for example a waypoint's own `blend_radius_m`, or the pick
motion options' `acceleration_scale` and `translation_tol_m`.

## 2. Inheriting the Program value

A motion default is a Program parameter **named exactly like the field**, for
example a `float64` parameter `velocity_scale`. When a node field inherits, it
reads that parameter (`value_sources: {velocity_scale: {kind:
program_parameter, reference: velocity_scale}}`).

- A Program parameter with another name, for example `cycle_velocity`, is not
  a motion default. A node can still read it as a dynamic source (section 4).
- When the Program has no parameter of that name, the node uses its own value,
  or the catalog default when it sets none.
- When the Program has the parameter, each node field must either inherit it
  or set its own value. Program validation reports a field that does neither.

**Program settings → Motion defaults** edits the declared motion defaults
together. Each field shows its parameter and how many nodes inherit it. A field
whose parameter the Program does not declare is shown as such and has no
input; the other fields stay editable. **Update motion defaults** changes the
declared values in one edit, with Undo and Redo.

## 3. Overriding a value on one node

Set a literal on the node to override the Program value. In the node
properties, clearing the literal returns the field to **Use Program default**
when the Program declares the parameter.

`velocity_scale` and `acceleration_scale` are ceilings: a literal may not be
above the Program value. When **Update motion defaults** lowers a ceiling
below some node literals, Composer lists them and asks whether to reduce them
to the new value or cancel.

## 4. Speeds that change while the Program runs

A motion-default field can read any value source, like other node fields: a
blackboard key, a key field, an expression, or another Program parameter. Use
this for a speed that a node or the operator sets during the run.

- The source must give a number. Program validation checks the type.
- When the move starts, its value is checked. If it is out of the field's
  range (for a scale: above 0 up to 1), the node fails with
  `node_input_unavailable`. If a `velocity_scale` or `acceleration_scale`
  value is above the Program parameter of the same name, the node fails with
  `motion_value_above_program_limit`. In both cases the move is refused before
  any motion; the value is never reduced silently.
- **Motion defaults** shows how many nodes read such a source as "dynamic".
  Updating the Program value does not change them.

## 5. Adding, replacing and copying nodes

Adding a node keeps every value and source it comes with, at every depth.
Only its unset motion-default fields inherit, on the node and on all its
children. The same applies when you replace a subtree, change a node's kind,
or insert a Pick or Place subtree. New nodes from the palette, and a node
whose kind you change, start with the four fields unset, so they inherit.

**Copy**, **Paste** and **Duplicate** make an exact copy except for names:

- Each copied node gets a new name (`pick_approach` becomes
  `pick_approach_2`).
- A reference from one copied node to another is renamed with it: a node
  outcome key `node_outcome.<node>`, in a key setting, a value source or an
  expression (`key.node_outcome.<node>`).
- References to nodes outside the copy, and all blackboard keys, stay as they
  were.
