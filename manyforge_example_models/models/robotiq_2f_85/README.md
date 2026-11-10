# Robotiq 2F-85

The tool: every link below `robotiq_85_base_joint`, under a new root link `robotiq_2f_85_mount`.
`robotiq_2f_85_mount` is the robot flange plane; `robotiq_85_base_joint` keeps its original
origin, now relative to `robotiq_2f_85_mount`. Mounted on a robot's `flange` with an
identity pose, every link sits exactly where the combined model had it.

The default TCP is `robotiq_2f_85_mount`: it is today's UR5e `tcp`
(`tool0`), so captured poses keep their meaning. The maintained standalone
Robotiq 2F-85 source (`manyforge_deployment/resources/maintained/model_sources/robotiq_2f_85`)
has the same links and joints but no mount: its root is
`robotiq_85_base_link`, 1 cm from `tool0`. It stays the gripper-only test
fixture; this cut bundle is the maintained `robotiq_2f_85` model family.

Derived from the combined bundle `../ur5e_robotiq_2f_85` by
`scripts/models/cut_combined_model.py` (robot/tool split), cut at
the fixed joint `robotiq_85_base_joint`. The arm half is `../ur5e`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.urdf \
  --srdf manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.srdf \
  --mount-joint robotiq_85_base_joint \
  --arm-name ur5e --arm-out manyforge_example_models/models/ur5e/ur5e \
  --tool-name robotiq_2f_85 --tool-out manyforge_example_models/models/robotiq_2f_85/robotiq_2f_85
```

Contents:
- `robotiq_2f_85.urdf`, `robotiq_2f_85.srdf`
- `component.yaml`: the component descriptor. Kind `tool` (`robotiq.robotiq_2f_85`), lane `mock`.
- `mock_controllers.yaml`: the controllers of its Mock lane row (`controllers_file`).

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount. Apply disables collisions
between the two rigid link groups that meet at a mount; any other arm-tool
pairs that never touch are listed on the Deployment's attachment row
(`collision_exclusions`; Components page: **Collision exclusions with parent**).

Meshes are not copied: the mesh URIs keep pointing into `../ur5e_robotiq_2f_85/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../ur5e_robotiq_2f_85/README.md` and `../ur5e_robotiq_2f_85/licenses/`
(`ros2_robotiq_gripper.LICENSE`).
