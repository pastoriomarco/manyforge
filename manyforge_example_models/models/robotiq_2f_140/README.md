# Robotiq 2F-140

The tool: every link below `gripper_base_joint`, under a new root link `robotiq_2f_140_mount`.
`robotiq_2f_140_mount` is the robot flange plane; `gripper_base_joint` keeps its original
origin, now relative to `robotiq_2f_140_mount`. Mounted on a robot's `flange` with an
identity pose, every link sits exactly where the combined model had it.

Derived from the combined bundle `../ur10e_robotiq_2f_140` by
`scripts/models/cut_combined_model.py` (merge 6, robot/gripper split), cut at
the fixed joint `gripper_base_joint`. The arm half is `../ur10e`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.urdf \
  --srdf manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.structural.srdf \
  --mount-joint gripper_base_joint \
  --arm-name ur10e --arm-out manyforge_example_models/models/ur10e/ur10e \
  --tool-name robotiq_2f_140 --tool-out manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140.structural
```
(and the same command on the non-structural pair for `robotiq_2f_140.urdf`/`.srdf`; the arm output of both runs is identical)

Contents:
- `robotiq_2f_140.urdf`, `robotiq_2f_140.srdf`
- `robotiq_2f_140.structural.urdf`, `robotiq_2f_140.structural.srdf`: the
  same cut of the combined `.structural` pair. They differ from the files
  above only by the two collision `<material>` children and the HPP
  `<gripper>` element, which are this tool's (see the combined README).
- `component.yaml`: the component descriptor.

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount; Apply disables collisions
between the two rigid link groups that meet at a mount instead.

Meshes are not copied: the mesh URIs keep pointing into `../ur10e_robotiq_2f_140/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../ur10e_robotiq_2f_140/README.md` and `../ur10e_robotiq_2f_140/licenses/`
(`ros2_robotiq_gripper.LICENSE`).
