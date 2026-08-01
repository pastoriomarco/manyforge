# Authoring a ManyForge scene from an Isaac Sim stage

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until I9/I and its prerequisite headless gates pass.
> Unless explicitly marked as retained standalone extractor detail,
> present-tense wording below describes the captured baseline, not a live
> Composer/viewer workflow.

This records the former operator runbook for taking a freshly-authored Isaac
Sim scene and producing the ManyForge artefacts (program YAML + static `.obj`
meshes) that drove Composer planning and the native viewer. The captured
workflow ran once per scene revision; the resulting OBJ files were static
inputs committed alongside the program YAML.

For the extractor's wire contract, filename rules, and OBJ format see
[`../../exts/manyforge.isaac.collider_service/README.md`](../../exts/manyforge.isaac.collider_service/README.md).

## Prerequisites

- Isaac Sim with ROS 2 Python support enabled.
- The `manyforge.isaac.collider_service` extension enabled — see the
  extension's README "Install" section. Confirm with
  `ros2 service list | grep manyforge`.
- A ManyForge checkout on the same network (or the same host) as Isaac.
- `ros2` reachable from the ManyForge host (either via a native ROS
  install or the `manyforge-ros-jazzy-full` Docker image).

Throughout this runbook the placeholder `${MANYFORGE_REPO_ROOT}` refers
to your checkout (for example `${HOME}/workspaces/dev_ws/src/manyforge`), and
`${SCENE_NAME}` is whatever short identifier you're giving the new
scene (e.g. `isaac_manipulator_scene_light`).

## 1. Confirm the extension sees your scene

With Isaac Sim running and your scene loaded, ask the extension for an
inventory:

```bash
source /opt/ros/jazzy/setup.bash
ros2 service call /manyforge/isaac_collider_report std_srvs/srv/Trigger '{}'
```

The response is a JSON string. Spot-check that the objects you expect
appear under `objects[].source_root`. If an object is missing:

- Check it actually has a collider authored in Isaac (the extension is
  collider-driven; visual-only meshes without a `MeshCollisionAPI` are
  skipped by default).
- Use a parameterised query (see the extension README "CLI Examples"
  section) with `include_hidden: true` and a wider `include_subtrees`
  if you've hidden the prim or it lives outside `/World`.
- Check the extension logs in Isaac Sim's Kit window for stage-walk
  errors.

Do **not** proceed if the inventory is incomplete; the meshes you
don't see here are the meshes that will be missing from your program.

## 2. Decide which mesh source you want per object

The extractor can produce up to four variants per object — visual mesh,
collider mesh, cooked convex collider, primitive collider — and the
`mesh_source_preference` request field picks between them:

- `auto` — visual when available, falling back to collider/cooked/primitive.
- `collider` — prefer cheap collider-derived geometry; this is usually
  the right choice for a planning scene because RoboPlan's collision
  cost scales with triangle count.
- `original` — prefer the authored visual mesh; pick this only when
  visual fidelity matters more than planner cost.
- `primitive` — force AABB/primitive recommendations and skip mesh
  transfer entirely.

If a collision body falls back to a conservative AABB proxy because mesh-like
geometry cannot be preserved or loaded, treat it as degraded collision geometry:
diagnostics should carry `conservative_proxy` and
`warning_conservative_aabb_proxy`, and you should inspect the collision overlay
before tuning poses around a reported contact.

For the current Isaac demo scenes we use `collider` per-object and
accept the fidelity trade-off. The decision is the operator's; the
file-naming convention in step 4 reflects what you pick (`*_collider.obj`
vs `*_visual.obj` etc.).

## 3. Pull mesh files onto the ManyForge host

The transfer client lives in `manyforge_ros`. From the ManyForge host
(either natively or in the ROS Docker image), run one transfer per
object you want to materialise. The `mesh_export_dir` argument is a
directory on the **Isaac host** where the extractor writes the OBJ;
the client then chunked-transfers the bytes back and reassembles them
into `--output-dir` on the **ManyForge host**.

Example, importing one object's collider variant into a program-local
mesh folder:

