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

Chunk header shape:

```json
{
  "version": "manyforge.isaac.mesh_transfer_chunk.v0.1",
  "request_id": "mesh-1",
  "chunk_index": 0,
  "chunk_count": 3,
  "offset": 0,
  "data_size": 262144,
  "total_bytes": 712384,
  "sha256": "whole-file-sha256",
  "payload_sha256": "this-chunk-sha256",
  "filename": "World__soup_can_collider_cooked.obj",
  "mesh_resource_uri": "file:///tmp/manyforge-isaac-meshes/World__soup_can_collider_cooked.obj",
  "source_root": "/World/soup_can",
  "mesh_source": "exported_cooked_convex_collider_mesh",
  "mesh_source_prim_path": "/World/soup_can/Cylinder"
}
```

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

- `llm_scene_authoring` — compact, operator-facing object records meant
  to become ManyForge scene resources. Use this for normal scene
  authoring and assistant-assisted draft generation.
- `raw_inventory` — lower-level inspection dump that preserves more
  USD/PhysX detail for debugging the extractor itself. Use this when
  an object is missing or the compact profile hid information needed to
  diagnose source authoring.

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

## How the extractor builds OBJ files

When a query or transfer request supplies a `mesh_export_dir`, the extension
walks the USD subtree under each logical object root and produces up to four
variants per object (`exported_original_mesh`, `exported_collider_mesh`,
`exported_cooked_convex_collider`, `exported_primitive_collider`; see
"Collider Vs Visual Mesh Sources" above for selection). The extractor entry
point is `_export_original_mesh_obj` in `isaac_collider_probe_lib.py`; the
cooked-collider and primitive paths follow the same shape.

For each `UsdGeom.Mesh` prim in the subtree the extractor reads:

- `GetPointsAttr()` — array of `Gf.Vec3f` vertex positions in the prim's
  local frame.
- `GetFaceVertexCountsAttr()` — per-face vertex counts (3 for a triangle,
  4 for a quad, etc.).
- `GetFaceVertexIndicesAttr()` — flat index buffer into the vertex array,
  laid out face-by-face according to `face_counts`.

Each point is transformed by the prim's world matrix and then by the
inverse of the logical-object-root world matrix, producing **root-local**
vertices. This is why the response carries `pose` and `mesh_scale`
separately: the OBJ is deliberately authored in the object's own frame,
and the program YAML re-applies the object's pose/scale at scene-load
time. Tools that ignore the returned pose/scale will render the mesh at
the wrong place.

Polygons are written out as triangles via simple fan triangulation: for
a polygon `[v0, v1, v2, ..., vN-1]` the extractor emits the triangles
`(v0, v1, v2)`, `(v0, v2, v3)`, …, `(v0, vN-2, vN-1)`. Degenerate
polygons with fewer than three vertices are skipped. This is correct
for the convex polygons USD `UsdGeom.Mesh` faces effectively are after
USD pre-processing; non-convex polygons would render with crossed
triangles, but no producer in the current Isaac authoring path emits
those.

The OBJ file is written as a single text pass with three line types: a
comment header, one `v x y z` per vertex (9 significant digits), one
`f a b c` per triangle (1-indexed per the OBJ spec). Indices are
contiguous within the file even when the source spans multiple
`UsdGeom.Mesh` prims; the extractor accumulates a single vertex pool
and offsets indices across prims.

## OBJ format contract

Downstream consumers (RoboPlan's planning context, the Composer
visualizer's trimesh → GLB conversion) MAY rely on the following
guarantees about the OBJ files this extension writes:

- **Positions-only.** `v x y z` lines for vertices, `f a b c` lines for
  triangles, plus a single `# ManyForge ...` comment header. No `vn`
  normals, no `vt` texture coordinates, no `usemtl` / `mtllib` directives,
  no associated `.mtl` file. Consumers that need normals MUST compute
  them from the vertex / face data.
- **Triangulated.** Every face directive is a 3-vertex triangle. No
  quads, no n-gons, no `f a/b/c` index triplets — only `f a b c`.
- **1-indexed, contiguous.** Vertex indices in `f` lines follow the OBJ
  specification (start at 1). When multiple `UsdGeom.Mesh` prims
  contribute to one logical object, their vertex pools are concatenated
  and indices rebased so a single file is internally consistent.
- **Root-local coordinates.** Vertices are baked into the
  logical-object-root local frame. The object's world placement is
  published separately in the inventory / transfer response as `pose`
  (position + orientation) and `mesh_scale`. Program YAML authors MUST
  carry both into the scene resource declaration.
- **UTF-8, LF line endings.** Files are written with
  `mesh_path.write_text(..., encoding="utf-8")`; no trailing CR
  characters. Trimesh, MeshLab, and Blender all accept this; producers
  downstream that assume Windows CRLF do not.
- **Deterministic across re-runs against the same scene revision.** The
  extractor reads USD attributes in stage-iteration order with no
  shuffling. Two extractions of the same unedited stage produce
  byte-identical OBJ bytes; the Composer visualizer's content-addressed
  cache relies on this.

What the contract intentionally does NOT promise:

- Mesh decimation or simplification. The extractor writes the raw
  points and triangles from USD. If you need fewer triangles, decimate
  downstream.
- Sanitisation against malformed USD authoring (zero-area triangles,
  duplicated vertices, degenerate normals). The OBJ file faithfully
  reflects what was in the stage.
- A stable filename across different `filename_suffix` values. The four
  variants land at `<object_id_hint>_visual.obj`,
  `<...>_collider.obj`, `<...>_collider_cooked.obj`, and
  `<...>_collider_primitives.obj` respectively. If the authored mesh
  changes shape (visual ↔ collider) between runs, the filename
  selection changes with it.

The `<object_id_hint>` is derived from the logical source-root path by
stripping leading/trailing slashes, replacing path separators with
double underscores, and replacing remaining non-portable characters
with underscores. For example, `/World/small_KLT_01` becomes
`World__small_KLT_01`, yielding filenames such as
`World__small_KLT_01_visual.obj` and
`World__small_KLT_01_collider_primitives.obj`.

## See also

- End-to-end pipeline reference (Isaac stage → ManyForge program YAML →
  visualizer): `manyforge_specs/docs/reference/ISAAC_MESH_PIPELINE.md`.
- Operator runbook for re-extracting against a new Isaac scene:
  `manyforge/docs/operations/ISAAC_SCENE_AUTHORING.md`.
