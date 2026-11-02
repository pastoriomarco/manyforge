# UF850 + xArm Gripper

Self-contained, generation-time-expanded ManyForge model. Loading it needs
no ROS, Xacro, MoveIt, `uf_ros_lib`, or xarm_ros2.

This bundle has no `component.yaml` and isn't placed in a Deployment: it is
the source of the split models `../uf850` (the arm) and `../xarm_gripper` (the
tool), which are placed as separate components with the tool mounted on the
arm's `flange`. It holds their meshes, licenses and upstream sources.

The arm, standard xArm gripper, physical parameters, kinematics, MoveIt joint
limits and meshes come from `https://github.com/pastoriomarco/xarm_ros2`,
branch `jazzy_no_gazebo`, commit `012731afb9ef1d4289ec45112e8d083ab59a5c1e` with SDK submodule
`2b6b595a956c2558e867c08001fda454212b271a`. See `licenses/xarm_ros2.LICENSE` (BSD 3-Clause). The
upstream `xarm_description/package.xml` still carries a placeholder license
field, so this bundle records and retains the repository-root license that
governs the source tree instead of repeating that incomplete metadata.

The model uses the same `robot_type=uf850`, `dof=6`, `limited=true`,
`add_gripper=true` configuration as the ManyMove UF850 example. The upstream
gripper link group and `close` state are retained; its existing mimic joints
are made explicit group members and a ManyForge-compatible `closed` alias is
added. Exact expanded upstream XML is retained unchanged under
`upstream_source/`.

Documented divergences in the deployable projection:

- The primary URDF has only normalized XML serialization and localized mesh
  URIs. Kinematics, physical data, visuals and collision geometry are unchanged.
- The primary SRDF adds the five existing URDF mimic joints to the gripper
  group and adds `closed` as an alias for the upstream `close` state.
  `drive_joint` remains the only commanded joint; no joint or mimic law is
  introduced.
- The prepared structural view removes top-level Gazebo, `ros2_control` and
  transmission extensions plus one exact duplicate unordered SRDF
  collision-disable row. It retains structural geometry and unique collision
  semantics and records every removal in the preparation receipt.

`STATIC_MANIFEST.json` binds all retained source, license, limit and mesh
bytes. Redistribution must retain the notice above.
