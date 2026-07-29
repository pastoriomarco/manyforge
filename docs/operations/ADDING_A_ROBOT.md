# Adding a robot (and optional gripper)

This is the operator's recipe for importing a new robot — or a new
robot+gripper pair — into ManyForge. It is written so both humans and
LLM agents can follow it without re-discovering the conventions.

> **Scope.** This document covers the *integration* steps: where files
> go, which fields in the deployment/program YAMLs have to be filled
> in, and what to verify before declaring the import done. It does not
> cover authoring a behavior tree, gripper-controller adapter
> development, or planner tuning — those have their own references.

> **Schema authority.** This public runtime guide mirrors the current
> program/deployment schema used by ManyForge. Maintainers changing
> schema-governed behavior must update the canonical private development
> specs first, then keep this operator guide and the implementation in sync.

> **Maintenance rule** — keep this file in sync with the deployment
> spec and `manyforge_behavior/manyforge_behavior/deployment.py`. If
> you add a new gripper `command_mode`, a new required field, or a new
> mesh-resolution rule, update the relevant section below in the same
> commit.

## TL;DR — what a clean import looks like

1. Drop your URDF + SRDF + mesh files under
   `manyforge_example_models/models/<robot_id>/` (or a separate package
   you reference via `package_paths`).
1. Author a deployment YAML at `examples/<scenario>.deployment.yaml`
   pointing at the URDF/SRDF, declaring the SRDF planning group, and
   (if applicable) the gripper block.
1. Author a program YAML at `examples/<scenario>.program.yaml`
   referencing the robot's `base_frame` + `tcp_frame`.
