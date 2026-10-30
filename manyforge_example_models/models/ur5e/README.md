# Universal Robots UR5e (arm)

The arm: every link from the root to the flange. Its `flange` frame role
(`tool0`) is the attachment interface a tool mounts on.
`tool0` is the UR tool flange (same origin as `flange`, tool-oriented axes)
and is where the combined model fixed the gripper, so the cut keeps it.

Derived from the combined bundle `../ur5e_robotiq_2f_85` by
`scripts/models/cut_combined_model.py` (merge 6, robot/gripper split), cut at
the fixed joint `robotiq_85_base_joint`. The tool half is `../robotiq_2f_85`.

```bash
python3 scripts/models/cut_combined_model.py \
  --urdf manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.urdf \
  --srdf manyforge_example_models/models/ur5e_robotiq_2f_85/ur5e_robotiq_2f_85.srdf \
  --mount-joint robotiq_85_base_joint \
  --arm-name ur5e --arm-out manyforge_example_models/models/ur5e/ur5e \
  --tool-name robotiq_2f_85 --tool-out manyforge_example_models/models/robotiq_2f_85/robotiq_2f_85
```

Contents:
- `ur5e.urdf`, `ur5e.srdf`
- `component.yaml`: the component descriptor.

Element text, geometry, origins, limits and vendor tags are copied unchanged;
only `<robot name>` and the mount joint's parent link differ. The cut drops the
combined SRDF's collision pairs that cross the mount; Apply disables collisions
between the two rigid link groups that meet at a mount instead.

Meshes are not copied: the mesh URIs keep pointing into `../ur5e_robotiq_2f_85/meshes/`,
and the model family's `accepted_package_uri_prefix` and `resource_root` name
that folder. Upstream sources, revisions and licenses are recorded in
`../ur5e_robotiq_2f_85/README.md` and `../ur5e_robotiq_2f_85/licenses/`
(`Universal_Robots_ROS2_Description.LICENSE`, `manymove.LICENSE`).
