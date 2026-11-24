# Tool from parameters

The Tool model (`manyforge.tool`, family `tool`): a Tool is one component,
mounted as a whole on a robot's flange, made from its parameters
(`manyforge.parametric_tool.v1`, `kind: tool`). Chains of fixed elements lead
from its body to its tool ends (jaw pairs, spring sliders, probe tips); each
TCP belongs to a tool end or the body. Load generates its URDF and SRDF from
the parameters and derives its component type from them: the `tool` target
with every TCP, and one gripper target per jaw pair. The Deployment holds the
parameters (inline in `model_source.parameters`, or a parameter file it
names) and, per jaw pair, its wiring only. See
[Adding a component](../../../docs/authoring/ADDING_A_COMPONENT.md#a-tool-from-its-parameters).

Contents:
- `tool.urdf`, `tool.srdf`: the family's neutral placeholder (the model of
  `new_tool.tool.yaml`); a component of this family always has its own
  parameters.
- `component.yaml`: the component descriptor. Kind `tool`, no `gripper`
  section, `lanes: {}`.
- `new_tool.tool.yaml`: the parameters a new Tool starts from (Add
  component).
- Examples: `dual_jaw_gripper.tool.yaml` (two jaw pairs, one with pads),
  `spring_probe.tool.yaml` (a spring-loaded stylus) and `probe.tool.yaml`
  (a probe tip and a second TCP on the body).

    scripts/manyforge component export probe.tool.yaml <folder>

writes the generated `tool.urdf` and `tool.srdf` into `<folder>`; they name
each mesh where it is, by its path relative to `<folder>`. A mesh path in the
parameters is relative to the parameter file's folder (inline parameters: the
Scenario folder) or absolute; `..` is allowed, inside an allowed folder. USD
export is not available yet.