1. Load the scenario in Composer. The three checks in
   [§ Verify the import](#verify-the-import) tell you whether the import
   succeeded.

If those three checks pass, the rest is automatic: visual mesh loading,
mimic-joint hiding in the jog panel, FK + link poses, frame inventory,
per-joint Robot Control limits, named-frame defaults — all of it
flows from URDF metadata + the deployment YAML.

ManyForge treats the URDF/SRDF as the canonical robot description. Do
not create reduced "compatibility" URDF/SRDF variants or deployment-level
joint-coupling schemas to work around a parser. If a third-party backend needs
a private import view, ManyForge derives that view internally while keeping the
public joint names, gripper command joint, mimic laws, and fingerprints tied to
the canonical model.

## What auto-handles itself (do not re-author)

Everything in this list is derived from the URDF/SRDF + deployment
YAML at load time. You should not add code per-robot for these:

- **Visual rendering.** URDF `<visual>` blocks are converted to glTF
  per-link; cache-busted by a content-addressed `model_key`
  (`manyforge_composer/backend/routes_urdf.py`). Compound
  `<origin rpy="..." />` rotations go through a quaternion path so
  Three.js renders them correctly.
- **Mimic chains.** URDF `<mimic>` joints (e.g. Robotiq 2F-140's
  knuckle/finger joints chained to `finger_joint`, or a two-finger gripper
  where one prismatic finger follows the other) are auto-detected from the
  canonical model. FK, collision, and visualization still include the derived
  mimic joints.
- **Drivable-joint detection.** The jog panel lists the active SRDF planning
  group's commandable arm joints. Gripper command joints are driven through the
  gripper controls, not through the arm jog sliders; derived mimic joints are
  never directly joggable.
- **Frame inventory.** Every URDF link is a candidate frame. Frames
  named in the deployment YAML (robot base, program TCP, gripper
  mount/attach/TCP/auxiliary) are visible by default; URDF link
  frames default off and the operator can toggle them on.
- **Per-joint Robot Control limits.** When the deployment declares
  `robot_control_joint_limits`, the backend validates the joint names
  against the kernel's commandable joint set at load time and rejects
  unknown names with a structured error.

## Step 1 — Drop URDF, SRDF, and meshes

ManyForge resolves robot assets via the ROS package convention. The
simplest path is to vendor your model under `manyforge_example_models`:

```
manyforge_example_models/models/<robot_id>/
├── <robot_id>.urdf
├── <robot_id>.srdf
├── meshes/
│   ├── visual/      # <mesh filename="package://..."/> targets
│   └── collision/
├── licenses/
└── README.md        # provenance + license summary
```

Mesh references inside the URDF must use the `package://` form so they
work from both the Composer backend and the launcher:

```xml
<mesh filename="package://manyforge_example_models/models/my_robot/meshes/visual/upperarm.dae"/>
```

If your meshes live in a different ROS package (e.g. an upstream
`*_description` package), keep the `package://` URI and add the
package's parent directory to `package_paths` in the deployment YAML
(see Step 2).

**Supported mesh formats:** `.dae`, `.stl`, `.obj`, `.glb`. Anything
else `trimesh` rejects will return 404 from the mesh route and the
link will skip rendering (the rest of the robot still renders fine).

**Visual vs. collision.** The default robot display renders `<visual>`
geometry. For debugging, Composer's **Tools → Sources → Robot collision
meshes** overlay renders the URDF `<collision>` geometry separately so you can
compare what validation sees against the visible robot. SRDF disabled-collision
pairs and explicit `<collision>` blocks are consumed by the planner regardless
of whether the overlay is enabled.

## Step 2 — Author the deployment YAML

A deployment ties a URDF to a specific control configuration (planning
group, gripper, capability adapters). Clone the closest existing
deployment as your template:

```
examples/pick_and_place_ur10e_robotiq.deployment.yaml   # arm + parallel gripper
examples/pick_and_place.deployment.yaml                  # arm only
examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml   # ros2_control lane
```

Minimum-viable shape for a new arm + gripper:

```yaml
api_version: manyforge.deployment.v0
schema_version: "0.1.0"
name: my_robot_pick_place
description: "MyRobot + MyGripper preview."
program_profile: v0.1

robot_config:
  robot_id: my_robot                    # operator-facing identifier
  group_id: arm                         # SRDF planning group name (exact match)
  urdf_path: package://manyforge_example_models/models/my_robot/my_robot.urdf
  srdf_path: package://manyforge_example_models/models/my_robot/my_robot.srdf
  package_paths:
    - ..                                # relative to this YAML's directory
  global_max_tcp_linear_speed_mps: 0.25 # required; deployment-wide
                                        # ceiling on TCP linear speed
                                        # for all moves

  # Optional: per-joint caps bounding operator-authored Robot Control
  # targets (independent of planned-trajectory limits). Joint names MUST
  # be in the SRDF planning group above. Unknown names are rejected with
  # a DeploymentValidationError at load time.
  # robot_control_joint_limits:
  #   shoulder_pan_joint:    {max_velocity_rad_s: 0.5, max_acceleration_rad_s2: 1.0}
  #   shoulder_lift_joint:   {max_velocity_rad_s: 0.5, max_acceleration_rad_s2: 1.0}

grippers:
  primary:                              # logical id used by program.yaml
    command_mode: simulated_joint       # see § Gripper command modes
    group_id: gripper                   # SRDF planning group (if any)
    joint_name: finger_joint            # master joint of the mimic chain
    motion_time_s: 0.8
    touch_links:                        # links allowed to contact attached objects
      - left_inner_finger
      - right_inner_finger
    mount_frame: tool0                  # URDF link the gripper mounts on
    attach_frame: grasp_frame           # frame an object snaps to when grasped
    tcp_frame: grasp_frame              # gripper TCP (often same as attach)
    auxiliary_frames:                   # extra frames operators want visible
      - gripper_frame
    description: "Simulated parallel gripper."

adapters:
  planning:
    adapter_id: planning.roboplan
    adapter_version: "0.1.0"
    supported_profiles: ["v0.1"]
    capability_keys:
      - planning.motion.joint_goal
      - planning.motion.pose_goal
      - planning.trajectory.time_parameterized
    config: {rng_seed: 12345}
  # Add `context`, `execution`, etc. blocks from the template. Copy
  # them verbatim from a working example for the same lane and replace
  # only the robot-specific identifiers.
```

The execution adapter config must also declare controlled-stop capability
evidence for the lane. In particular, selecting `soft_on_path` in
`operator_stop_profile.stop_mode` is not enough: ros2_control / Isaac
deployments must enable the path-ramp fields and provide per-joint jerk/decel
limits, otherwise Composer reports `soft_on_path_not_enabled` and blocks Run.
Use [STOP_PROFILES.md](STOP_PROFILES.md) as the checklist when cloning or
creating a deployment.

### Field cheat-sheet

| Field                                  | Source                    | Notes                                                                                                                     |
| -------------------------------------- | ------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| `robot_config.group_id`                | SRDF `<group name="...">` | Must exist in SRDF. Determines the kernel's commandable joint set.                                                        |
| `robot_config.urdf_path` / `srdf_path` | filesystem                | Use `package://` URIs; fall back to absolute paths only as a last resort.                                                 |
| `robot_config.package_paths`           | filesystem                | Parent directories ROS package URIs resolve against. Relative paths are resolved against the deployment YAML's directory. |
| `grippers.<id>.command_mode`           | enum                      | See [§ Gripper command modes](#gripper-command-modes).                                                                    |
| `grippers.<id>.joint_name`             | URDF master joint         | The master of the mimic chain (or the single driven joint for parallel grippers without mimics).                          |
| `grippers.<id>.mount_frame`            | URDF link                 | Where the gripper attaches on the arm.                                                                                    |
| `grippers.<id>.attach_frame`           | URDF link                 | Where grasped objects snap to. Often a virtual frame added in URDF.                                                       |
| `grippers.<id>.tcp_frame`              | URDF link                 | Tool centre point. Used by Cartesian commands.                                                                            |
| `grippers.<id>.auxiliary_frames`       | URDF link list            | Operator-facing frames (default visible in the Tools→Frames panel).                                                       |
| `grippers.<id>.touch_links`            | URDF link list            | Links that may contact the grasped object without triggering a collision.                                                 |

### Gripper command modes

Allowed values (enforced at load by
`manyforge_behavior/manyforge_behavior/deployment.py`):

- `simulated_joint` — RoboPlan sim drives the master joint directly.
  Use this on the `manyforge-only` lane and for offline demos. The native
  gripper backend owns command progress and the status rendered by Composer.
- `ros_gripper_command` — actions are forwarded to a ROS gripper
  controller (e.g. `GripperActionController`). Use this on the
  `isaac-sim` lane and on real hardware. The selected execution/gripper adapter
  owns action acceptance, feedback/dwell, timeout, terminal result, and status.

The native viewer never advances gripper state from animation time. It renders
the authoritative gripper status plus Robot Model Authority mimic laws. If the
arm ghost moves but its gripper does not, verify that the deployment's gripper
is bound to the same selected execution backend/status surface and that the
status includes the declared command/derived joints.

A new gripper hardware family that does not fit either mode needs an explicit
adapter/transport capability, lifecycle mapping, contract tests, and a new
schema value. That is an SDK change, not a config-only change. Do not bypass it
with a viewer-side or raw-topic state writer.

## Step 3 — Author the program YAML

A program declares the behavior tree and references the robot's frames.
Clone `examples/pick_and_place_ur10e_robotiq.program.yaml` and replace:

```yaml
api_version: manyforge.program.v0
program_profile: v0.1
name: my_robot_pick_place_demo
description: "..."
catalog_version: 0.5.0  # exact value from manyforge_behavior/resources/node_catalog.yaml

robot:
  robot_id: my_robot                    # MUST match deployment.robot_config.robot_id
  group_id: arm                         # MUST match deployment.robot_config.group_id
  base_frame: base_link                 # URDF link
  tcp_frame: grasp_frame                # URDF link (or virtual frame added in URDF)

scene:
  - ...                                 # operator-authored scene primitives

parameters:
  - ...                                 # tunables exposed in the Params panel

tree:
  - ...                                 # behavior tree (see catalog)
```

The `robot.robot_id` and `robot.group_id` strings must match the
deployment exactly — the load step cross-validates them.
The `catalog_version` must likewise match the shipped node catalog exactly.
When starting from a cloned current example this value is already correct;
when authoring from scratch, copy it from
`manyforge_behavior/resources/node_catalog.yaml`.

## Step 4 — Verify the import

Launch Composer and load your scenario:

```bash
./scripts/launch.sh --lane manyforge-only --assistant off \
    --program examples/<scenario>.program.yaml \
    --deployment examples/<scenario>.deployment.yaml --yes
```

Then check these three signals in the browser at
`http://127.0.0.1:9000/` (the native viewer is the only product viewer):

1. **Robot panel shows the expected joint sliders.** Open the Robot
   overlay (top-right). The slider count should match the SRDF
   planning group's commandable arm joint count. Gripper command joints belong
   in the gripper controls; mimic followers must not appear as jog sliders. If
   a gripper joint shows up in the arm jog panel, see
   [§ Mimic chains and the jog panel](#mimic-chains-and-the-jog-panel).
1. **Tools → Frames lists only the named frames as visible.**
   Expected visible-by-default set:
   `universe` (world), `<base_frame>` (robot_base),
   `<gripper.mount_frame>`, `<gripper.attach_frame>`,
   `<gripper.tcp_frame>`, and each `gripper.auxiliary_frames` entry.
   All structural URDF link frames default off. A program may persist explicit
   overrides in its optional `viewer.frame_visibility` map; those saved values
   replace role defaults for the named frames when that program is loaded.
1. **Scene viewer renders the robot as one connected body.** No
   floating mesh pieces. The home pose should look kinematically
   coherent — base on the ground, joints lined up. If meshes float
   independently, see [§ Mesh and transform issues](#mesh-and-transform-issues).

If all three pass, the integration is clean.

## Verification API endpoints (machine-readable)

For automated verification or LLM agents, these endpoints expose the
same information the UI consumes:

```
GET /api/scene/state
    -> scene.robot.joint_names                 # commandable joints
    -> scene.robot.urdf_model_key              # content-addressed key
    -> scene.link_poses                        # FK output per URDF link

GET /api/scene/frames
    -> frames[*].name, roles, viewerVisible

GET /api/scene/urdf/manifest
    -> joints[*].name, type, limit_lower, limit_upper,
                   mimic (or null), is_mimic_master
    -> links[*].visuals[*].mesh_url, visual_origin.orientation_xyzw
```

Smoke-test script for a new robot:

```bash
# 1) Load
curl -sX POST http://localhost:9000/api/program/load \
  -H 'Content-Type: application/json' \
  -d '{"path":"/workspace/examples/<scenario>.program.yaml",
       "deploymentPath":"/workspace/examples/<scenario>.deployment.yaml",
       "forceDiscardOverrides": true}'

# 2) Commandable joints (should equal SRDF planning group)
curl -s http://localhost:9000/api/scene/state \
  | python3 -c "import json,sys; print(json.load(sys.stdin)['robot']['joint_names'])"

# 3) Drivable joints in the active arm jog panel
curl -s http://localhost:9000/api/scene/urdf/manifest \
  | python3 -c "import json,sys; m=json.load(sys.stdin); \
      print([j['name'] for j in m['joints'] \
             if j['type'] in {'revolute','prismatic','continuous'} \
             and not j.get('mimic') and not j.get('is_mimic_master')])"

# 4) Default-visible frames
curl -s http://localhost:9000/api/scene/frames \
  | python3 -c "import json,sys; \
      print([f['name'] for f in json.load(sys.stdin)['frames'] if f['viewerVisible']])"
```

## Troubleshooting

Robot-import-specific cases below. For cross-cutting runtime failures
see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).

### Mesh and transform issues

**Symptom: floating / disconnected mesh pieces.**
Almost always a compound `<origin rpy="..." />` on a `<visual>` block.
The backend extracts a quaternion via Shepperd's method and the
frontend binds `quaternion={...}` (never `rotation={...}`) on the
visual group. Confirm by GETting `/api/scene/urdf/manifest` and
inspecting `links[*].visuals[*].visual_origin.orientation_xyzw` for
the offending link — non-trivial values should be present and the
mesh should render correctly.

**Symptom: 404 on a mesh URL.**
Either the file is missing on disk, or `trimesh.load(...)` failed
(e.g. unsupported format). The backend logs `"Mesh file missing for link X"` or `"trimesh.load failed for ..."`. The rest of the robot
keeps rendering; the affected visual is silently dropped.

**Symptom: stale mesh after switching scenarios.**
Hard-refresh the browser. Mesh URLs are content-addressed by
`?model=<key>`, so a stale cache only matters across browser sessions.
If it persists after a hard-refresh, check that `scene.robot.urdf_model_key`
in `/api/scene/state` matches the `model_key` in `/api/scene/urdf/manifest`.

### Mimic chains and the jog panel

If a gripper joint shows up in the arm jog panel, first verify the loaded model
still declares the mimic relation in the canonical URDF and that the deployment
uses the arm SRDF group as `robot_config.group_id`, not the gripper group. The
gripper's command joint should be declared in `grippers.<id>.joint_name` and
driven by the gripper controls. Verify the mimic relation with:

```bash
python3 -c "
import xml.etree.ElementTree as ET
root = ET.parse('<path-to-your.urdf>').getroot()
for j in root.findall('joint'):
    m = j.find('mimic')
    if m is not None:
        print(j.get('name'), '-> master:', m.get('joint'))
"
```

If your gripper genuinely has **multiple independent driven joints** (e.g. a
three-finger gripper without mimic links), model those joints truthfully in the
URDF/SRDF and use a gripper command mode that supports the hardware contract.
Do not fake independence with mimic tags, and do not add deployment-level
coupling rules.

### Robot Control joint-limit errors

**Error: "robot_control_joint_limits references joint 'X' not in the SRDF
planning group".**
The deployment declares a per-joint cap for a joint that the kernel
cannot command. Check `robot_config.group_id` and that every name
under `robot_control_joint_limits` is in that SRDF group.

### Frame visibility unexpected

If a frame you expect to be visible is hidden by default, it is not
named in the deployment YAML's role table. Add it to one of:
`robot.tcp_frame`, `grippers.<id>.mount_frame` /
`attach_frame` / `tcp_frame` / `auxiliary_frames`.

If a frame you expect to be hidden is visible, the operator (or a
previous session) explicitly toggled it on. Reloading the program
with `forceDiscardOverrides: true` clears all per-frame overrides.

## Reference layout (where each piece lives in the code)

| Concern                                                             | File / module                                                                                  |
| ------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Canonical robot metadata, mimic laws, commandable-vs-derived joints | `manyforge_behavior/manyforge_behavior/robot_model_authority.py`                               |
| URDF parsing + manifest                                             | `manyforge_composer/backend/routes_urdf.py`                                                    |
| Mimic-joint detection                                               | `routes_urdf.py` `_extract_mimic`                                                              |
| Frame inventory + default visibility                                | `manyforge_composer/backend/routes_scene.py` (`_scene_frame_roles`, `_load_model_frame_names`) |
| Deployment YAML parser                                              | `manyforge_behavior/manyforge_behavior/deployment.py`                                          |
| Allowed gripper command modes                                       | `deployment.py` `_ALLOWED_GRIPPER_COMMAND_MODES`                                               |
| Robot Control limits validator                                      | `deployment.py` `validate_robot_control_joint_limits_against`                                  |
| Composer state owner                                                | `manyforge_composer/backend/state.py`                                                          |
| Launcher entry                                                      | `scripts/launch.sh`                                                                            |

## LLM agent checklist

A short machine-readable summary for an agent that has been asked to
"add robot X with gripper Y":

```
1. INPUTS REQUIRED FROM USER:
   - URDF file (path or URL)
   - SRDF file (path or URL)
   - Mesh files (or confirmation meshes are referenced via package://)
   - Planning group name (string, from SRDF)
   - Gripper master joint name (string, from URDF) — null if no gripper
   - Gripper command_mode: 'simulated_joint' | 'ros_gripper_command'

2. ACTIONS:
   - Create directory manyforge_example_models/models/<robot_id>/
   - Place URDF, SRDF, meshes; vendor a README.md with provenance + license
   - Author examples/<scenario>.deployment.yaml from the template in
     this doc, filling robot_id, group_id, urdf_path, srdf_path,
     package_paths, grippers (if any)
   - Author examples/<scenario>.program.yaml referencing robot_id,
     group_id, base_frame, tcp_frame

3. VERIFICATION (must pass before declaring done):
   - GET /api/scene/state -> robot.joint_names == SRDF planning group
   - GET /api/scene/urdf/manifest -> mimic chain detected and filtered
   - Viewer renders robot as one connected body (visual check)

4. DO NOT:
   - Hardcode the new robot anywhere in code (everything must flow
     from the YAML + URDF/SRDF)
   - Bypass package_paths by using absolute mesh URIs unless the
     model is genuinely outside any ROS package
   - Add a new gripper command_mode without an SDK change request

5. ASK THE USER IF:
   - The gripper has multiple independent driven joints (needs a
     dedicated widget, not the standard mimic-chain path)
   - The robot's joint count in SRDF planning group does not match
     the user's expectation (the SRDF, not the URDF, defines what is
     commandable)
   - Any verification step above fails
```