```bash
mkdir -p ${MANYFORGE_REPO_ROOT}/examples/${SCENE_NAME}/meshes

docker run --rm --network host \
  -v ${MANYFORGE_REPO_ROOT}:/workspace \
  -w /workspace manyforge-ros-jazzy-full \
  bash -lc 'source /opt/ros/jazzy/setup.bash;
    export PYTHONPATH=/workspace:/workspace/manyforge_ros:${PYTHONPATH};
    python3 -m manyforge_ros.isaac_mesh_transfer \
      --timeout 90 \
      --include-hidden \
      --purpose all \
      --source-root /World/<object_prim_path> \
      --mesh-source-preference collider \
      --mesh-export-dir /tmp/manyforge-isaac-meshes \
      --output-dir /workspace/examples/'${SCENE_NAME}'/meshes'
```

Repeat per object. The client verifies the full-file sha256 from the
chunked transfer header before writing the file out, so a partial or
corrupted transfer fails the client and leaves no garbage on disk.

If you already have a known local URI (because Isaac already wrote the
OBJ in a previous run), you can use `--mesh-resource-uri` instead of
`--source-root` to skip the export step.

## 4. Place files in the program directory

Convention: program-local meshes live at
`${MANYFORGE_REPO_ROOT}/examples/${SCENE_NAME}/meshes/`. The extractor
names them `<object_id_hint>_<variant>.obj` (for example
`mac_n_cheese_centered_collider_cooked.obj`). Keep the filenames as
shipped — the program YAML references them by path.

Commit these files. They are deterministic outputs of the pipeline
against a known scene revision; they should travel with the program
YAML so that operators on other hosts can load the scene without
re-running the extractor.

## 5. Write or update the program YAML

The program YAML's `scene` block references each mesh by local path.
Repo-root relative paths such as `examples/${SCENE_NAME}/meshes/...`
match the committed examples; paths relative to the program file's
directory are also accepted. The pose and `mesh_scale` come from the
inventory response — if you skip those, the mesh ends up in the wrong
place. Example shape:

```yaml
scene:
- object_id: isaac_cheese_box
  shape:
    type: mesh
    mesh_resource_uri: examples/${SCENE_NAME}/meshes/mac_n_cheese_centered_collider_cooked.obj
    mesh_scale:
    - 1.0
    - 1.0
    - 1.0
  pose:
    frame_id: universe
    position_m: [-0.455038, 0.076595, 0.087827]
    orientation_rpy_deg: [92.340746, -1.76826, -90.093258]
```

Cross-check the `position_m` and `orientation_rpy_deg` against the
inventory response — they're the pose the extractor returned, not a
guess. The path is relative to the program YAML's directory.

## 6. Smoke-test the load through Composer

From the ManyForge host:

```bash
cd ${MANYFORGE_REPO_ROOT}
./scripts/launch.sh   # interactive wizard; pick the isaac-sim lane
```

Once Composer is up, load your program (`POST /api/program/load` or
through the UI). Confirm three things:

1. **`/api/scene/state` reports each object** with the right pose and
   non-error `mesh_asset_status`.
1. **The native viewer renders the actual collider shape** — table,
   crates, cheese box — not orange wireframe placeholders. If you see
   placeholders, the visualization plan's error-marker tooltip will
   tell you which status the asset is in (`missing`,
   `parse_failed`, `unsupported_format`, etc.).
1. **A test plan succeeds** through the loaded scene — the planning
   context is reading the same OBJ files, so a successful plan
   confirms the geometry is reaching RoboPlan as well as the viewer.

## 7. Optional: stream object poses from Isaac

Static OBJ files define object shape. Object pose can also be observed
from Isaac at runtime through deployment `pose_sources`. In Composer,
open **Deployment > Object Pose Sources** to edit the source, entity
binding, and apply policy. The Isaac demo source uses
`kind: simulation_interfaces`, `service_prefix: auto`, `frame_id: universe`, and a bounded `max_update_hz` such as `4.0`.

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

Pick the apply policy per object:

- `observe_only` shows freshness and observed pose without changing
  ManyForge's planning scene.
- `when_idle` updates only while no cycle or direct-control session is
  active.
- `before_planning` latches the latest fresh pose just before planning
  an object-relative motion.
- `between_nodes` applies only at behavior-tree safe points.
- `continuous` applies fresh observations immediately at the configured
  source rate. If the object is attached, Composer updates the object's
  offset in the attachment frame instead of detaching it with a
  world-frame overwrite.
