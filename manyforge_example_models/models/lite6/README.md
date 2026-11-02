# UFACTORY Lite 6 (arm)

The arm: every link from the root to the flange. Its `flange` frame role
(`link_eef`) is the attachment interface a tool mounts on.

Derived from the combined bundle `../lite6_lite_gripper` by
`scripts/models/cut_combined_model.py` (robot/tool split), cut at
the fixed joint `gripper_fix`. The tool half is `../lite_gripper`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.urdf \
  --srdf manyforge_example_models/models/lite6_lite_gripper/lite6_lite_gripper.srdf \
  --mount-joint gripper_fix \
  --arm-name lite6 --arm-out manyforge_example_models/models/lite6/lite6 \
  --tool-name lite_gripper --tool-out manyforge_example_models/models/lite_gripper/lite_gripper
```

Contents:
- `lite6.urdf`, `lite6.srdf`
- `component.yaml`: the component descriptor. Kind `robot` (`ufactory.lite6`), lanes `mock`, `isaac` and `real`. It is placed only as the arm member of its machine `ufactory.lite6_machine` (`components/machine/`), on every lane; the real lane runs the xArm vendor adapter (`manyforge_xarm_control/vendor/README.md`).

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount. Apply disables collisions
between the two rigid link groups that meet at a mount; any other arm-tool
pairs that never touch are listed on the Deployment's attachment row
(`collision_exclusions`; Components page: **Collision exclusions with parent**).

Meshes are not copied: the mesh URIs keep pointing into `../lite6_lite_gripper/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../lite6_lite_gripper/README.md` and `../lite6_lite_gripper/licenses/`
(`xarm_ros2.LICENSE`).
