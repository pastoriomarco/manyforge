# UFACTORY xArm gripper

The tool: every link below `gripper_fix`, under a new root link `xarm_gripper_mount`.
`xarm_gripper_mount` is the robot flange plane; `gripper_fix` keeps its original
origin, now relative to `xarm_gripper_mount`. Mounted on a robot's `flange` with an
identity pose, every link sits exactly where the combined model had it.

Derived from the combined bundle `../uf850_xarm_gripper` by
`scripts/models/cut_combined_model.py` (robot/tool split), cut at
the fixed joint `gripper_fix`. The arm half is `../uf850`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.urdf \
  --srdf manyforge_example_models/models/uf850_xarm_gripper/uf850_xarm_gripper.srdf \
  --mount-joint gripper_fix \
  --arm-name uf850 --arm-out manyforge_example_models/models/uf850/uf850 \
  --tool-name xarm_gripper --tool-out manyforge_example_models/models/xarm_gripper/xarm_gripper
```

Contents:
- `xarm_gripper.urdf`, `xarm_gripper.srdf`
- `component.yaml`: the component descriptor. Kind `tool` (`ufactory.xarm_gripper`), lane `mock`.
- `mock_controllers.yaml`: the controllers of its Mock lane row (`controllers_file`).

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount. Apply disables collisions
between the two rigid link groups that meet at a mount; any other arm-tool
pairs that never touch are listed on the Deployment's attachment row
(`collision_exclusions`; Components page: **Collision exclusions with parent**).

Meshes are not copied: the mesh URIs keep pointing into `../uf850_xarm_gripper/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../uf850_xarm_gripper/README.md` and `../uf850_xarm_gripper/licenses/`
(`xarm_ros2.LICENSE`).
