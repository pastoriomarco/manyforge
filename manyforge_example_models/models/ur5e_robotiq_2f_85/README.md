# UR5e + Robotiq 2F-85

Self-contained, generation-time-expanded ManyForge model. The runtime does
not depend on ROS, Xacro, MoveIt, or the upstream repositories.

Sources and licenses:

- UR5e structure, physical parameters, limits and meshes: official
  `Universal_Robots_ROS2_Description` at `89bbe795f38a7ab00fb66fe8831dfff79dc99edf`; see
  `licenses/Universal_Robots_ROS2_Description.LICENSE`.
- Robotiq 2F-85 structure and meshes: NVIDIA Isaac ROS
  `ros2_robotiq_gripper` at `587b6548640e366d50322068021e20b487372a75`; see
  `licenses/ros2_robotiq_gripper.LICENSE`.
- Base SRDF collision semantics: ManyMove at `f3a4c65e154744403d721d6ef25d2a8ea5f3f1e3`; see
  `licenses/manymove.LICENSE`. The ordinary gripper group and open/closed
  states are added here because the ManyMove SRDF controls only the arm.

The arm TCP remains the source-compatible `tool0` used by ManyMove. The
Robotiq mount frame is separately exposed as `robotiq_85_base_link`; a
site-calibrated grasp TCP can therefore be introduced without changing the
vendor geometry. Exact expanded upstream XML is retained unchanged under
`upstream_source/`.

Documented divergences in the deployable projection:

- The primary URDF normalizes XML and mesh URIs. It changes the four Robotiq
  mimic followers from `continuous` to bounded `revolute` joints with limits
  derived exactly from the existing master joint and mimic multiplier. This
  preserves their motion while making the bounded mimic chain valid in
  Pinocchio. Links, inertials, visuals and collision geometry are unchanged.
- The primary SRDF adds the Robotiq gripper group, open/closed states and
  end-effector declaration. ManyMove's arm groups and collision semantics are
  unchanged.
- The prepared structural view removes two direct `collision/surface`
  runtime-physics extensions. It retains each collision and its geometry and
  records the removal count in the preparation receipt.

`STATIC_MANIFEST.json` binds all retained source, license and mesh bytes.
Redistribution must retain the notices above.
