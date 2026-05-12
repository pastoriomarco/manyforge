# ManyForge Isaac Collider Service

Native Isaac Sim extension that exposes the ManyForge collider probe over ROS 2.

The extension is read-only. It inspects the currently loaded Isaac Sim stage and
returns a JSON collider report suitable for ManyForge scene-authoring tools and
assistant workflows.

## Requirements

- Isaac Sim with ROS 2 Python support available in the Isaac Python
  environment.
- `rclpy`, `std_msgs`, and `std_srvs` importable from Isaac Sim Python.
- The `manyforge.isaac.collider_service` extension folder. The probe library is
  bundled in the extension package for copied installs.

## Install

Use the repository `exts` directory as an Isaac Sim extension search path:

```text
/path/to/manyforge/exts
```

For a copied install, this shape is enough:

```text
${HOME}/manyforge-isaac-exts/
  manyforge.isaac.collider_service/
```

Add `${HOME}/manyforge-isaac-exts` as the Isaac Sim extension search path.

Then enable:

```text
manyforge.isaac.collider_service
```

Equivalent launch shape:

```bash
./isaac-sim.sh \
  --ext-folder /path/to/manyforge/exts \
  --enable manyforge.isaac.collider_service
```

For normal copied installs, no environment variable is needed. The extension
first looks for the bundled `isaac_collider_probe_lib.py`.

Advanced override, only if you want to force a different probe library:

```bash
export MANYFORGE_PROBE_SCRIPTS_DIR=/path/to/manyforge/scripts
```

## ROS Endpoints

Default report service:

```text
/manyforge/isaac_collider_report
std_srvs/srv/Trigger
```

The Trigger request has no input fields. It returns the configured
`default_query` report in `response.message`.

Parameterized query topics:

```text
/manyforge/isaac_collider_query/request
std_msgs/msg/String

/manyforge/isaac_collider_query/response
std_msgs/msg/String
```

The topic route is a request/response RPC using versioned JSON strings. It
exists because ROS 2 core packages do not provide a standard string-in /
string-out service type. A future production interface may replace this with a
ManyForge-specific service definition if that becomes preferable.

Mesh-transfer topics:

```text
/manyforge/isaac_mesh_transfer/request
std_msgs/msg/String

/manyforge/isaac_mesh_transfer/status
std_msgs/msg/String

/manyforge/isaac_mesh_transfer/chunk
std_msgs/msg/UInt8MultiArray
```

Mesh transfer deliberately uses only standard ROS interfaces. The request and
status topics carry versioned JSON. Each chunk message is self-describing:
`UInt8MultiArray.data` contains a 4-byte big-endian JSON-header length, the JSON
header, then raw mesh bytes. The header includes `request_id`, chunk index/count,
byte offsets, total sha256, and per-chunk sha256.

## Query JSON

```json
{
  "version": "manyforge.isaac.collider_query.v0.1",
  "request_id": "optional-correlation-id",
  "include_subtrees": ["/World"],
  "exclude_subtrees": ["/World/ur10e_robotiq2f_140_ROS"],
  "probe_prims": ["/World/ur10e_robotiq2f_140_ROS"],
  "include_hidden": false,
  "include_cooked_convex": true,
  "include_cooked_samples": false,
  "max_colliders": 200,
  "purpose": ["default", "proxy"],
  "output_profile": "llm_scene_authoring",
  "mesh_export_dir": "/tmp/manyforge-isaac-meshes",
  "mesh_source_preference": "collider"
}
```

Allowed `output_profile` values:

- `llm_scene_authoring`
- `raw_inventory`

Allowed `purpose` values:

- `default`
- `render`
- `proxy`
- `guide`
- `all`

All fields are validated strictly. Invalid JSON, invalid field types, and
unsupported enum values return an error response instead of being silently
coerced.

## Mesh Transfer JSON

Transfer an already exported local mesh URI:

```json
{
  "version": "manyforge.isaac.mesh_transfer_request.v0.1",
  "request_id": "mesh-1",
  "mesh_resource_uri": "file:///tmp/manyforge-isaac-meshes/soup.obj",
  "chunk_size": 262144
}
```

Or ask Isaac to select/export the mesh from a scene root and stream it back:

```json
{
  "version": "manyforge.isaac.mesh_transfer_request.v0.1",
  "request_id": "mesh-2",
  "source_root": "/World/soup_can",
  "mesh_export_dir": "/tmp/manyforge-isaac-meshes",
  "mesh_source_preference": "collider",
  "include_hidden": true,
  "purpose": ["all"],
  "chunk_size": 262144
}
```

