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
  "output_profile": "llm_scene_authoring"
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
