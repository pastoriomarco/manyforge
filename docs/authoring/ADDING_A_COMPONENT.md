# Adding a component

A component is a folder with one `component.yaml`, its **descriptor**: a robot,
a tool or an I/O device described as data. One command family creates, checks
and lists component folders:

| Command | What it does |
| --- | --- |
| `scripts/manyforge component new <robot\|tool\|io_device> <vendor>.<name>` | Creates the folder from a template |
| `scripts/manyforge component check <folder> [--scenario <scenario>]` | Checks the descriptor, its model files and, for a robot or tool, the rows Apply reads |
| `scripts/manyforge component register <folder>` | Writes a robot's or tool's rows from its descriptor |
| `scripts/manyforge component list [--json]` | Lists the curated components, each checked |

Run them in the workbench container from the repository root; ROS is never
installed on the host. Each problem prints as one line,
`error: <where>: <what> [<code>]`, and the exit status is 1. Nothing is
written except by `new` and `register`.

What each kind does today:

- **Robot** (an arm): runs on the
  **Mock lane** once registered and generated (steps below). Apply reads the
  registered rows, not the descriptor. Isaac and Real robots still need code.
- **I/O device** (`io_device`): its signals, the values of a Deployment's I/O
  device component (`io_device_v0`). Checked and listed.
- **Tool** (a gripper or a passive tool): its own component, mounted on a
  robot's `flange`. Once registered and generated it runs as a commanded Mock
  gripper (simulated fingers) and, when pneumatic, as a lane-free tool whose
  valve and sensor are I/O components of any lane. Isaac tools still need code.

`list --json` prints each curated component with its descriptor, its
errors (`{code, message, details}`), `addable` (no errors, and a robot with a
Mock profile row, a tool with a Mock or lane-free profile row, or an I/O
device) and `runtime`: for a robot or tool the model family id and its
component profile ids per lane (`lane_free` for a pneumatic tool's lane-free
profile), for an I/O device `io_device_v0`.

## Where components live

| Kind | Folder |
| --- | --- |
| Robot | `manyforge_example_models/models/<name>/` (URDF/SRDF live here; package mesh URIs resolve through the maintained model source root) |
| Tool | `manyforge_example_models/models/<name>/`, the same way |
| I/O device | `components/io_device/<vendor>.<name>/` |

The templates are in `components/templates/<kind>/`. Two folders with the same
`id` and `version` but different content are both listed in error
(`component_package_conflict`); different versions are listed side by side.

## Add a robot (Mock)

The worked example is the UF850 arm at
`manyforge_example_models/models/uf850/component.yaml`; its xArm gripper is the
separate tool at `manyforge_example_models/models/xarm_gripper/component.yaml`.
Copy the closest arm descriptor when that is easier than the template.
UFACTORY models (xArm 5/6/7, Lite 6, UF850) share one Mock controller template,
and so do UR models. A robot of another vendor family needs a new controller
template, which is code.

You need: the arm URDF (xacro expanded once), its mesh package URIs, a MoveIt
SRDF with the arm group, the vendor controller file (its joint trajectory
constraint), and the vendor TCP speed limit. Describe a gripper separately as
a tool with its own URDF and, when present, its gripper SRDF group.

1. **Create the folder.**

   ```bash
   scripts/manyforge component new robot acme.r6
   ```

   This writes `manyforge_example_models/models/r6/component.yaml` and
   `README.md`. The name after the dot (`r6`) becomes the model family
   id and the Mock profile id.

2. **Add the model files** to the folder: `<name>.urdf`, `<name>.srdf` and
   `licenses/`. Keep joint names and mimic joints as upstream. Mesh URIs must
   resolve under `model.package_name`; a split arm may keep meshes in the
   original combined model source directory. Say in `README.md` where files
   and meshes come from.

