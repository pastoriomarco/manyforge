# Adding a component

A component is a folder with one `component.yaml`, its **descriptor**: a robot,
a tool, an I/O device, a machine, a sensor, the AI assistant or a simulator
described as data.
One command family creates, checks and lists component folders:

| Command | What it does |
| --- | --- |
| `scripts/manyforge component new <robot\|tool\|io_device\|machine> <vendor>.<name> [--into <folder>]` | Creates the folder from a template |
| `scripts/manyforge component check <folder> [--scenario <scenario>]` | Checks the descriptor, its model files, its lane rows and the rows Apply reads |
| `scripts/manyforge component register <folder>` | Writes a robot's or tool's model family row; lists a machine, a model-form I/O device, a sensor or an assistant in `component_models` |
| `scripts/manyforge component list [--json]` | Lists the curated components, each checked |

Run them in the workbench container from the repository root; ROS is never
installed on the host. Each problem prints as one line,
`error: <where>: <what> [<code>]`, and the exit status is 1. Nothing is
written except by `new` and `register`.

The kinds:

- **Robot** (an arm): its model, roles, TCPs, vendor TCP speed limit and one
  row per lane it runs on (`lanes.mock`, `lanes.isaac`, `lanes.real`).
- **Tool** (a gripper or a passive tool): its own component, mounted on a
  robot's `flange`. A gripper runs on a lane's controller (`lanes.<lane>`), or,
  when pneumatic, without a lane, driven by I/O signals. A passive tool (no
  `gripper` section) has nothing to drive and never names a lane. A project
  makes its own tool from parameters with the shipped Tool model
  ([A Tool from its parameters](#a-tool-from-its-parameters)), or brings its
  own URDF with the generic passive tool
  ([A project's own passive tool](#a-projects-own-passive-tool)).
- **I/O device** (`io_device`): signals. In provider form each signal names
  its provider (a separately connected device, Deployment profile
  `io_device_v0`); in model form it is an I/O bank (signals, each with an
  optional `index`, plus `lanes.real: {adapter, input_maximum_age_ms, ...}`):
  a machine's own bank, or a device of its own such as a PLC's bank
  ([I/O banks](IO_BANKS.md)).
- **Machine** (`machine`): one physical machine as an **owner** (connection,
  driver process, controller state, power) with **members** (its arm, its I/O
  banks). See [Add a machine](#add-a-machine).
- **Sensor** (`sensor`, e.g. `components/sensor/manyforge.pointcloud_source`):
  a sensor type and one adapter row (`adapter_row`); no lane.
- **AI assistant** (`assistant`, `components/assistant/manyforge.assistant`):
  the chat behind the Composer AI tab; one adapter row (`adapter_row`); no
  lane; at most one per cell. Its Deployment row names an `ai_model`
  component (`model_component_id`) and an assistant profile with a chat mode
  (`assistant_profile_id`); see the
  [assistant cookbook](../operations/ASSISTANT_E2E_COOKBOOK.md).
- **Simulator** (`simulator`, e.g. `manyforge_example_models/models/isaac_sim`):
  lane rows only. Enable plays the simulation, Disable pauses it.

Apply reads the model family row (for a robot or tool) and the descriptor
itself (its lane rows); a Deployment component selects a model by its `id`
(`component_profile_id`) and a lane by `lane`.

`list --json` prints each curated component with its descriptor, its errors
(`{code, message, details}`), `addable` and `runtime`: `componentProfileId`,
and for a robot or tool `modelFamilyId` and `lanes` (`lane_free` for a
pneumatic or passive tool), for a machine `lanes` (its arm's) and `members`. Addable are:
a robot with a Mock lane that is no machine's member, a tool with a Mock lane
or lane-free, a machine whose arm has a Mock lane, a provider-form I/O device,
a sensor, an assistant and a simulator. A machine's member models are added only with their
machine.

## Where components live

| Kind | Folder |
| --- | --- |
| Robot, tool, simulator | `manyforge_example_models/models/<name>/` (URDF/SRDF live here; package mesh URIs resolve through the maintained model source root) |
| I/O device, machine, sensor, assistant | `components/<kind>/<vendor>.<name>/` |

The templates are in `components/templates/<kind>/` (`robot`, `tool`,
`io_device`, `machine`). Two folders with the same `id` and `version` but
different content are both listed in error (`component_package_conflict`);
different versions are listed side by side.

## Add a robot

The worked examples are the arms in `manyforge_example_models/models/`:
`lite6` and `uf850` (xArm, on Mock, Isaac and Real through their machines),
`ur10e` (Mock and Isaac), `ur5e`. Copy the closest arm descriptor when that is
easier than the template. A gripper is a separate tool ([Add a
tool](#add-a-tool)).

You need: the arm URDF (xacro expanded once), its mesh package URIs, a MoveIt
SRDF with the arm group, the vendor's controller configuration and the vendor
TCP speed limit.

1. **Create the folder.**

   ```bash
   scripts/manyforge component new robot acme.r6
   ```

   This writes `manyforge_example_models/models/r6/component.yaml`,
   `mock_controllers.yaml` and `README.md`. The name after the dot (`r6`)
   becomes the model family id.

2. **Add the model files** to the folder: `<name>.urdf`, `<name>.srdf` and
   `licenses/`. Keep joint names and mimic joints as upstream. Mesh URIs must
   resolve under `model.package_name`. Say in `README.md` where files and
   meshes come from.

3. **Fill in `component.yaml`.** Replace every `<TODO>`; the
   [field reference](#descriptor-fields) lists each field. Keep robot roles
   arm-only. Each `tcps` entry is a TCP under its own `id`; the first is the
   robot's default TCP ([Set up a component's TCPs](#set-up-a-components-tcps)).
   Then the lane rows ([Lanes and adapters](#lanes-and-adapters)):
   - `lanes.mock`: `adapter: mock`, the arm's JTC as `controller`,
     `arrival_tolerance_rad` (0.01 for Mock; it becomes every arm joint's JTC
     `goal`: a motion arrives when it finishes within it), `runtime_scale`
     (`supported` when the arm's JTC claims `position` only, else
     `unsupported`) and the controllers.
     A model of a vendor with an adapter (xArm) names `vendor: xarm` and
     `vendor_model` and runs the vendor's own controllers; any other model
     fills in `mock_controllers.yaml` (`controllers_file`) from the vendor's
     controller file: its rates and per-joint trajectory constraint.
   - `lanes.isaac` and `lanes.real`, when the robot runs there: the keys of
     that lane's adapter, listed in its README
     (`manyforge_isaac_adapter/lane/README.md`,
     `manyforge_xarm_control/vendor/README.md`).

4. **Check.**

   ```bash
   scripts/manyforge component check manyforge_example_models/models/r6
   ```

   Fix every line until the only codes left are `component_not_registered`
   and `component_generated_missing`. Each line names the field, for example:

   ```text
   error: manyforge_example_models/models/r6/component.yaml: roles.frames.flange: link 'link_tcpx' is not in r6.urdf [component_role_unknown_link]
   ```

5. **Register.**

   ```bash
   scripts/manyforge component register manyforge_example_models/models/r6
   ```

   It writes the model family row `<name>` in
   `manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json`:
   frame, group, joint and collision roles, the TCP list and
   `component_descriptor` (the path of `component.yaml`). Lane rows stay in
   the descriptor, which the runtime reads. Run it again after every change
   to `model`, `roles` or `tcps`: `check` reports a row that no longer matches
   as `component_registration_stale`. It refuses while the descriptor or its
   model has errors, and writes nothing when the row already matches.

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
   - Component types:

     ```bash
     PYTHONPATH=manyforge_behavior:. python3 manyforge_deployment/tools/generate_maintained_deployment_occurrences.py \
       --update \
       --description manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json \
       --assistant-profiles manyforge_deployment/resources/maintained/maintained_assistant_profiles.v2.json \
       --workspace-root .
     ```

     It writes `<name>.component.json`
     under `manyforge_deployment/resources/maintained/catalog/component_type/`.

7. **Scenario.** In a Scenario's `deployment.yaml`, add the robot component:
   `component_profile_id` (the descriptor's `id`, `acme.r6`), `lane`
   (`mock`), `model_source` (the URDF, SRDF and package root),
   `root_pose_in_cell`, `target_settings.arm` (the TCP ceiling
   `maximum_tcp_linear_speed_mps`, at or below
   `limits.max_tcp_linear_speed_mps`, and `controlled_stop.per_joint_limits`,
   one entry per arm joint in order), `configuration.default_fault_stop_profile`
   (`per_joint_max_deceleration_rad_s2` for every arm joint) and
   `configuration.runtime_configuration` with `initial_joint_positions_rad`
   (one per arm joint) and `component_streams` (`measured_joints`). A robot that is a machine's arm is added through its
   machine instead ([Add a machine](#add-a-machine)). Mount a gripper as a
   separate tool on the robot's `flange` ([Add a tool](#add-a-tool)). Do not
   edit `scenario.yaml` digests; Save Scenario rewrites them. Once such a
   Scenario is under `examples/`, Composer's Add component (Components
   section) offers the robot row to other Scenarios. Then:

   ```bash
   scripts/manyforge component check manyforge_example_models/models/r6 \
     --scenario examples/<name>.scenario
   ```

8. **Load.** `./scripts/compose-dev.sh start --project examples/<name>.scenario`,
   or Load the Scenario in Composer. After Load every component is
   `prepared`; Enable it (or Activate) before the Program runs. In Composer,
   check that the arm model renders, jog each joint, and run the Program.

**Done when** `component check <folder> --scenario <scenario>` prints only
`ok:` lines and step 8 passes.

## Add a machine

A machine is one physical robot with its own controller: the **owner** holds
the connection, the driver process, the controller's error state and power;
its **members** are its parts, an arm and I/O banks. The worked examples are
`components/machine/ufactory.lite6_machine` and `ufactory.uf850_machine`, with
their arms (`manyforge_example_models/models/lite6`, `uf850`) and I/O banks
(`components/io_device/ufactory.lite6_controller_io`, `ufactory.lite6_tool_io`,
and the UF850's).

1. Describe each member model first: the arm as a robot ([Add a
   robot](#add-a-robot)) and each I/O bank as a model-form `io_device`
   ([Add an I/O device](#add-an-io-device)).
2. `scripts/manyforge component new machine acme.r6_machine` writes
   `components/machine/acme.r6_machine/component.yaml`. Fill in:
   - `default_owner_id`: the owner's component id when the machine is added;
     its members get `<owner_id>_<role>` (both editable in the Deployment);
   - `members`: one `{role, model}` per slot, each role once; each model is a
     curated robot or a model-form I/O device (`arm`, `controller_io`,
     `tool_io` on the xArm machines);
   - `lanes`: one row per lane with a vendor adapter (`real: {adapter: xarm}`).
     On a lane without a row (Mock, Isaac) the owner gets the host's passive
     adapter, the arm runs its own lane row and the I/O banks run on the host's
     Mock I/O.
3. `scripts/manyforge component check components/machine/acme.r6_machine`,
   then `scripts/manyforge component register components/machine/acme.r6_machine`
   (lists it in the description's `component_models`; do the same for each
   model-form I/O bank).

Composer's Add component offers the machine (kind machine): it adds the owner
row and one row per member. In the Deployment:

- the owner row selects the machine model and names the `lane`; on the real
  lane its `configuration.device_address_ref` names the site settings key of
  the controller's address;
- each member row has `owner` (the owner's component id) and `role`, and runs
  on its owner's lane (it names none);
- a member with `active: false` stays in the file and is left out of the cell.
  Every member may be deactivated, the arm included; members are never
  removed. With the arm deactivated, the owner still runs the machine's process
  (on xArm: the I/O stream flows, the motors are never enabled).

Apply refuses a member without its owner, an unknown or repeated role
(`deployment_member_role_unknown`, `deployment_member_role_repeated`), a
missing member slot (`deployment_member_missing`), and a member model placed
on its own (`deployment_machine_part_without_machine`). Program and Deployment
references to a member's signals are `<member_id>/<signal_name>`.

The owner offers Prepare, Recover and its capabilities (Power off, Reboot);
it has no Enable. Its members offer Prepare, Enable, Disable and Recover,
refused (`owner_not_prepared`) while the owner isn't prepared. While the owner
is offline, not prepared or faulted, its members show `offline` with the
reason `owner_unavailable`. An error of the machine's controller is the
owner's fault; it names the members it concerns. An e-stop reported by the
controller is the owner's stop, not a fault.

## Add an I/O device

1. `scripts/manyforge component new io_device <vendor>.<name>` writes
   `components/io_device/<vendor>.<name>/component.yaml`, a working Mock
   example with one output and one input (provider form).
2. Edit `signals`: one entry per signal (see the fields below).
   - **Provider form** (a separately connected device): each signal names its
     `provider`. The provider profiles are a closed list in code: inputs
     `geometric_contact`, `mock`, `ros2_control_gpio`, `ros_bool`; outputs
     `mock_output`, `ros2_control_gpio`, `ros_set_bool`. The fields beside
     `profile` are the profile's own, as in a Deployment's `io_signals`.
   - **Model form** (an I/O bank): signals without a provider, each with an
     optional `index` (its bit in the device's bank, unique per direction),
     and `lanes: {real: {adapter, input_maximum_age_ms, ...}}`
     (`input_maximum_age_ms`: the inputs' freshness budget, the oldest sample
     still fresh). The row's other keys belong to the adapter family, which
     checks them when the Deployment is applied: the xArm family's `bank`
     (`controller` or `tool`), the `ros_io_bank` family's `rate_hz` and its
     required `index` per signal ([I/O banks](IO_BANKS.md)). A machine's bank
     is read and written by its vendor family through its owner's connection;
     a bank of its own (no owner) by its own family. On Mock and Isaac the
     host's Mock I/O provides it (outputs read back). Register it
     (`component register <folder>`).
3. `scripts/manyforge component check components/io_device/<vendor>.<name>` until it
   prints `ok:`.

The per-instance fields of a Deployment signal (`controllable_component_id`,
`command_target_id`, `resource_role_id`, `source_id`) are not part of the
descriptor. Apply checks each provider's own fields.

A `mock` input can be set by hand from the viewer's Tool & I/O panel while
the cell runs; see [Setting a Mock input](INTERRUPTS.md#signals).

**Health of a provider-form device.** Its ROS inputs (`ros_bool`,
`ros2_control_gpio`) are watched for their senders: once an input topic has
had a publisher, losing every publisher of all its ROS input topics is the
device's fault (`fault_communication`) while it's enabled, held until you
Recover it once a sender is back; while it isn't enabled the device shows
`offline` with the reason `io_device_senders_gone`. Silence alone isn't a
fault, so change-only inputs are fine. For a periodic device you may add a
silence budget: a Deployment `policies.component_streams` row
`{controllable_component_id: <device>, stream_id: inputs, budget_ns}` (the
**Inputs** stream under Component streams). Then no message on its ROS inputs
for longer than the budget is `fault_stream_stale`, which clears when messages
arrive again. Without a row there's no budget. Mock and `geometric_contact`
inputs have no sender. A machine's I/O members are covered by their own I/O
stream.

### Geometric-contact inputs

This input is true while the selected robot's contact bodies touch the selected Scene resource, checked against live geometry. If ManyForge can't make the check, the input is unavailable, never false. Contact bodies are the collision shapes on the robot, or on the mounted tool, that owns its Active TCP (the tool center point currently selected for that robot); a Scene resource is a named object in the cell's Scene.

During setup, ManyForge prepares the selected robot, its mounted tools, the robot collision model and the cell
Scene for this check. While the I/O source is enabled, it samples independently of whether a Program reads the
signal. At every sample, ManyForge reads the current Active TCP and collision geometry. A Program may read this
signal, but it does not store those live values or the contact result.

A live unavailable sample uses `io_source_unavailable`; its `reason` says why:

- `active_tcp_unavailable`: the robot has no current Active TCP.
- `active_tcp_unknown`: the current Active TCP is not in this robot's TCP list.
- `tcp_tool_not_mounted`: the TCP's tool is not mounted on this robot.
- `active_tcp_contact_bodies_unavailable`: no contact bodies are declared for the Active TCP's component.
- `runtime_motion_endpoint_body_unresolved`, `runtime_motion_endpoint_invalid`,
  `runtime_motion_endpoint_body_invalid`, `runtime_collision_body_alias_table_invalid`, or
  `runtime_collision_body_alias_unresolved`: the runtime cannot identify or validate a contact body
  in the robot's model.
- `observed_state_missing_or_stale`: current robot or tool positions needed for the check are
  missing or older than their freshness limit.
- `collision_geometry_missing_or_stale`: current collision geometry for the robot or Scene resource
  is missing or stale.
- `collision_geometry_unresolved`: ManyForge cannot match or prepare the contact bodies and Scene
  resource geometry for a complete check.
- `collision_query_busy`: the previous contact check is still running.
- `collision_query_inconclusive`: the geometry query cannot confirm contact or no contact.
- `contact_bodies_static`: at least one contact body is static relative to the robot's motion
  group, so it does not move with the robot.
- `mesh_not_closed`: a trip depth between two meshes, and one of them is not a closed surface.
- `mesh_depth_vertex_cap_reached`: a trip depth between two meshes, with more than 256 vertices of
  one mesh inside the other.

Before the first sample, the signal has no value. A valid sample becomes stale when it is older than the configured **Maximum sample age**. Setting that age below normal sample-production time can make the sample stale. This input is read-only; manual
and timed Mock writes do not apply.

In place of `target_scene_resource_id`, the provider may name two selections
`a` and `b` with the selection kinds of a collision rule
([Selections](COLLISION_RULES.md#selections)): `component`, `link`,
`attached_object`, `scene_objects` and `scene_object` (by
`scene_resource_id`). The input is then true while a pair of one part of `a`
and one of `b` touches, and with `trip_depth_m` only while a pair overlaps
deeper than that. With `release_depth_m` as well (above 0 and below
`trip_depth_m`) the input has hysteresis like a real sensor: it turns true
when a pair overlaps deeper than `trip_depth_m` and turns false again only
when no pair overlaps deeper than `release_depth_m`. Without it the input
turns false as soon as the overlap is no longer deeper than `trip_depth_m`. A
`release_depth_m` that is not below `trip_depth_m`, or one without
`trip_depth_m`, is refused.
`robot_component_id` still names the robot whose positions are sampled. An `attached_object` selection while nothing is carried holds no
pair: the input is false. A selection that names nothing in the cell is
`collision_geometry_unresolved`, and a touching pair whose depth the query
does not give is `collision_query_inconclusive`.

### Mock inputs from geometry

On Mock and Isaac a machine's I/O inputs are the host's Mock inputs. An I/O
member row may instead let an input follow the cell's geometry with
`mock_inputs`, for example a holder whose sensor trips when the tube the robot
carries is pressed into it:

```yaml
- component_id: uf850_tool_io
  component_profile_id: ufactory.uf850_tool_io
  owner: uf850
  role: tool_io
  mock_inputs:
    - signal_name: TI0
      geometric_contact:
        robot_component_id: uf850_robot
        a: [{kind: attached_object, component_id: tube_gripper}]
        b: [{kind: scene_object, scene_resource_id: endplate}]
        trip_depth_m: 0.004           # true once pressed in deeper than 4 mm
        release_depth_m: 0.003        # optional: false again only below 3 mm
        maximum_sample_age_ms: 1000   # optional, default 1000
```

On Mock and Isaac the input is then a geometric-contact input with these
fields (see above); on Real the row is not used and the physical input is
read. The Program reads the same signal on every lane. A row naming a signal
that is not an input of the member's model, a signal listed twice or a
missing field fails Apply with `io_mock_input_invalid`.

A holder modelled as a generated tool's spring slider
([Parameters](#parameters)) names the slider in place of
`geometric_contact`:

```yaml
  mock_inputs:
    - signal_name: TI0
      spring_slider: {component_id: tube_holder, element_id: holder_rod}
      b: [{kind: scene_object, scene_resource_id: endplate}]
      maximum_sample_age_ms: 1000   # optional, default 1000
```

- `a` is the slider's body and every object the slider carries.
- The robot is the arm the tool is mounted on.
- `trip_depth_m` and `release_depth_m` are the slider's (without its
  `release_depth_m` the input has no hysteresis).
- A row has exactly one of `geometric_contact` and `spring_slider`; `b` is
  required.
- A component that is not a generated tool, an element that is not a
  spring slider or a tool not mounted on a robot fails Apply with
  `io_mock_input_invalid`.

A trip depth between two meshes needs both to be closed surfaces, and its
precision follows their triangulation
([Depth between two meshes](COLLISION_RULES.md#depth-between-two-meshes)).

## Add a tool

A tool is its own component, mounted on a robot's `flange`. The worked
examples are the curated tools in `manyforge_example_models/models/`:
`lite_gripper` (pneumatic), `xarm_gripper`, `robotiq_2f_140` and
`robotiq_2f_85`. Copy the closest one when that is easier than the template.

You need: the tool URDF, whose root link is the link that sits on the flange;
an SRDF with the gripper group and its `open` and `closed` group states; and
mesh `package://` URIs that share one folder before `meshes/`.

1. **Create the folder.**

   ```bash
   scripts/manyforge component new tool acme.vac_gripper
   ```

   This writes `manyforge_example_models/models/vac_gripper/component.yaml`,
   `mock_controllers.yaml` and `README.md`. The name after the dot
   (`vac_gripper`) becomes the model family id and, in examples, the
   component id.

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
     controller. `lanes.mock` gives its Mock row: `adapter: mock`, its
     parallel gripper action controller as `controller`, `controllers_file`
     (`mock_controllers.yaml`) and
     `gripper_stop_finish_tolerance` ([Grippers](GRIPPERS.md#stop-near-the-target)).
     A tool row has no `arrival_tolerance_rad`. An Isaac row
     (`lanes.isaac`) follows `manyforge_isaac_adapter/lane/README.md`.
   - `gripper.actuation: pneumatic` is a valve gripper. Add
     `gripper.command_duration_s` (the valve dwell in seconds). It runs
     without a lane, from I/O signals; keep `lanes.mock` only for a commanded
     Mock gripper that needs no I/O.

4. **Check.**

   ```bash
   scripts/manyforge component check manyforge_example_models/models/vac_gripper
   ```

   Fix every line until the only codes left are `component_not_registered`
   and `component_generated_missing`.

5. **Register.**

   ```bash
   scripts/manyforge component register manyforge_example_models/models/vac_gripper
   ```

   It writes the model family row: frame roles `mount` (the root) and one per
   TCP under its `id`, the gripper group, finger and collision roles, the TCP
   list with the first descriptor entry as default, and
   `component_descriptor`. It writes nothing when the row already matches.

6. **Generate the catalog files** as in robot step 6 (`--family <name>`). A
   family without `arm_joints` gets a tool component type: root and
   interface `mount`, one `gripper` target with a frame role per TCP, the
   first the default. A passive tool's (no `gripper` group) has one
   frame-only `tool` target instead: its TCPs and no capability. A collision
   body listed in `roles.optional_collision_bodies` (the passive tool's
   `tip`) is optional: a model without its links leaves it unbound, and it
   then contributes nothing; a body naming a link without `<collision>`
   geometry still fails.

7. **Scenario.** Add the tool component next to its robot in
   `deployment.yaml`:
   - `component_id` (for example `vac_gripper`), `component_profile_id` (the
     descriptor's `id`), `lane` (one of its lanes; none for a pneumatic tool
     driven by I/O), `model_source` (the tool URDF and SRDF),
     `target_settings: {}` and **no** `root_pose_in_cell`: the attachment row
     places the tool;
   - `runtime_configuration`: `component_streams` (`gripper_feedback` and
     `measured_joints` with a lane, `[]` without) and one
     `gripper_target_configurations` row owned by the tool, with
     `mount_frame` = `mount.frame`. A pneumatic row adds `pneumatic_io` and
     `output_mode: commanded` ([Grippers](GRIPPERS.md));
   - one `attachments` row
     `{parent: {component_id: <robot>, interface: flange}, child: {component_id: <tool>, interface: mount}}`,
     with an optional `pose` (default identity). When two or more tool-tagged
     components share one parent interface, mark exactly one of their rows
     `primary: true`; Apply refuses with `mount_primary_tool_required` until
     exactly one is marked. Other components on the same interface, such as a
     camera without the `tool` tag, need no mark;
   - optional `collision_exclusions: [{child_link, parent_link}]` on the
     attachment row: tool and arm link pairs that never touch, so planning
     skips them. They depend on the arm–tool combination, so they live on the
     mount, not in the tool package; Apply refuses a link the models lack
     (`component_attachments`). They belong to the cell context: changing them
     restarts nothing ([Grippers](GRIPPERS.md));
   - `default_tcp: {command_target_id, frame_role_id}` on a component row
     when the component has TCPs across multiple targets and no single
     type-declared default. Apply refuses with
     `component_default_tcp_required` until it is set, and with
     `component_default_tcp_unknown` when the pair is not one of the
     component's TCPs. A tool made from a descriptor has one target and never
     needs it.

   Program nodes select the mounted tool directly by its component and command
   target IDs. Then:

   ```bash
   scripts/manyforge component check manyforge_example_models/models/vac_gripper \
     --scenario examples/<name>.scenario
   ```

   It checks each of the tool's components: one attachment row from a robot's
   free `flange` to its `mount`, no `root_pose_in_cell`, its lane, its
   streams, its gripper rows against the tool model, and the `tool` role.

8. **Load** as in robot step 8.

**Done when** `component check <folder> --scenario <scenario>` prints only
`ok:` lines and the Program's gripper steps run.

### Unmounted tools and Replace tool

A tool can be added before it has a robot: in **Add component → Tool**,
choose **Not mounted yet** under **Mount on** (it is also what is offered
when no robot has a free flange). The Draft then holds the tool row without
an attachment row and without `root_pose_in_cell`; only a tool may stay
unmounted, any other model component still needs one of the two. Its page
and its tool editor work as for a mounted tool. **Mounted on** on its page
mounts it later (one Draft edit adds its attachment row) and, set to
**Not mounted**, unmounts a mounted tool again.

Preview and Apply refuse while any tool is unmounted
(`deployment_tool_unmounted`, naming the tool), so the cell never holds a
floating tool: mount it or remove it first.

**Replace with…** on a mounted tool's page swaps it for another tool of the
Draft, typically one added unmounted. One Draft edit:

- the new tool takes over the old tool's attachment row: parent, interfaces,
  `pose`, `primary` and `collision_exclusions` (its own mount, if it had one,
  is dropped);
- every reference to the old tool's component id in the Deployment Draft
  (collision rule selections, I/O and wiring rows of other components,
  attachments where it is the parent, policies) and in the Program Draft
  (node targets, TCP frames, collision bodies, collision rule selections,
  stored-pose target frames, Active TCP keys, frame visibility) names the new
  tool;
- the old tool leaves the Draft with its own settings (its custom TCPs, its
  gripper wiring).

Afterwards the Components page lists what the rewritten references name that
the new tool does not declare: command targets, frames (TCPs) and collision
bodies, from its parameters or its model. Change those references, or the
new tool, before Apply; Program reference validation reports the ones left.
Collision exclusions name the old tool's links: adjust them when the new
tool's links differ (Apply refuses unknown links).

### A Tool from its parameters

A tool without a controller of its own (a probe, a tool body, a part holder,
a pneumatic gripper with one or more jaw pairs, a spring-loaded probe) needs
no component folder: the Tool model, `manyforge.tool`
(`manyforge_example_models/models/tool/`), makes it from its parameters.

- A **Tool** is one component, mounted as a whole on a robot's `flange`.
- Its **tool ends** are where it meets the work: a jaw pair, a spring slider
  or a probe tip. Each has its TCPs and its grip or contact bodies; a jaw
  pair is actuated and is a gripper command target under its id.
- Its **mount** is fixed.

Its components name no `lane` and run on Mock and Real with any robot lane,
on the host's generic adapter; they have no streams. Jaw positions are the
commanded ones. Isaac is not supported for a Tool with jaw pairs
(`pneumatic_io_unsupported_on_isaac`).

1. **Add it.** Add component, kind Tool, group **ManyForge**, **Tool from parameters**, and
   pick the robot to mount it on. The new row holds the starter parameters
   (`new_tool.tool.yaml`: a cylinder body and its default TCP) inline in
   `model_source.parameters`, and its `attachments` row.
2. **Edit it** on its component page. The Tool card shows a 3D preview of the
   generated tool (mount frame at the origin, jaws open, TCPs as small axes;
   `.stl` meshes drawn like the other parts; a mesh it cannot draw, such as
   a `.dae` file, is a small red box named below the preview), and its
   sections edit the parameters:
   **Body** (its parts), **Chains**, **TCPs**, **Wiring** and **Mass**. Each
   change is a Draft edit; Apply loads it. When the parameters do not fit,
   the refusal shows on its field. A part with **Mesh file** geometry has
   **Browse approved meshes**: it lists the `.stl` and `.dae` files in the
   allowed folders and writes the picked file's path relative to the
   Scenario folder.
3. **Wire each jaw pair** in **Wiring**: one valve output (single-acting) or
   two (open and close), with their Open and Close values, picked from the
   cell's declared output signals; its opened and closed sensors (and the
   other gripper inputs) are optional, picked from the declared inputs.
4. **Export** (Tool card) writes the generated `tool.urdf` and `tool.srdf`
   into `<project folder>/<component id>/`. The exported URDF names each mesh
   where it is, by its path relative to that folder; with **Copy meshes into
   the export folder** it writes a copy of each mesh into its `meshes/`
   folder (`<digest>-<file name>`) and names the copies instead.

The row, as saved (the Deployment holds the parameters and, per jaw pair,
its wiring only; states, motion time, frames and the grip body come from the
parameters):

```yaml
- component_id: gripper
  component_profile_id: manyforge.tool
  frame_prefix: ''
  model_source:
    parameters:                     # inline; or {project_path: tool/gripper.tool.yaml}
      api_version: manyforge.parametric_tool.v1
      kind: tool
      body: [...]
      chains: [...]
      tcps: [...]
  target_settings: {}
  configuration:
    runtime_kind: ros2_control
    runtime_configuration:
      component_streams: []
      gripper_target_configurations:  # one wiring row per jaw pair
        - controllable_component_id: gripper
          command_target_id: outer      # the jaw pair's id
          pneumatic_io:
            inputs: {}
            outputs:
              - signal: {controllable_component_id: gripper_io, signal_name: outer_valve}
                close_value: true
                open_value: false
```

A wiring row may add `grasp_confirmation`
([Grasp confirmation](GRIPPERS.md#grasp-confirmation)); any other field fails
Load with `generated_tool_wiring_invalid`, as does a row without
`pneumatic_io`. A jaw pair without its row fails with
`generated_tool_wiring_missing`, a row naming no jaw pair (or one twice) with
`generated_tool_wiring_unknown`. A `manyforge.tool` row without parameters
fails with `generated_tool_parameters_required`; parameters on any other
model with `parametric_tool_kind_unsupported`.

`model_source.parameters` may instead name a parameter file in the project
(`{project_path: ...}`, relative to the Scenario project folder, or
`{external_path: ...}`), for example to share a tool between projects; the
file holds the same parameters, and its component page then edits its
wiring only. Load reads the parameters (and the file) and their meshes once
and generates the URDF and SRDF from them. The meshes stay where they are:
Load keeps their content with the loaded model, which names each mesh by its
content, so the same parameters and the same mesh content always give the
same model, and nothing is written into the project folders.

#### Parameters

```yaml
api_version: manyforge.parametric_tool.v1
kind: tool
body:                          # one or more parts, at least one with collision
  - name: housing
    pose: {position_m: [0, 0, 0.03]}
    collision: {box: {size_m: [0.08, 0.08, 0.06]}}
chains:
  - attach_to: body            # the default; or an element of an earlier chain
    pose: {position_m: [0, 0, 0.06]}
    elements:
      - jaw_pair:
          id: outer
          positive_jaw:
            - name: outer_finger
              pose: {position_m: [0, 0.006, 0.03]}
              collision: {box: {size_m: [0.02, 0.008, 0.06]}}
          min_aperture_m: 0.0
          max_aperture_m: 0.04
tcps:
  - id: tcp                    # the first is the default TCP; its id is tcp
    attach_to: outer           # a tool end, or body (the default)
    pose: {position_m: [0, 0, 0.05]}
mass_kg: 0.6                   # or mass_kg (and center_of_mass_m) per part
center_of_mass_m: [0, 0, 0.04]
```

- Poses: `position_m` in metres and `orientation` as `rpy_deg` (fixed-axis
  roll, pitch, yaw, as URDF) or a quaternion `xyzw`; each optional (zero,
  identity). Body parts are in the mount frame (the robot flange plane).
- A part has a `name` (lower-case, unique in the parameters), a `pose`, and
  `collision` and `visual` geometry: exactly one of `box: {size_m: [x, y,
  z]}`, `cylinder: {radius_m, length_m}` (along its z axis, centred),
  `sphere: {radius_m}` or `mesh: {path, scale}`. The visual is the
  collision geometry unless given (`visual: none` shows nothing);
  `collision_pose` and `visual_pose` offset them from the part's pose.
- A mesh `path` is an `.stl` or `.dae` file under the project path rule, as
  for Scene meshes and model sources: relative to the parameter file's
  folder (inline parameters: the Scenario folder) or absolute. `..` may
  leave that folder, but the file must lie inside an allowed folder;
  otherwise Load refuses with `parametric_tool_mesh_unavailable`, naming the
  path and the allowed folders.
- Each chain hangs from the body or from an element of an earlier chain
  (`attach_to: <element id>`, with `jaw: positive|negative` for a jaw of a
  jaw pair), at its `pose`; its elements lead to its tool ends. Each element
  has an `id` (unique; `body` and `tool` are reserved) and a `pose` in the
  chain frame:
  - `fixed`: rigid `parts`, the link `tool_<id>`.
  - `jaw_pair` (tool end, actuated): two jaws on the pair frame's y axis.
    `positive_jaw` is the +y jaw's parts at the closed position; the -y jaw
    is the same parts turned 180 degrees about the pair's z, unless
    `negative_jaw` gives its own parts (in the pair frame). Optional
    `pads: {positive, negative}` (the same rule). `min_aperture_m` and
    `max_aperture_m` are the apertures at full closure and full opening;
    `open_aperture_m` (default the maximum) and `closed_aperture_m`
    (default the minimum, required with pads: the assumed grasp) are its
    `open` and `closed` states; `command_duration_s` (default 0.8 s) is its
    motion time. Each jaw travels `(aperture - min_aperture_m) / 2` on one
    commanded prismatic joint `tool_<id>_joint` and its mimic follower.
  - `spring_slider` (tool end): a spring-loaded rod, a fixed collision body
    `<id>` (link `tool_<id>`) with its `axis` (element frame, default z),
    `stroke_m`, `trip_depth_m` (at most the stroke), optional
    `release_depth_m` (above 0 and below `trip_depth_m`: the sensor's
    hysteresis) and `max_compression_step_m` (default 0.002) declared beside
    it. Load carries the values with the component. The rod does not move in
    the model: its compression is the modelled overlap. The `axis` points out
    of the tool along the rod; a push moves the rod back against it.
    - The slider is a frame of the tool, `<id>`, at the slider link. An
      object attached to it (carrier frame `{component_id: <tool>,
      frame_id: <id>}`) is carried by the slider: it presses the spring with
      the rod, as a tube in a holder. The attach node's target is the robot
      the tool is mounted on (its arm target), whose kinematics places the
      slider; the touch bodies are the tool's, for example
      `{component_id: <tool>, collision_body_id: <id>}`.
    - A move that presses the slider in names it in a `spring_slider` rule
      ([Springs](COLLISION_RULES.md#springs)): the limits are the slider's
      `stroke_m` and `max_compression_step_m`.
    - An input that follows the slider's trip uses the `spring_slider` form
      ([Mock inputs from geometry](#mock-inputs-from-geometry)): the trip
      and release depths are the slider's.
    `swept_space: true` (default false) adds a fixed collision body
    `<id>_swept` (link `tool_<id>_swept`, no visual) covering every
    position the parts reach over the stroke: a box, or a cylinder whose
    own axis is the slider axis, is lengthened by the stroke and moved
    back by half of it; a sphere becomes a cylinder between its end
    positions with a sphere at each end. Any other part (a mesh, or a box
    or cylinder at an angle to the axis) fails Load with
    `parametric_tool_swept_space_unsupported`. The swept body is an
    ordinary collision body of the tool, not one of its TCPs' contact
    bodies, and is not checked against the slider or what the slider is
    attached to.
  - `tip` (tool end): a probe tip, rigid `parts`, the link `tool_<id>`.
- `tcps`: the first is the default TCP, id `tcp` (link `tool_tcp`), later
  ones `tool_tcp_<id>`. A TCP belongs to a tool end (`attach_to`, its pose in
  the tool end's frame) or to the body. `contact_bodies` names collision
  bodies ([TCPs](TCPS.md#contact-bodies)); without it a TCP takes its tool
  end's: a jaw pair's pads (else its fingers), a tip's or a spring slider's
  own body; a TCP on the body has none.
- The mass is either the tool's `mass_kg` (with an optional
  `center_of_mass_m`, default the mount origin) or per-part `mass_kg`, never
  both. The robot's payload adds it while the tool is mounted. Without a
  mass the tool is 0 kg.

Parameters that do not fit fail Load with `parametric_tool_invalid` (the
message names the field), an unknown `kind` with
`parametric_tool_kind_unsupported`, and a missing mesh or one outside the
allowed folders with `parametric_tool_mesh_unavailable` (a swept space that cannot be built:
`parametric_tool_swept_space_unsupported`). Examples in
`manyforge_example_models/models/tool/`: `dual_jaw_gripper.tool.yaml` (two
jaw pairs), `spring_probe.tool.yaml` and `probe.tool.yaml` (a tip).

At Load, the component's type is derived from the parameters:

- `tool`: the body with every collision body and every TCP under its id,
  each with its contact bodies, and one frame per spring slider. The default TCP is `<component> · tool · tcp`,
  the robot's End-effector TCP while the tool is on its flange
  ([TCPs](TCPS.md)).
- one gripper target per jaw pair, under the pair's id, with the pair's
  group, commanded joint and collision bodies (`<id>_fingers`, and
  `<id>_pads` with pads). Its grip body (what may touch what it holds) is
  `<id>_pads` with pads, otherwise `<id>_fingers`. A Program's Close and
  Open nodes, the manual gripper control and a carried object's attachment
  address one pair as `<component> · <pair id>`; each pair has its own
  state, valve and Stop.

The collision bodies are `body`, each fixed element, tip and spring slider
by its id, and `<id>_fingers` and `<id>_pads` per jaw pair. The SRDF has one
group per jaw pair (with `open` and `closed` states) and disables the
adjacent link pairs only. As for every tool, Apply disables collisions
between the two rigid link groups that meet at the mount; list other tool
and arm link pairs that touch by design on the attachment row
(`collision_exclusions`), as in [Add a tool](#add-a-tool) step 7. An Apply
that leaves the Tool unchanged keeps it running, like any unchanged
component.

Outside the Composer, `scripts/manyforge component export <parameter file>
<folder>` writes the same `tool.urdf` and `tool.srdf` as Export (each mesh
named where it is, by its path relative to `<folder>`);
`--format urdf` or `--format srdf` writes one of them, `--format usda` is
refused (`parametric_tool_export_format_unsupported`).

A gripper with its own controller stays a component package
([Add a tool](#add-a-tool)), with its one target `gripper`.

### A project's own passive tool

A project that has its own URDF of a tool with nothing to drive uses the
generic passive tool, `manyforge.passive_tool`
(`manyforge_example_models/models/passive_tool/`): a mount frame, a TCP frame
and an optional tip, no joints, groups, gripper section or controller
(`lanes: {}`). Its components name no `lane` and run on every lane, Mock,
Isaac and Real, with any robot lane, on the host's generic adapter
([Lanes and adapters](#lanes-and-adapters)); it has no streams, and its
Components page shows its lane as **Every lane**. Its folder holds a neutral
placeholder model. Add it with Add component, kind Tool, group ManyForge,
**Tool from your URDF (passive)** (`manyforge.passive_tool`), and point its row at the project's files. The tool
folder (for example `tool/`) is then the package `passive_tool`:

- the URDF (any file name), with the root link `passive_tool_mount` (the
  robot flange plane) and a link `passive_tool_tcp` at the TCP, placed by its
  fixed joint's `origin`; the other links and their names are the project's.
  Use fixed joints only: nothing moves them. A link `passive_tool_tip` with
  `<collision>` geometry is the tool's `tip` (the default TCP's contact body);
  without such a link the tool has no tip;
- its meshes, `.stl`, `.dae`, `.png` or `.jpg`, anywhere in the folder.
  Every mesh URI is `package://passive_tool/<path in the folder>`, for
  example `package://passive_tool/meshes/body.stl` for
  `tool/meshes/body.stl`. A URI of another package fails Load
  (`activation_model_preparation_failed`);
- the tool's mass and centre of mass: `<inertial>` elements on its links.
  Load sums their masses and places the centre of mass in
  `passive_tool_mount`. A URDF without `<inertial>` is 0 kg;
- no SRDF: the passive tool has no groups, so a URDF alone is a complete
  model. Add one only for `disable_collisions` between the tool's own links;
  its `<robot name>` must then equal the URDF's.

```yaml
- component_id: holder
  component_profile_id: manyforge.passive_tool
  frame_prefix: ''
  model_source:
    urdf: {project_path: tool/holder.urdf}
    package_roots:
      passive_tool: {project_path: tool}
  target_settings: {}
  configuration:
    runtime_kind: ros2_control
    runtime_configuration: {component_streams: []}
```

The package root names this component's files only; the robot's row keeps
its own. Every link with `<collision>` geometry is checked; the links other
than the tip form the tool's `default_collision` body. Without an SRDF nothing
excludes parts that touch by design: pairs of the tool's own links go in an
SRDF's `disable_collisions`. Add more TCPs as custom TCPs on the component
page.

`scripts/manyforge component check manyforge_example_models/models/passive_tool
--scenario <scenario>` checks the rows: one attachment row from a free
`flange`, no `root_pose_in_cell`, no `lane`, no streams, and a
`model_source` naming a URDF with its SRDF and package roots (parameters are
for the Tool model, [A Tool from its parameters](#a-tool-from-its-parameters)).

## Set up a component's TCPs

The TCPs of a new robot or tool are set up in this order (the terms are in
[TCPs](TCPS.md#terms)):

1. **Import the vendor model**: the URDF and SRDF in the component folder
   (step 2 above). Vendor files never make a frame a TCP: a link named `tcp`
   stays an ordinary frame.
2. **Targets and frame roles**: name the frames the component uses
   (`roles.frames`, a tool's `mount.frame`). Every frame has one name.
3. **Mark the TCPs**: list each TCP in `tcps`. Only the listed frames are TCPs.
   Custom TCPs can be added later on the component page. A TCP's
   `contact_bodies` name the collision bodies meant to touch the work
   ([TCPs](TCPS.md#contact-bodies)).
4. **Choose a default if there are several**: the first `tcps` entry is the
   component's default. A component type with TCPs across multiple targets
   and no single type-declared default needs `default_tcp` on its Deployment
   row ([Add a tool](#add-a-tool) step 7). The selected default belongs to
   the component, wherever the tool is mounted.

A robot's End-effector TCP is the default TCP of the only tool on its flange,
or of the marked primary tool when there are several. If no tool is mounted
there, it uses the robot's default TCP, or its flange if it has no other
default. If the primary tool has no TCP, the robot has no End-effector TCP;
there is no fallback to the robot's own TCP or flange. When multiple tools
share an interface, mark exactly one tool's attachment row `primary: true` in
the Scenario's Deployment. A primary tool with no TCP still allows the cell to
load and Apply, with a non-blocking Deployment warning. Execute is refused
while any robot in the cell has no End-effector TCP. Declare a TCP on each
such robot's primary tool to provide one.

A Real robot component also has **Send TCP offset to controller** (selected by
default). Turn it off for instant TCP switching without sending TCP offsets;
the controller's own TCP features then refer to the flange. Explicit Program
and direct Scene attach/detach payload updates still run. See
[TCPs](TCPS.md#the-controllers-tcp-offset).

## Lanes and adapters

A Deployment component runs on one **lane**: `mock`, `isaac` or `real`. Every
lane runs robots on ros2_control with the stock JointTrajectoryController;
the lanes differ only in the hardware. The component's `lane` picks one row
of its model's `lanes`; a machine member runs on its owner's lane.

**Lane rows** (`lanes.<lane>` in `component.yaml`) name their `adapter`, and,
except on a simulator, the `controller` (the arm's JTC or the tool's gripper
controller). A robot's row also has `arrival_tolerance_rad` (every arm
joint's JTC `goal`) and `runtime_scale` (`supported` or `unsupported`:
whether its arm controller accepts runtime speed scaling; only a JTC that
claims `position` alone does, and Load refuses with
`component_runtime_scale_mismatch` when the generated controller differs from
the declaration), and may have `goal_time_s`. Controllers come from one of:

- `vendor: <family>` (and the vendor's keys, e.g. `vendor_model`): the vendor
  adapter's controllers, its base files plus ManyForge's additions, on this
  lane's hardware; used by xArm models on every lane;
- `controllers_file`: a controllers template in the model folder, for a model
  without a vendor adapter (UR, the tools).

The other keys are the adapter's own; each adapter's README lists them.

**Adapter folders.** Each lane or vendor adapter keeps its configuration in
one folder: `vendor/` for a vendor adapter, `lane/` for a lane adapter:

| Adapter | Folder |
| --- | --- |
| `mock` | `manyforge_ros2_control_execution/lane/` |
| `isaac` | `manyforge_isaac_adapter/lane/` (also `controllers.template.yaml`, `gripper_bridges/`) |
| `xarm` | `manyforge_xarm_control/vendor/` |

Each has `driver.yaml` (the source repository and pinned commit, the hardware
plugin, the base config files), `manyforge_settings.yaml` (ManyForge's
additions, each commented with its reason) and `README.md` (what the adapter
does, its row keys, how to add a model). Other families are `simulation_interfaces`
(the generic simulator adapter), `pointcloud` (the point-cloud sensor) and
`assistant` (the AI assistant).

**Adapter registry.** Every adapter family is a shared library registered in
the ament resource index `manyforge_component_adapters` (one resource per
family, named after it, whose content is the library's file name). The
library exports `manyforge_component_adapter_family()`, which returns the
family: its id (the row's `adapter`), the factory of one component's adapter
(an owner's, a member's or an owner-less component's; a member's factory gets
an opaque handle to its owner's adapter), its launch contribution, and
optional hooks: the vendor `controllers` template, the lane's own ros2_control
settings, the tool-context hook (TCP offset and payload, xArm
`ApplyToolContext`) and the display joint source. The execution host keeps no
list of vendors or lanes and links none of them; a row naming an unregistered
adapter fails with `component_adapter_unknown`. A new robot of a supported
vendor or lane is data (folders and rows); a new vendor's Real lane is a new
adapter family, which is code.

**Launch contributions.** For a ros2_control lane, the family returns the
component's launch contribution from its model row and its folder: the
hardware section (plugin, parameters, command and state interfaces), the
controllers YAML, optional driver parameters, remappings and, for Isaac, the
`universe` → `<prefix>world` transform. The generic generator
(`controller_launch_artifacts`) adds the instance values (namespace, the
device address from the site settings, the Stop deceleration) and writes
`robot_description`, the controllers file and the driver parameters. The
description and the joints come from the component's compiled controller
profile: the Deployment compiler writes the model's URDF with the frames the
cell adds (offset frames, custom TCPs, mounted tools, the fixed links of a
mounted tool without a process of its own) and the arm's joints in controller
order, or a tool's gripper joint;
`manyforge_ros_bringup/launch/ros2_control.launch.py` runs them. A machine's
process belongs to its owner, built with its arm member's model.

**Capabilities.** Besides the four lifecycle actions, an adapter declares
what it supports: `reset_position`, `power_off`, `reboot`, `io_manual_write`,
`io_output_readback`, and a simulator's `sim_reset`. Only declared actions
are offered. Declaring `sim_reset` identifies the component as a simulator;
its state never latches the cell or blocks Execute.

**Components without a lane** (a pneumatic tool driven by I/O, a passive
tool) run on the host's generic adapter: Prepare, Enable, Disable and Recover
succeed at once; its activity follows its own commands (busy during an open
or close; a passive tool is always idle); it has no streams.

**The AI assistant** has no lane either; its adapter runs a component process
(the ROS node `assistant_host`) next to the execution host:

| Action or event | Effect |
| --- | --- |
| Prepare, Recover | Start the host, which binds a loopback socket on a free port → `prepared` |
| Enable | Start the bridge on that socket; when its `/health` answers, `enabled` with the endpoint `assistant_request` (its URL) |
| Disable | Stop the bridge (SIGTERM, SIGKILL after 5 s): running chat requests end `cancelled`, the conversation history is dropped → `prepared` |
| A chat request runs | Activity `busy` (lags by up to 1 s) |
| `/health` answers | Stream `bridge` fresh (budget 5 s; none unless enabled; stale is a warning) |
| Host or bridge exits | `fault_process_exited`: latches the cell and blocks Execute; Recover, then Enable |

Its Enable does not wait for its AI model: the AI tab and Composer check the
model separately.

## Lifecycle

Each component has one lifecycle state:

| State | Meaning |
| --- | --- |
| `offline` | Its process or device isn't reachable (or, for a member, its owner isn't usable), with a reason |
| `not_prepared` | Reachable, not set up |
| `prepared` | Set up, not commandable (on a Real xArm: connected, errors cleared, motors off) |
| `enabled` | Commandable (on a Real xArm: motors on) |
| `fault` | Faulted; the fault says what to do (Enable, Recover or nothing) |

The actions are **Prepare**, **Enable**, **Disable** and **Recover**, plus its
capabilities; **Enable all** enables every prepared component. One action runs
at a time per component; a Stop interrupts one in progress. After any Apply,
Load included, the components it added or replaced come back **prepared, on
every lane**: nothing enables by itself. Execute is blocked with the reason
until they are enabled; Activate prepares and enables every active component,
then executes once.

**Streams.** A component's `component_streams[]` rows each carry
`{controllable_component_id, stream_id, budget_ns}`: the stream is fresh while
a sample arrived within `budget_ns`. What a stale stream means belongs to the
adapter, by kind: a robot's or I/O device's own stream going stale is a fault;
a pose source's, camera's, sensor's, AI model's or AI assistant's only shows as stale and
blocks Program conditions that read it. A robot or tool simulated in Isaac
whose simulation isn't playing keeps its lifecycle with the reason
`simulation_not_playing` and stale streams, never a fault; new motion and tool
commands on it fail with `simulation_not_playing` until the simulation plays.

### Lifecycle time budget

Every component has a lifecycle time budget: the maximum time one of its
lifecycle steps (Prepare, Enable, Disable, Recover) may take, for example how
long a robot may take to come up after activation. An Apply that installs or
replaces the component also waits for it.

- **Default.** 10 s, for every component kind and lane.
- **AI assistant.** Its Enable and Disable have no time limit: they wait until
  the host reports the bridge serving or stopped, the host exits, or a Stop
  interrupts them, and stay visible as running meanwhile. An Enable that does
  not complete stops the bridge again.
- **Override.** Set `lifecycle_command_timeout_s` (seconds, above 0, at most
  3600) in the component's `configuration`, or on its Components page
  ("Lifecycle command time limit"; blank keeps the default). An I/O device
  sets it in its `runtime_configuration.io_provider_devices` row (default
  10 s).
- An Apply waits for the sum of the budgets of the components it adds,
  replaces or removes; an Apply that changes none waits 10 s.
- The budget bounds the whole step, not each call: individual controller and
  vendor service calls must still answer within a few seconds, and one that
  does not is reported as an unresponsive component. A call's own wait (for
  example the few seconds a Real xArm takes to settle after arming) never
  runs past the step's budget.
- The component's adapter keeps to the budget. Group actions (**Enable all**,
  **Prepare all**) and an Apply's Disable and Prepare steps wait for each
  action's own result; a component that reports `component_owner_stalled`
  ends its step at once with that code.

A changed budget, like any other component setting, stays in the Draft and
takes effect at the confirmed Apply, which replaces that component.

## What an Apply restarts

Apply compares, per component, its **component spec**: what it is launched
and constructed with. A component is replaced (stopped, rebuilt and
prepared again) exactly when its spec changes; the spec's sections are the
reasons the preview (`POST /api/deployment/preview`) lists:

| Reason | What changed |
| --- | --- |
| `adapter` | Its adapter rows: lifecycle time budget, streams, I/O device, pose-source, AI-model, sensor or assistant settings (an assistant's include its AI model's URL and token variable name); the stream budget overrides it's built with (a robot's `measured_joints`, a pose source's streams) |
| `address` | A device address it uses, as resolved from the site settings |
| `bindings` | Its control bindings |
| `controllers` | Its control domains and controller configuration |
| `description` | Its component row, its models (custom TCPs included), the mounts on it (pose, interfaces, primary) and the mounted tools' models |
| `launch` | Its rows of the execution host's configuration (lane row, Stop profiles, motion limits, gripper and I/O signal rows, initial joints) |
| `owner` | For a member: its owner's spec |
| `providers` | The capability providers it cites |

Everything else is the **cell context**, refreshed at Apply without
restarting anything: policies and the other stream budget overrides (I/O
inputs, gripper feedback, sensors), takeover budgets,
the Scene and planning model (root poses, attachment rows with their
collision exclusions), pose sources, default TCPs (`default_tcp`), global
rules, planner settings and the Deployment's assistant settings (an AI
assistant component's own fields are its spec). Replacing or removing an
owner replaces or removes its members; the processes of removed and replaced
components stop before any new one starts.

## Descriptor fields

Common to every kind:

| Field | Required | Meaning |
| --- | --- | --- |
| `api_version` | yes | `manyforge.component_package.v2` (`v1` is still read; v2 added the optional I/O signal `index` and leaves a model-form I/O device's lane keys to its family) |
| `kind` | yes | `robot`, `tool`, `io_device`, `machine`, `sensor`, `assistant` or `simulator` |
| `id` | yes | `<vendor>.<name>`, each a lower-case portable id (`[a-z][a-z0-9_]*`). A Deployment component selects the model by it (`component_profile_id`); for a robot or tool `<name>` is the model family id |
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
| `roles.collision_bodies` | no | Collision roles to URDF links (`arm_collision`); see [Collision bodies](#collision-bodies) |
| `roles.instance_cardinality` | no | `multiple_per_cell` (default) or `one_per_cell` |
| `tcps` | yes | `[{id, frame}]`, one or more TCPs, each a frame role named by its `id`; the first is the default TCP. An `id` that is also a frame role (`roles.frames`, or `root`) must be on that role's link, else it is refused (`component_manifest_invalid`). Only the listed frames are TCPs: a frame merely named `tcp` is an ordinary frame. `tcp` is an ordinary name, allowed for any TCP. The descriptor checker verifies each frame exists |
| `limits.max_tcp_linear_speed_mps` | yes | The vendor TCP speed limit. Take it from the component's datasheet: checking the value is your responsibility, and ManyForge enforces whatever value is entered. `check` refuses a default or a Scenario TCP ceiling above it (`component_limit_exceeded`); Apply does not read it |
| `lanes.<lane>` | yes | One row per lane (`mock`, `isaac`, `real`): `adapter`, `controller`, `arrival_tolerance_rad`, `runtime_scale`, optional `goal_time_s`, `vendor` or `controllers_file` (a Mock row needs one of them), and the adapter's own keys ([Lanes and adapters](#lanes-and-adapters)) |
| `defaults` | yes | Values for a new Deployment component: `controlled_stop_max_deceleration_rad_s2` (required: the per-joint deceleration of the arm's controlled Stop, for example the `max_acceleration` in the vendor's MoveIt `joint_limits.yaml`), `initial_joint_positions_rad` (one per arm joint), `maximum_tcp_linear_speed_mps` (at or below the limit) |

Tool:

| Field | Required | Meaning |
| --- | --- | --- |
| `model.urdf`, `model.srdf` | yes | Files in the folder |
| `model.package_name` | no | The package of the mesh URIs; read from the URIs when absent. When it is the folder's own name, the folder is the package (`package://<name>/...`, as the passive tool) |
| `mount.frame` | yes | The URDF root link, the one on the robot flange (frame role `mount`) |
| `tcps` | yes | `[{id, frame}]`, each a frame role named by its `id`; the first is the default TCP. An `id` of `mount` (or a `roles.frames` role) must be on that role's link, else it is refused. `tcp` is an ordinary name, allowed for any TCP. A row may add `contact_bodies`, a list of `roles.collision_bodies` names ([TCPs](TCPS.md#contact-bodies)); without it a gripper's TCP takes `gripper_pads`, else `gripper_fingers`, and a passive tool's TCP none |
| `roles.groups.gripper` | with a gripper | The SRDF group with the `open`/`closed` states; a passive tool (no `gripper` section) has no groups |
| `roles.joints.fingers` | with a gripper | The commanded finger joint, not a mimic follower; a passive tool has no joints |
| `roles.collision_bodies` | no | `gripper_collision` and other collision roles to URDF links; see [Collision bodies](#collision-bodies) |
| `gripper.actuation` | with a gripper | `position_commanded` or `pneumatic` |
| `gripper.states` | with a gripper | `{open, closed}`: group_states of the gripper group |
| `gripper.attach_frame`, `gripper.grip_body` | with a gripper | The link a grasped object attaches to; the collision body (a `roles.collision_bodies` name, `gripper_collision` in the curated grippers) whose links hold it |
| `gripper.command_duration_s` | pneumatic | The valve dwell in seconds |
| `lanes.<lane>` | no | One row per lane: `adapter`, `controller` (the gripper controller), `controllers_file` on Mock, timeouts and the adapter's keys; no `arrival_tolerance_rad` or `runtime_scale`. A pneumatic tool may have none; a passive tool declares `lanes: {}` |
| `lanes.<lane>.gripper_stop_finish_tolerance` | Mock and `ros_gripper_command` Isaac rows | Finger distance to the target (the gripper's position unit, about 5% of the stroke) within which a Stop lets the command finish instead of cancelling it; see [Grippers](GRIPPERS.md#stop-near-the-target) |

### Collision bodies

Every URDF link with `<collision>` geometry is collision-checked, whether or
not a collision body names it. Collision bodies (`roles.collision_bodies`)
are names for groups of links, so that Programs and Deployments can refer to
parts of the model (for example the gripper's fingers as a TCP's contact
bodies, or a single body in a [collision rule](COLLISION_RULES.md)).
Links that no collision body names go to the component's `default_collision`
body. A collision body that names a link without `<collision>` geometry is
refused at Load. A curated robot or tool names every collision link in its
collision bodies.

I/O device: `signals` (required), each with `signal_name` (unique),
`direction` (`input` or `output`) and `value_type` (`bool`). Provider form:
`signal_name` is a portable id, and each signal has `provider` (`profile`
plus its fields) and, for an input, `freshness`, `transition_capability` and
`update_mode`. Model form: `signal_name` is the device's own name (e.g. `CI0`,
`TO1`), no provider, an optional `index` (a whole number, unique per
direction), and `lanes: {real: {adapter, input_maximum_age_ms, ...}}` whose
other keys are the adapter family's.

Machine: `default_owner_id` (portable id), `members` (`[{role, model}]`, each
role once, each model a curated robot or model-form I/O device) and `lanes`
(rows with at least `adapter`).

Sensor: `sensor` (`pointcloud`) and `adapter_row` (`{adapter, ...}`).
Assistant: `adapter_row` (`{adapter: assistant, backend: local_process,
health_period_s, stream_budget_s}`).
Simulator: `lanes` (rows with at least `adapter`).

## Check codes

| Code | Meaning |
| --- | --- |
| `component_manifest_invalid` | A descriptor field is missing, unknown or malformed, the YAML is invalid, or a machine member names no curated robot or model-form I/O device (the line names the field) |
| `component_model_unreadable` | The URDF or SRDF cannot be read |
| `component_role_unknown_link` | A frame, collision, TCP, mount or attach link is not in the URDF |
| `component_group_unknown` | A group role names no SRDF group |
| `component_joint_invalid` | A joint role names a missing, fixed or mimic joint |
| `component_state_unknown` | The gripper group lacks an `open`/`closed` state, or `gripper.states` names a missing one |
| `component_mesh_unresolved` | A mesh URI does not resolve in the package or maintained model source root |
| `component_lane_invalid` | A robot row without `arrival_tolerance_rad` or `runtime_scale` (`supported` or `unsupported`), a tool row with either, a Mock row with neither `vendor` nor `controllers_file`, a missing `controllers_file`, a passive tool without `lanes: {}`, or a Scenario lane the model lacks |
| `component_joint_count_mismatch` | Initial joint positions do not match the arm joints |
| `component_limit_exceeded` | A default or Scenario TCP ceiling is above `limits.max_tcp_linear_speed_mps` |
| `component_not_registered` | A robot's or tool's family row is missing, or a machine, model-form I/O device or sensor isn't in `component_models`; run `register` |
| `component_registration_stale` | A robot's or tool's family row differs from the descriptor; run `register` |
| `component_generated_missing` | A model asset or component type file is missing (step 6) |
| `component_scenario_mismatch` | A Scenario's component of this robot or tool does not match it (`--scenario`) |
| `component_attachment_invalid` | A Scenario's tool is not mounted from a free robot `flange` to its `mount`, carries `root_pose_in_cell`, or the `tool` role selects no mounted tool (`--scenario`) |
| `component_package_conflict` | Two folders have the same id and version with different content |
| `component_folder_exists` | `new` found a `component.yaml` already |

## Messages at Apply

| Message | Cause |
| --- | --- |
| `component X selects unknown model Y` | No registered model has that `id` (robot or tool step 5, machine step 3). |
| `component_lane_unknown` | The component's `lane` (or its owner's) is not a lane of its model. |
| `component_lane_required` | A component other than a pneumatic tool names no lane. |
| `prepared model differs from its repository-owned profile` | A mesh URI or package root does not use the family's package prefix. |
| `component_streams must declare exactly measured_joints [and gripper_feedback]` | Streams are derived per owning component: a robot arm declares `measured_joints`; a component owning a gripper feedback target also `gripper_feedback`. A lane-free tool declares none. |
| `tool_lane_required` | A lane-free tool whose gripper row isn't pneumatic. |
| `tool_pneumatic_io_required` | A lane-free pneumatic gripper row with `output_mode: commanded` has no `pneumatic_io`. |
| `deployment_member_role_unknown`, `deployment_member_role_repeated`, `deployment_member_missing`, `deployment_machine_part_without_machine` | A machine's member rows don't match its model ([Add a machine](#add-a-machine)). |
| `mount_primary_tool_required` | Several tools share an interface and not exactly one is `primary: true`. |
| `component_attachments` | An attachment row is invalid, e.g. a collision exclusion names a link its model lacks. |
| `component_adapter_unknown` | A lane row's `adapter` names no registered adapter family. |