The request must provide exactly one of `mesh_resource_uri` or `source_root`.
Only local `file://` and plain local paths are transferable by this extension;
remote asset references should first be exported by Isaac into a local OBJ.
When a source root is converted to a ManyForge recommendation, the response
includes the mesh pose and `mesh_scale` derived from the Isaac transform. This
matters for Xform parents such as scaled tables where the OBJ vertices are
exported root-local and the runtime must apply the authored scale separately.

## Collider Vs Visual Mesh Sources

ManyForge can import either a visual/object mesh or a collider-derived mesh.
Use `mesh_source_preference` to make that choice explicit:

- `original` prefers the authored visual/object mesh. This is useful when the
  Composer scene should look like Isaac and planning cost is not the priority.
- `collider` ignores visual meshes when a collider-derived export is available.
  This is the normal choice for planning scenes because Isaac collider
  approximations are usually much cheaper than render meshes.
- `primitive` forces primitive/AABB recommendations and avoids mesh transfer.
- `auto` prefers visual/object meshes when available and falls back to collider
  or primitive data.

For `collider`, the extension tries sources in this order:

1. A directly authored collider mesh prim under the requested source root.
2. PhysX cooked convex data from the authored collider prim.
3. PhysX direct cooked convex cache data for that collider prim.
4. A temporary descendant-mesh recook for assets that author collision settings
   on an `Xform` while storing the actual `UsdGeom.Mesh` on a child prim.
5. Primitive collider geometry, when the collider is a cube/sphere/cylinder.

The temporary recook path is intentionally not a scene edit. It creates a
temporary `__manyforge_probe_cooking_*` prim, copies the descendant mesh geometry
and convex-collider settings, asks PhysX for the cooked representation, exports
the result, and removes the temporary prim before returning. It exists for Isaac
assets whose UI shows a collider approximation such as `Convex Decomposition`
on an `Xform`, while the PhysX cooking API rejects that Xform with
`RESULT_ERROR_INVALID_PARSING` because the cookable mesh geometry lives below
it.

When this path is used, raw reports identify it explicitly:

```json
{
  "cooked_convex": {
    "available": true,
    "result": "TEMPORARY_DESCENDANT_MESH_RECOOK",
    "source_path": "/__manyforge_probe_cooking_mac_n_cheese_centered/collision_mesh"
  }
}
```

The exported OBJ still uses the logical source root pose. A ManyForge scene
resource should reference the copied OBJ from the program folder and keep the
pose and `mesh_scale` returned by the report or mesh-transfer status.

Example, importing the collider approximation for an Isaac object into a
program-local mesh folder:

```bash
docker run --rm --network host \
  -v /home/tndlux/workspaces/dev_ws/src/manyforge:/workspace \
  -w /workspace manyforge-ros-jazzy-full \
  bash -lc 'source /opt/ros/jazzy/setup.bash;
    export PYTHONPATH=/workspace:/workspace/manyforge_ros:${PYTHONPATH};
    python3 -m manyforge_ros.isaac_mesh_transfer \
      --timeout 90 \
      --include-hidden \
      --purpose all \
      --source-root /mac_n_cheese_centered \
      --mesh-source-preference collider \
      --mesh-export-dir /tmp/manyforge-isaac-meshes \
      --output-dir /workspace/examples/isaac_manipulator_scene_light/meshes'
```

For the current Isaac light scene, this produces
`mac_n_cheese_centered_collider_cooked.obj` from the cheese-box collider
approximation. In the validated scene this collider is a compact cooked convex
mesh with 25 vertices and 46 triangular faces, instead of the 12,502-vertex
visual mesh.

## CLI Examples

Call the default Trigger service:

```bash
source /opt/ros/jazzy/setup.bash
ros2 service call /manyforge/isaac_collider_report std_srvs/srv/Trigger '{}'
```

Send a parameterized request:

```bash
source /opt/ros/jazzy/setup.bash
ros2 topic echo /manyforge/isaac_collider_query/response
```

```bash
source /opt/ros/jazzy/setup.bash
ros2 topic pub --once /manyforge/isaac_collider_query/request std_msgs/msg/String \
  "{data: '{\"version\":\"manyforge.isaac.collider_query.v0.1\",\"request_id\":\"probe-1\",\"exclude_subtrees\":[\"/World/ur10e_robotiq2f_140_ROS\"],\"probe_prims\":[\"/World/ur10e_robotiq2f_140_ROS\"],\"include_hidden\":false,\"include_cooked_convex\":true,\"output_profile\":\"llm_scene_authoring\"}'}"
```

The response `data` field is JSON.

## Operational Notes

- USD and PhysX access runs on the Isaac/Kit update thread.
- ROS callbacks only enqueue requests and wait for the Kit-thread result.
- The extension does not call global `rclpy.shutdown()` on unload, so it does
  not tear down other ROS users inside Isaac Sim.
- Large scenes can produce large JSON strings. Tooling should parse and
  summarize the response, not expose raw payloads directly to users by default.