3. **Fill in `component.yaml`.** Replace every `<TODO>`; the
   [field reference](#descriptor-fields) lists each field. Keep robot roles
   arm-only. Each `tcps` entry is a TCP under its own `id`; the first is the
   default TCP for a robot or tool descriptor. A frame that `tcps` does not
   list is not a TCP, even if it is named `tcp`. When a tool is the primary
   tool on a robot's flange, its default TCP is the robot's End-effector TCP
   instead of the robot's own default TCP. A separate tool has its own
   `mount.frame`, TCPs and gripper roles. Copy
   the trajectory constraint into `vendor_joint_constraints` from the vendor
   controller file (`xarm_controller/config/<model>_controllers.yaml`); leave
   it out for `ur`. `arrival_tolerance_rad` (0.01 for Mock) becomes every arm
   joint's JTC `goal`: a motion arrives when it finishes within it.

4. **Check.**

   ```bash
   scripts/manyforge component check manyforge_example_models/models/r6
   ```

   Fix every line until the only codes left are `component_not_registered`,
   `component_generated_missing` and `component_schema_refuses`. Each line
   names the field, for example:

   ```text
   error: manyforge_example_models/models/r6/component.yaml: roles.frames.flange: link 'link_tcpx' is not in r6.urdf [component_role_unknown_link]
   ```

5. **Register.**

   ```bash
   scripts/manyforge component register manyforge_example_models/models/r6
   ```

   It writes, from the descriptor, the model family row and a minimal Mock
   component profile row `<name>_mock_commanded_v0` in
   `manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json`,
   and the Mock controller row in
   `manyforge_ros_bringup/config/mock_controller_profiles.yaml`. Run it again
   after every change to `model`, `roles`, `tcps` or `lanes`: `check` reports
   a row that no longer matches as `component_registration_stale`. It refuses while
   the descriptor or its model has errors, and writes nothing when the rows
   already match.

6. **Generate the catalog files.** The model asset comes first, because the
   component-type generator reads it.
   - Model asset: configure a `manyforge_deployment` build with tests
     (`-DBUILD_TESTING=ON`). Its configure prepares one structural folder per
     model family row, `<build>/generated/<name>_structural`. Then:

     ```bash
     <build>/manyforge_maintained_model_catalog --update \
       --description manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json \
       --workspace-root . --family <name> \
       --prepared <name>=<build>/generated/<name>_structural
     ```

     It writes `manyforge_deployment/resources/maintained/model_assets/<name>.model_asset.json`.
   - Component types: after one workbench start (which configures
     `/runner/build/deployment-authoring`):

     ```bash
     cmake --build /runner/build/deployment-authoring --target manyforge_deployment_revision
     PYTHONPATH=manyforge_behavior:. python3 manyforge_deployment/tools/generate_maintained_deployment_occurrences.py \
       --update \
       --description manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json \
       --assistant-profiles manyforge_deployment/resources/maintained/maintained_assistant_profiles.v1.json \
       --workspace-root . --revision-tool /runner/build/deployment-authoring/manyforge_deployment_revision
     ```

     It writes `<name>.component.json` and `<name>.fixed_scale.component.json`
     under `manyforge_deployment/resources/maintained/catalog/component_type/`.

7. **Accept the id in the runtime schema.** The closed runtime schema
   (`manyforge_deployment/resources/closed_schema/value/manyforge.value_schema.runtime.ros2_control_execution.v1.yaml`)
   lists the accepted `mock_profile` ids, and so does
   `manyforge_deployment/src/ros2_control_execution_runtime_unit_internal.cpp`.
   A new id must be added to both. This changes the schema's revision, which
   is recorded in several files, and every ROS component's identity, so it is
   a maintainer change, not part of a site setup. `check` reports a missing id
   as `component_schema_refuses`.

8. **Scenario.** In an arm-only Scenario, set the robot component:
   `component_profile_id` (`<name>_mock_commanded_v0`),
   `runtime_configuration.mock_profile` (`<name>`), `model_source` (the URDF
   and SRDF), `initial_joint_positions_rad` (one per arm joint), the Stop
   profiles (one entry per arm joint) and the TCP ceiling (at or below
   `limits.max_tcp_linear_speed_mps`; start at 0.15 m/s). The gripper is a
   separate tool component mounted on the robot's `flange` ([Add a
   tool](#add-a-tool)); keep or replace it as described in
   [Grippers](GRIPPERS.md) (its row's frames, touch links and `open`/`closed`
   states). Do not edit `scenario.yaml` digests; Save Scenario rewrites them.
   Once such a Scenario is under `examples/`, Composer's Add component
   (Components section) offers the robot row to other Scenarios. Then:

   ```bash
   scripts/manyforge component check manyforge_example_models/models/r6 \
     --scenario examples/<name>.scenario
   ```

9. **Rebuild and load.** The Mock controller rows are compiled into the
   workbench, so restart it:
   `./scripts/compose-dev.sh stop && ./scripts/compose-dev.sh start --project examples/<name>.scenario`.
   The start reports the Scenario `ready`. In Composer, check that the arm
   model renders, jog each joint, and run the Program.

**Done when** `component check <folder> --scenario <scenario>` prints only
`ok:` lines and step 9 passes.

## Add an I/O device

1. `scripts/manyforge component new io_device <vendor>.<name>` writes
   `components/io_device/<vendor>.<name>/component.yaml`, a working Mock example with
   one output and one input.
2. Edit `signals`: one entry per signal (see the fields below). The provider
   profiles are a closed list in code: inputs `geometric_contact`, `mock`,
   `ros2_control_gpio`, `ros_bool`, `ur_io_states`, `xarm_digital`; outputs
   `mock_output`, `ros2_control_gpio`, `ros_set_bool`, `ur_set_io`,
   `xarm_digital`. The fields beside `profile` are the profile's own, as in a
   Deployment's `io_signals`.
3. `scripts/manyforge component check components/io_device/<vendor>.<name>` until it
   prints `ok:`.

The per-instance fields of a Deployment signal (`controllable_component_id`,
`command_target_id`, `resource_role_id`, `source_id`) are not part of the
descriptor. Apply checks each provider's own fields.

The `geometric_contact` input names a motion robot and follows its current
Active TCP at each evaluation, using the contact bodies of that TCP's owner.
It is always active. Preparation wires the robot and all tools it carries,
the robot's collision model and the Scene. No Active TCP, an owner without
contact bodies, or an uncarried tool makes the input unavailable with a typed
reason, rather than false. Missing, stale, busy or inconclusive geometry also
makes it unavailable. Contact bodies static relative to the motion group give
`contact_bodies_static`. The input is read-only and sampled; manual and timed
Mock writes do not apply.

## Add a tool

A tool is its own component, mounted on a robot's `flange`: a gripper with
fingers, or a passive tool. The worked examples are the curated tools in
`manyforge_example_models/models/`: `lite_gripper` (pneumatic),
`xarm_gripper`, `robotiq_2f_140` and `robotiq_2f_85`. Copy the closest one
when that is easier than the template.

You need: the tool URDF, whose root link is the link that sits on the flange;
an SRDF with the gripper group and its `open` and `closed` group states; and
mesh `package://` URIs that share one folder before `meshes/`.

1. **Create the folder.**

   ```bash
   scripts/manyforge component new tool acme.vac_gripper
   ```

   This writes `manyforge_example_models/models/vac_gripper/component.yaml`
   and `README.md`. The name after the dot (`vac_gripper`) becomes the model
   family id, the Mock profile id and, in examples, the component id.

2. **Add the model files** to the folder: `<name>.urdf`, `<name>.srdf` and
   `licenses/`. Say in `README.md` where they come from.

3. **Fill in `component.yaml`.** Replace every `<TODO>`; the
   [field reference](#descriptor-fields) lists each field.
   - `mount.frame` is the URDF root link. `tcps[0]` is the tool's default
     TCP. Each TCP has its `id` as its only name (`cup_center` stays
     `cup_center`). The default belongs to the tool, wherever it is mounted.
   - A gripper names its SRDF group (`roles.groups.gripper`), its commanded
     finger joint (`roles.joints.fingers`) and the `gripper` section.
   - `gripper.actuation: position_commanded` is a gripper with its own
     controller; `lanes.mock.controllers` gives its gripper-only Mock row
     (`template` of the robots it mounts on, which sets the controller
     manager's rates; no `arm_controller`).
   - `gripper.actuation: pneumatic` is a valve gripper. Add
     `gripper.command_duration_s` (the valve dwell in seconds). Keep
     `lanes.mock` for a commanded Mock gripper that needs no I/O; delete it
     when the tool only runs from I/O components.
   - A passive tool keeps `model`, `mount` and `tcps` and deletes `roles`,
     `gripper` and `lanes`.

4. **Check.**

   ```bash
   scripts/manyforge component check manyforge_example_models/models/vac_gripper
   ```

   Fix every line until the only codes left are `component_not_registered`,
   `component_generated_missing` and `component_schema_refuses`.

5. **Register.**

   ```bash
   scripts/manyforge component register manyforge_example_models/models/vac_gripper
   ```

   It writes, from the descriptor:
   - the model family row: frame roles `mount` (the root) and one per TCP
     under its `id`, the gripper group, finger and collision roles. Unless
     the only TCP is named `tcp`, it records the TCP list with the first
     descriptor entry as default (`tcp_frame_role_ids`);
   - with `lanes.mock`, a minimal Mock component profile row
     `<name>_mock_commanded_v0` and the gripper-only Mock controller row;
   - with a pneumatic gripper, the lane-free component profile row
     `<name>_pneumatic_v0`: no lane selector, no streams, and one gripper row
     with `output_mode: none` (no output until a Scenario wires its I/O).

   On an existing lane-free row it replaces only the fields the descriptor
   decides (actuation, frames, states, touch links, dwell, owner); the row's
   description, auxiliary frames and timeouts stay. It writes nothing when
   the rows already match.

6. **Generate the catalog files** as in robot step 6 (`--family <name>`). A
   family without `arm_joints` gets a tool component type: root and
   interface `mount`, one `gripper` target with a frame role per TCP, the
   first the default.

7. **Accept the id in the runtime schema** as in robot step 7, only with
   `lanes.mock`. A lane-free tool selects no profile id.

8. **Scenario.** Add the tool component next to its robot in
   `deployment.yaml`:
   - `component_id` (for example `vac_gripper`), `component_profile_id`
     (`<name>_mock_commanded_v0` or `<name>_pneumatic_v0`), `model_source`
     (the tool URDF and SRDF), `target_settings: {}` and **no**
     `root_pose_in_cell`: the attachment row places the tool;
   - `runtime_configuration`: the profile's lane selector
     (`mock_profile: <name>`; none when lane-free), `component_streams`
     (`gripper_feedback` and `measured_joints` with a lane, `[]` when
     lane-free) and one `gripper_target_configurations` row owned by the tool,
     with `mount_frame` = `mount.frame`. A pneumatic row adds `pneumatic_io`
     and `output_mode: commanded` ([Grippers](GRIPPERS.md));
   - one or more `attachments` rows
     `{parent: {component_id: <robot>, interface: flange}, child: {component_id: <tool>, interface: mount}}`,
     with an optional `pose` (default identity). When two or more tool-tagged
     components share one parent interface, mark exactly one of their rows
     `primary: true`; Apply refuses with `mount_primary_tool_required` until
     exactly one is marked. Other components on the same interface, such as a
     camera without the `tool` tag, need no mark;
   - `default_tcp: {command_target_id, frame_role_id}` on a component row
     when the component has TCPs across multiple targets and no single
     type-declared default. Apply refuses with
     `component_default_tcp_required` until it is set, and with
     `component_default_tcp_unknown` when the pair is not one of the
     component's TCPs. A tool made from a descriptor has one target and never
     needs it.

   In `assignment.yaml`, a `tool` component role selects the tool component.
   Then:

   ```bash
   scripts/manyforge component check manyforge_example_models/models/vac_gripper \
     --scenario examples/<name>.scenario
   ```

   It checks each of the tool's components: one attachment row from a robot's
   free `flange` to its `mount`, no `root_pose_in_cell`, the lane selector
   and streams of its profile, its gripper rows against the tool model, and
   the `tool` role.

9. **Rebuild and load** as in robot step 9 when the tool has a Mock row.

**Done when** `component check <folder> --scenario <scenario>` prints only
`ok:` lines and the Program's gripper steps run.

## Set up a component's TCPs

The TCPs of a new robot or tool are set up in this order (the terms are in
[TCPs](TCPS.md#terms)):

1. **Import the vendor model**: the URDF and SRDF in the component folder
   (step 2 above). Vendor files never make a frame a TCP: a link named `tcp`
   stays an ordinary frame.
2. **Targets and frame roles**: name the frames the component uses
   (`roles.frames`, a tool's `mount.frame`). Every frame has one name.
3. **Mark the TCPs**: list each TCP in `tcps`. Only the listed frames are TCPs.
   Custom TCPs can be added later on the component page.
4. **Choose a default if there are several**: the first `tcps` entry is the
   component's default. A component type with TCPs across multiple targets
   and no single type-declared default needs `default_tcp` on its Deployment
   row ([Add a tool](#add-a-tool) step 8). The selected default belongs to
   the component, wherever the tool is mounted.

A robot's End-effector TCP is the default TCP of the only tool on its flange,
or of the marked primary tool when there are several. If no tool is mounted
there, it uses the robot's default TCP, or its flange if it has no other
default. If the primary tool has no TCP, the robot has no End-effector TCP;
there is no fallback to the robot's own TCP or flange. Mounts are not edited
in Composer: mark exactly one tool's attachment row `primary: true` in the
Scenario's Deployment when multiple tools share an interface, then Load the
Scenario again. A primary tool with no TCP still allows the cell to load and
Apply, with a non-blocking Deployment warning. Execute is refused while any
robot in the cell has no End-effector TCP. Declare a TCP on each such robot's
primary tool to provide one.

A Real robot component also has **Send TCP offset to controller** (selected by
default). Turn it off for instant TCP switching without sending TCP offsets;
the controller's own TCP features then refer to the flange. Explicit Program
and direct Scene attach/detach payload updates still run. See
[TCPs](TCPS.md#the-controllers-tcp-offset).

## Lifecycle time budget

Every component has a lifecycle time budget: the maximum time one of its
lifecycle steps (Prepare, Enable, Disable, Recover, and the re-enable of
Recover cell) may take, for example how long a robot may take to come up
after activation. An Apply that installs or replaces the component also waits
for it.

- **Default.** 10 s, for every component kind and lane.
- **Override.** Set `lifecycle_command_timeout_s` (seconds, above 0, at most
  3600) in the component's `configuration`, or on its Components page
  ("Lifecycle command time limit"; blank keeps the default). An I/O device
  sets it in its `runtime_configuration.io_provider_devices` row (default
  10 s).
- An Apply waits for the sum of the budgets of the components it installs or
  replaces; an Apply that installs none waits 10 s.
- The budget bounds the whole step, not each call: individual controller and
  vendor service calls must still answer within a few seconds, and one that
  does not is reported as an unresponsive component. A call's own wait (for
  example the few seconds a Real xArm takes to settle after arming) never
  runs past the step's budget.

A changed budget, like any other component setting, stays in the Draft and
takes effect at the confirmed Apply, which replaces that component.

## Descriptor fields

Common to every kind:

| Field | Required | Meaning |
| --- | --- | --- |
| `api_version` | yes | `manyforge.component_package.v1` |
| `kind` | yes | `robot`, `tool` or `io_device` |
| `id` | yes | `<vendor>.<name>`, each a lower-case portable id (`[a-z][a-z0-9_]*`). For a robot, `<name>` is the model family id and the Mock profile id |
| `version` | yes | `MAJOR.MINOR.PATCH`. Bump the minor version for compatible data changes, the major version when roles or joints change |
| `title` | yes | Human-readable name |
| `license` | yes | License of the model files (SPDX id) |
| `provenance` | no | Text fields (`source`, `revision`, `notes`, ...); recorded, never checked |

Robot:

| Field | Required | Meaning |
| --- | --- | --- |
| `model.urdf`, `model.srdf` | yes | Files in the folder |
| `model.package_name` | yes | The package name in `package://` mesh URIs; mesh files may resolve under a maintained source root outside this descriptor folder |
| `roles.root_frame` | yes | The URDF root link |
| `roles.frames` | yes | Frame roles to URDF links; robot arms require `base` and `flange` |
| `roles.groups` | yes | Group roles to SRDF groups; robot arms require `arm` |
| `roles.joints` | yes | `arm_joints`: the commanded arm joints, base to tip |
| `roles.collision_bodies` | no | Collision roles to URDF links (`arm_collision`) |
| `roles.instance_cardinality` | no | `multiple_per_cell` (default) or `one_per_cell` |
| `tcps` | yes | `[{id, frame}]`, one or more TCPs, each a frame role named by its `id`; the first is the default TCP. An `id` that is also a frame role (`roles.frames`, or `root`) must be on that role's link, else it is refused (`component_manifest_invalid`). Only the listed frames are TCPs: a frame merely named `tcp` is an ordinary frame. `tcp` is an ordinary name, allowed for any TCP. The descriptor checker verifies each frame exists. Apply reads the maintained rows, not this descriptor |
| `tcp_speed_check` | no | `active_tcp_and_flange` (default) or `active_tcp`: which frames the TCP speed limit applies to. Recorded and listed; Apply does not read it yet |
| `limits.max_tcp_linear_speed_mps` | yes | The vendor TCP speed limit. Take it from the component's datasheet: checking the value is your responsibility, and ManyForge enforces whatever value is entered. `check` refuses a default or a Scenario TCP ceiling above it (`component_limit_exceeded`); Apply does not read it yet |
| `lanes.mock.controllers` | yes | The robot arm's Mock row: `template` (`ufactory` or `ur`), `hardware_component_name`, `arm_controller`, `vendor_joint_constraints` (`ufactory` only: `{trajectory}`), timeouts and `arrival_tolerance_rad` |
| `defaults` | no | Values for a new Deployment component: `initial_joint_positions_rad` (one per arm joint), `maximum_tcp_linear_speed_mps` (at or below the limit) |

Tool:

| Field | Required | Meaning |
| --- | --- | --- |
| `model.urdf`, `model.srdf` | yes | Files in the folder |
| `model.package_name` | no | The package of the mesh URIs; read from the URIs when absent |
| `mount.frame` | yes | The URDF root link, the one on the robot flange (frame role `mount`) |
| `tcps` | yes | `[{id, frame}]`, each a frame role named by its `id`; the first is the default TCP. An `id` of `mount` (or a `roles.frames` role) must be on that role's link, else it is refused. `tcp` is an ordinary name, allowed for any TCP |
| `roles.groups.gripper` | with a gripper | The SRDF group with the `open`/`closed` states |
| `roles.joints.fingers` | with a gripper | The commanded finger joint, not a mimic follower |
| `roles.collision_bodies` | no | `gripper_collision` and other collision roles to URDF links |
| `gripper.actuation` | with a gripper | `position_commanded` or `pneumatic` |
| `gripper.states` | with a gripper | `{open, closed}`: group_states of the gripper group |
| `gripper.attach_frame`, `gripper.touch_links` | with a gripper | The link a grasped object attaches to; the links that may touch it |
| `gripper.command_duration_s` | pneumatic | The valve dwell in seconds |
| `lanes.mock.controllers` | no | The gripper-only Mock row: `template`, `hardware_component_name`, `gripper_controller` and timeouts; no `arm_controller` |

I/O device: `signals` (required), each with `signal_name` (portable id, unique),
`direction` (`input` or `output`), `value_type` (`bool`), `provider`
(`profile` plus its fields) and, for an input, `freshness`,
`transition_capability` and `update_mode`.

## Check codes

| Code | Meaning |
| --- | --- |
| `component_manifest_invalid` | A descriptor field is missing, unknown or malformed, or the YAML is invalid (the line names the field) |
| `component_model_unreadable` | The URDF or SRDF cannot be read |
| `component_role_unknown_link` | A frame, collision, TCP, mount, attach or touch link is not in the URDF |
| `component_group_unknown` | A group role names no SRDF group |
| `component_joint_invalid` | A joint role names a missing, fixed or mimic joint |
| `component_state_unknown` | The gripper group lacks an `open`/`closed` state, or `gripper.states` names a missing one |
| `component_mesh_unresolved` | A mesh URI does not resolve in the package or maintained model source root |
| `component_lane_invalid` | The Mock controller row is invalid, a robot row has no arm controller or a tool row has one |
| `component_joint_count_mismatch` | Initial joint positions do not match the arm joints |
| `component_limit_exceeded` | A default or Scenario TCP ceiling is above `limits.max_tcp_linear_speed_mps` |
| `component_not_registered` | A robot's or tool's rows are missing; run `register` |
| `component_registration_stale` | A robot's or tool's rows differ from the descriptor; run `register` |
| `component_generated_missing` | A model asset or component type file is missing (step 6) |
| `component_schema_refuses` | The runtime schema does not accept the Mock profile id yet (step 7) |
| `component_scenario_mismatch` | A Scenario's component of this robot or tool does not match it (`--scenario`) |
| `component_attachment_invalid` | A Scenario's tool is not mounted from a free robot `flange` to its `mount`, carries `root_pose_in_cell`, or the `tool` role selects no mounted tool (`--scenario`) |
| `component_package_conflict` | Two folders have the same id and version with different content |
| `component_folder_exists` | `new` found a `component.yaml` already |

## Messages at Apply

| Message | Cause |
| --- | --- |
| `component X selects unknown profile Y` | The profile row is missing or misspelled (step 5). |
| `selects unknown model family` | The family row is missing (step 5). |
| `mock_profile does not match its ROS component profile` | The Deployment and the profile row name different `mock_profile` ids. |
| `prepared model differs from its repository-owned profile` | A mesh URI or package root does not use the family's package prefix. |
| `unsupported mock controller profile` | The Mock controller row is missing (steps 5 and 9). |
| `component_streams must declare exactly measured_joints [and gripper_feedback]` | Streams are derived per owning component: a robot arm declares `measured_joints`; a component owning a gripper feedback target may also require `gripper_feedback`. |
| `tool_lane_required` | A lane-free tool profile without an authored pneumatic gripper row. |
| `tool_pneumatic_io_required` | A lane-free pneumatic gripper row with `output_mode: commanded` has no `pneumatic_io`. |
| `real_adapter_family_unknown` | A Real robot row has no `adapter_family` (e.g. `xarm`), or one with no lifecycle provider `manyforge.provider.ros2_control.<adapter_family>` in the catalog. |
| `unexpected field root_pose_in_cell` | A mounted tool carries a root pose; its attachment row places it. |