- `continuous_unattached` applies fresh observations at the configured
  source rate only while the object is detached. Once attached, the
  planning-scene object stays at the canonical attachment offset while
  the external observation remains available for ghost/drift diagnostics.

For industrial cells, keep `observe_only` as the default while
commissioning a source, then move individual objects to `before_planning`,
`between_nodes`, `continuous`, or `continuous_unattached` only when that
source is part of the cell authority model and its stale-data behavior is
understood.

For diagnostics, open **Tools > Sources** in the native viewer and enable
**Measured-state ghosts**. It renders fresh external object observations as
a cyan translucent overlay so you can compare Isaac/perception state with
Composer's backend-authoritative planning scene. Enable **Robot collision
meshes** in the same panel to overlay the URDF collision geometry used by
trajectory validation. These overlays are viewer-only; they do not modify
object poses or collision validation.

## 8. Qualify visual-state responsiveness during a live cycle

The native viewer should read Composer's published visual-state snapshot, not
the mutable planning scene. While Isaac and Composer are running, use the live
probe to record whether `/api/scene/state` stays on the snapshot path during
the pick/place cycle:

```bash
cd ${MANYFORGE_REPO_ROOT}

docker compose run --rm --network host dev bash -lc '
  python scripts/qualify-visual-state-live.py \
    --base-url http://127.0.0.1:9000 \
    --duration-s 90 \
    --hz 10 \
    --max-p95-ms 100 \
    --samples-jsonl /tmp/manyforge-visual-state-samples.jsonl \
    --summary-json /tmp/manyforge-visual-state-summary.json'
```

Run the probe while the browser or a scenario harness drives the program. It is
read-only; it does not start, stop, reset, or otherwise mutate the cell state.

Acceptance for the current migration:

1. `mode` is `visual_snapshot` for steady-state samples. A short
   `visual_snapshot_pending` window is acceptable immediately after reload or
   reset. `legacy_live` is no longer an accepted scene-state route mode.
1. p95 `/api/scene/state` latency remains under the configured budget while
   planning, validation, gripper motion, and pose-source polling are active.
1. `snapshot_publisher_error` remains empty and the stale ratio stays within
   the configured budget.
1. Object and gripper counts remain plausible for the loaded scene, so a fast
   route is not hiding missing observer data.

If the probe fails, keep the JSONL samples and Composer logs. The samples show
exactly when the viewer fell back, went stale, or exceeded the latency budget.

## Troubleshooting

Scene-authoring-specific cases below. For cross-cutting failures
(collision rejection, kernel interrupts, ros2_control discovery)
see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

| Symptom                                                      | Likely cause                                                                                    | Fix                                                                                                                               |
| ------------------------------------------------------------ | ----------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| Inventory response missing an object                         | Object has no collider, or it's hidden and you didn't pass `include_hidden`.                    | Author a collider in Isaac, or re-run with the right include flags.                                                               |
| Transfer client fails with `sha256 mismatch`                 | Network instability during the chunked transfer.                                                | Re-run the same command. `request_id` deduplication on the extension side handles overlapping retries safely.                     |
| OBJ file present but Composer shows a `missing` marker       | Path mismatch between the program YAML's `mesh_resource_uri` and where the file actually lives. | Check both — repo-root relative paths such as `examples/...`, program-directory relative paths, and `file://` paths are accepted. |
| OBJ file present, Composer shows `parse_failed`              | OBJ is malformed (very rare from this extractor).                                               | Re-extract from Isaac. If it keeps happening, file a bug — the extractor contract guarantees parseable output.                    |
| Composer shows `outside_allowed_roots`                       | The `mesh_resource_uri` resolves outside the deployment / program / model allowlist.            | Move the OBJ inside an allowed root (the program directory is always allowed).                                                    |
| Planner fails on the loaded scene but the viewer looks right | RoboPlan's collision tolerance flagged the geometry (zero-area triangle, self-intersection).    | Inspect the OBJ in MeshLab/Blender; clean the source mesh in Isaac and re-extract.                                                |

## See also

- [`../../exts/manyforge.isaac.collider_service/README.md`](../../exts/manyforge.isaac.collider_service/README.md)
  — extension install, ROS contract, JSON schemas, filename rules, and
  OBJ format contract.
- [`STACK_SETUP.md`](STACK_SETUP.md)
  — Isaac / ROS 2 controller execution bring-up.
