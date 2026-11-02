# Using an Isaac Sim stage as a ManyForge scene

How to mirror objects from an Isaac Sim stage in a ManyForge Scene, and how to
stream their poses from Isaac at runtime. The bundled
`examples/sim_ur10e_pick_and_place.scenario` is a complete example: its Program
scene uses the OBJ meshes in `examples/isaac_manipulator_scene_light/meshes/`.

ManyForge does not include a tool that exports meshes from Isaac Sim. Export
each object's mesh from your stage with your own tools, then reference the
files from the Program scene as below.

## 1. Place the mesh files

Put the mesh files inside the Scenario project (for example under its
`assets/` folder) or in another folder Composer can read: the repository
`examples/` tree or a configured project folder (see
[`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md#project-folders)).
Supported formats are `.obj`, `.stl`, `.ply`, `.glb` and `.gltf`. For a
planning scene, prefer simple collider geometry over detailed visual meshes:
collision-check cost grows with triangle count.

## 2. Add the objects to the Program scene

In Composer's Scene workspace, add a mesh object per Isaac object and set its
mesh path, scale and pose. Take the pose from the Isaac stage (position in
metres, orientation as roll/pitch/yaw in degrees) in the frame the Scene uses.
In `program.yaml` an object looks like this (from the bundled example):

```json
{
  "scene_resource_id": "isaac_cheese_box",
  "pose": {
    "frame_ref": {"kind": "cell_frame"},
    "position_m": [-0.455038, 0.076595, 0.087827],
    "orientation": {"rpy_deg": [92.340746, -1.76826, -90.093258]}
  },
  "shape": {
    "type": "mesh",
    "mesh_resource_uri": "examples/isaac_manipulator_scene_light/meshes/mac_n_cheese_centered_collider_cooked.obj",
    "mesh_scale": [1.0, 1.0, 1.0]
  }
}
```

## 3. Check the scene in Composer

Start Isaac Sim yourself and open the stage with its ROS 2 bridge; the
workbench reaches it over DDS
([ROS and DDS](DEVELOPMENT_WORKBENCH.md#ros-and-dds)) and does not start it.
From the ManyForge repository root, start the workbench (see
[`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)):

```bash
./scripts/compose-dev.sh start
```

Open `http://localhost:3000`, then use **Load Scenario** for the Scenario that
binds this Program and Deployment. Confirm:

1. **The viewer renders each mesh** at the Isaac pose. A mesh that cannot be
   shown is marked with its status (`missing`, `parse_failed`,
   `unsupported_format`, `outside_allowed_roots`, …).
1. **A test plan succeeds** through the loaded scene: planning reads the same
   mesh files, so a successful plan confirms the geometry reaches the planner.

## 4. Optional: stream object poses from Isaac

Static OBJ files define object shape. Object pose can also be observed
from Isaac at runtime through a pose-source component of the Deployment. In
Composer, each object's **Object pose source** card selects the apply policy
for its source/object binding through normal Draft, Save and Apply. The Isaac
pose source in `examples/sim_ur10e_pick_and_place.scenario` polls the simulator
every 250 ms (`simulation.poll_period_ms`) in frame `universe`.

For camera/perception scenes such as the Lite6 trocar FoundationPose workflow,
also configure each Isaac Sim `ROS2CameraHelper` publisher with bounded
sensor-data QoS before playback:

```json
{"history":"keepLast","depth":1,"reliability":"bestEffort","durability":"volatile","deadline":0.0,"lifespan":0.0,"liveliness":"systemDefault","leaseDuration":0.0}
```

Set `queueSize` to `1` on the RGB image, depth image, and camera info helpers.
Use a conservative `frameSkipCount` while commissioning over a shared LAN. See
[`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)
for the full multi-machine DDS/QoS/perception runbook.

Pick the apply policy per object (the object's
`policies.scene_pose_sources` row in the Deployment):

- `observe_only` shows freshness and observed pose without changing
  ManyForge's planning scene.
- `before_planning` latches fresh configured detached poses at the planning
  boundary. Missing required observations block planning.
- `between_nodes` latches objects referenced by the next motion or pick;
  reset readiness latches the full configured set. It may also revise an
  attached object's offset. These captures require fresh observations and
  then retain a planning snapshot without continuous source expiry.
- `continuous` applies fresh observations immediately at the configured
  source rate. If the object is attached, the Scene owner updates the object's
  offset in the attachment frame instead of detaching it with a
  world-frame overwrite.
- `continuous_unattached` applies fresh observations at the configured
  source rate only while the object is detached. Once attached, the
  planning-scene object stays at the canonical attachment offset while
  the external observation remains available for ghost/drift diagnostics.
- `continuous_relative_to_attached` behaves like `continuous_unattached`
  while the object is detached. While it is attached, the source must also
  report the object relative to a gripper reference measured at the same
  source instant (`simulation.relative_reference` on the pose-source
  component: the reference `entity`, its `source_header_frame_id`, and the
  model `link` it shows). The Scene owner sets the attachment offset from that relative
  pose and the fixed model transform from the link to the carrier frame; no
  commanded or measured robot pose is used. The Isaac observer reads both
  in the same simulator read; an observation that is not from one
  instant, or a link that is not joined to the carrier frame by fixed joints
  only, is refused and does not update the offset.

For industrial cells, keep `observe_only` as the default while
commissioning a source, then move individual objects to `before_planning`,
`between_nodes`, `continuous`, `continuous_unattached`, or
`continuous_relative_to_attached` only when that
source is part of the cell authority model and its stale-data behavior is
understood.

The primary viewer always uses the authoritative Scene, including while
objects are detached. For diagnostics, enable **Measured state ghosts** in the
viewer. It renders fresh external object observations as a translucent overlay
so you can compare Isaac/perception state with Composer's backend-authoritative
planning scene. Enable **Collision geometry** to show the collision shapes used
by trajectory validation. These overlays are viewer-only; they do not modify
object poses or collision validation.

With an outside measured robot source (the pose observer's measured robot),
the ghost uses that source for the robot's joints, mapped to the arm through
the component index. Other robots and tools use their own joint or position
streams. A static fixture or any other element without a stream gets no
ghost. If data needed to place an element has not arrived yet, that element is
hidden rather than drawn at a neutral pose as if measured; unrelated elements
continue to render. The Scene panel shows one line per component, for example
"Parts of lite_gripper are hidden in the measured overlay until their joint
state arrives." With no live robot position at all it shows "Measured robot
overlay requires execution."

## Troubleshooting

Scene-specific cases below. For cross-cutting failures (collision rejection,
controller discovery) see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| Mesh file present but Composer shows `missing` | Path mismatch between `mesh_resource_uri` and the file location. | Check the path; repository-relative paths such as `examples/...` and paths inside the Scenario project are accepted. |
| Composer shows `parse_failed` | The mesh file is malformed. | Re-export it and check it in a mesh viewer. |
| Composer shows `unsupported_format` | The file type is not supported (for example `.dae`). | Convert it to `.obj`, `.stl`, `.ply`, `.glb` or `.gltf`. |
| Composer shows `outside_allowed_roots` | The `mesh_resource_uri` resolves outside the folders Composer can read. | Move the file into the Scenario project, `examples/`, or a configured project folder. |
| Planner fails on the loaded scene but the viewer looks right | Degenerate geometry (zero-area triangles, self-intersection) or a genuine collision. | Inspect and clean the mesh, then re-export. |

## See also

- [`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)
  — multi-machine Isaac + perception runbook.
- [`STACK_SETUP.md`](STACK_SETUP.md) — lanes and setup overview.
