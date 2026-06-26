# ManyMove Lite6 Runtime Capture

This directory is qualification evidence for the vendored
`lite6_lite_gripper` static ManyForge model.

Source runtime:

- ManyMove image: `manymove-xarm:jazzy`
- xarm fork: `https://github.com/pastoriomarco/xarm_ros2`
- xarm branch: `jazzy_no_gazebo`
- runtime owner: `/move_group`
- launch:

```bash
ros2 launch manymove_bringup lite_foundationpose_movegroup_fake_cpp_trees.launch.py \
  no_gui_ctrl:=true
```

Artifacts:

- `manymove_lite6_runtime.urdf` and `manymove_lite6_runtime.srdf` are the
  captured `/move_group` `robot_description` and `robot_description_semantic`.
- `manymove_lite6_runtime_capture.json` records capture metadata, including the
  ManyMove image, launch command, xarm fork branch, and exact xarm commit. The
  non-live qualification gate validates this metadata before accepting the
  runtime URDF/SRDF comparison.
  Recapture reads the commit from `/opt/manymove_ws/.xarm_ros2_commit` in the
  ManyMove xArm container, or from the checked-out `xarm_ros2` git tree. Use an
  explicit `--xarm-commit` only when intentionally capturing from another
  layout.
- `compare_report.json` records the comparison result against the vendored
  ManyForge snapshot.
- `manymove_lite6_launch.log` is the launch/capture log. The Qt no-display HMI
  warning in that log is expected for this headless capture and does not affect
  `/move_group` description capture.

The comparison is intentionally not a byte-for-byte XML diff. The canonical
ManyForge snapshot is allowed to rewrite mesh URIs into the vendored
`manyforge_example_models` package and add the `closed` gripper-state alias used
by ManyForge UI language. It must preserve the captured runtime links, joints,
sibling-finger mimic, Gazebo tags, ros2_control tags, transmissions, explicit
Lite6 SRDF joint group, both-finger gripper SRDF group, limited runtime arm
bounds, open/close presets, disable-collision intent, and vendored mesh closure.
It must not add a synthetic `base_link -> link_base` alias, strip mimic from
the canonical URDF/SRDF, or require a separate stripped compatibility model.
