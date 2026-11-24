# Generic passive tool

The one generic passive tool family (`manyforge.passive_tool`, family
`passive_tool`): a tool with nothing to drive, such as a probe, a tool body
without fingers or a part holder, brought as the project's own URDF. It has a
mount frame, a TCP frame and an optional tip only: no joints, groups, gripper
section or controller. Its components name no lane and run on every lane
(Mock, Isaac, Real) on the host's generic adapter. A tool described by
parameters instead uses the Tool model (`../tool/`).

The model in this folder is a neutral placeholder (a cylinder 0.1 m long, a
small tip ball and the TCP on its end). A project supplies its own tool
through the Deployment row's `model_source`: its own URDF and meshes; see
[Adding a component](../../../docs/authoring/ADDING_A_COMPONENT.md#a-projects-own-passive-tool).

Contents:
- `passive_tool.urdf`: the placeholder. The family binds three links, which a
  project's own URDF must also have (the tip only when the tool has one):
  `passive_tool_mount` (the root link, the robot flange plane),
  `passive_tool_tcp` (the default TCP) and `passive_tool_tip` (the tip).
- `passive_tool.srdf`: no groups or states; the robot name and the tip
  touching the body (`disable_collisions`).
- `component.yaml`: the component descriptor. Kind `tool`, no `gripper`
  section, `lanes: {}`, the collision body `tip`, optional
  (`roles.optional_collision_bodies`).

## Tip and collisions

Every link with `<collision>` geometry is checked. The tip is the collision
body `tip` and the default TCP's contact body: the part a geometric contact
input senses. A tool without a tip leaves `tip` unbound and has no contact
body. All other collision links form the tool's `default_collision` body.
Parts that touch by design need exclusions: tool and robot links in the
Deployment's `collision_exclusions`, the tool's own links in the project's
SRDF.

The files are ManyForge's own (Apache-2.0); the placeholder has no meshes
and no mass. This folder is its own package (`model.package_name:
passive_tool`): the family's `accepted_package_uri_prefix` is
`package://passive_tool/`, and every mesh URI of a project's model starts
with it, its package root the project's tool folder. A project's tool mass
and centre of mass come from its URDF's `<inertial>` elements.
