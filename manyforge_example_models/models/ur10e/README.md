# Universal Robots UR10e (arm)

The arm: every link from the root to the flange. Its `flange` frame role
(`tool0`) is the attachment interface a tool mounts on.
`tool0` is the UR tool flange (same origin as `flange`, tool-oriented axes)
and is where the combined model fixed the gripper, so the cut keeps it.

Derived from the combined bundle `../ur10e_robotiq_2f_140` by
`scripts/models/cut_combined_model.py` (merge 6, robot/gripper split), cut at
the fixed joint `gripper_base_joint`. The tool half is `../robotiq_2f_140`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.urdf \
  --srdf manyforge_example_models/models/ur10e_robotiq_2f_140/ur10e_robotiq_2f_140.srdf \
  --mount-joint gripper_base_joint \
  --arm-name ur10e --arm-out manyforge_example_models/models/ur10e/ur10e \
  --tool-name robotiq_2f_140 --tool-out manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140
```
(and the same command on the non-structural pair for `robotiq_2f_140.urdf`/`.srdf`; the arm output of both runs is identical)

Contents:
- `ur10e.urdf`, `ur10e.srdf`
- `component.yaml`: the component descriptor.

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount; Apply disables collisions
between the two rigid link groups that meet at a mount instead.

Meshes are not copied: the mesh URIs keep pointing into `../ur10e_robotiq_2f_140/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../ur10e_robotiq_2f_140/README.md` and `../ur10e_robotiq_2f_140/licenses/`
(`Universal_Robots_ROS2_Description.LICENSE`, `ros2_robotiq_gripper.LICENSE`).
