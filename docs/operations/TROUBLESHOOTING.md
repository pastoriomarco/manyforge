# Troubleshooting — Symptom → Cause → Diagnostic Step

Cross-cutting symptom-to-diagnostic-step matrix. From each row, jump to the
detailed section or runbook.

For where to look (logs, REST endpoints, container name), see
[`OBSERVABILITY.md`](OBSERVABILITY.md). REST paths below are on the Composer
API, `http://127.0.0.1:9020`.

## Quick symptom matrix

| Symptom | Most likely cause class | First diagnostic step |
| --- | --- | --- |
| `./scripts/compose-dev.sh start` fails | Image missing, port occupied, Docker daemon down | Read the start output; see the [workbench troubleshooting table](DEVELOPMENT_WORKBENCH.md#operation-and-troubleshooting) |
| `start` refuses the site settings | Relative path, missing file, unknown field, bad folder or address | See [§Site file errors](#site-file-errors) below |
| `start` succeeds but Execute is disabled | A component not enabled, in fault or offline; the cell latched | Read Execute's tooltip, or `GET /api/program/readiness` and `GET /api/cell/latch`; Execute offers **Activate and execute** when components only need Prepare/Enable |
| Start never reports ready | First start compiles the C++ targets; container died | `docker logs --tail 200 manyforge-development-workbench`; lower `MANYFORGE_DEV_BUILD_JOBS` if compilation exhausts memory |
| Tree runs but nothing moves | Motion groups still held (press **Run**), pending recovery offer, active alarm | See [`STACK_SETUP.md`](STACK_SETUP.md) §Troubleshooting |
| Alarm remains after Isaac/provider returns | Condition returned but its activation still requires acknowledgement | Inspect `lifecycle_state` in `GET /api/execution/alarms`; acknowledge the returned activation |
| Retry/Resume/Reset only works on the second press | Stale recovery offer or a lifecycle command still pending; this is not expected UX | Save cycle status, alarm snapshot, and execution history; see [§Alarm and recovery behavior](#alarm-and-recovery-behavior) |
| Motion refused with `validation_collision` | Stale pose source, scene drift, or genuine collision | See [§Collision rejection](#collision-rejection) below |
| Motion stopped with **Robot position feedback lost** (`runtime_monitor_observation_lost`) | The robot's measured joint states stopped arriving or became too old during motion, typically because the robot controller faulted or its connection dropped; nothing was detected in contact | Inspect the operation alarm in Runtime and its History entry; check the robot component's state and controller connection, then Recover the component |
| AI tab disabled or chat request fails | No `ai_model` component / assistant profile, model server down, token variable unset | See [§AI assistant](#ai-assistant) below |
| Isaac robot shows **Simulation not playing**; the run holds with "Waiting for Run · Simulation not playing" | Simulation paused or stopped, or the workbench can't reach Isaac (DDS) | See [§Simulation not playing](#simulation-not-playing) below |
| Isaac moves abort with `aborted_path_tolerance` ("State tolerances failed for joint …") although the plan is slow | Isaac runs slower than real time (overloaded host); the arm controller runs on wall-clock time | See [§Isaac slower than real time](#isaac-slower-than-real-time) |
| Isaac arm stays not prepared or offline | Controller not active, DDS isolation | See [§Isaac controller discovery failing](#isaac-controller-discovery-failing) below |
| Real machine won't prepare; members show offline with `owner_unavailable` | Robot unreachable, wrong address key, site file not set | See [§Real robot does not connect](#real-robot-does-not-connect) below |
| Machine fault `fault_estop` with C2 after an e-stop | Normal: the controller keeps the error until it is cleared | See [§Error left after an e-stop](#error-left-after-an-e-stop) below |
| A component process dies with SIGSEGV (exit code -11) or aborts (exit code -6) while it is being stopped | Known Fast DDS 3.6.2 / rclcpp 32.0.x shutdown bugs; harmless to motion | See [§Component process crashes on shutdown](#component-process-crashes-on-shutdown) below |
| Everything (components, Manager, Program state) goes stale for seconds at once on a Jetson | NVMe SSD lost an I/O completion (kernel waits 30 s) | See [§Everything goes stale for seconds at a time](#everything-goes-stale-for-seconds-at-a-time-jetson-nvme-stalls) below |
| An I/O device faults with **Every sender of this I/O device's inputs has gone** (`io_device_senders_gone`) | The process publishing its inputs (PLC bridge, ROS I/O node) stopped or left the network | See [§I/O device senders gone](#io-device-senders-gone) below |
| Composer scene mesh shows `missing` / `parse_failed` / `outside_allowed_roots` | Path resolution mismatch / malformed OBJ / allowlist | See [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md) §Troubleshooting table |

## Detailed flows

### Alarm and recovery behavior

Use these three surfaces together:

1. `GET /api/execution/alarms` — current conditions, lifecycle state, source
   quality, and acknowledgement identity.
1. `GET /api/cycle/status` — current `recovery_offer`, including exact action
   ids/revisions and whether confirmation or reset is required.
1. `GET /api/execution/history` — the latest 32 condition, alarm, recovery,
   and lifecycle events (page further with `before_sequence`).

An alarm's `lifecycle_state` is one of:

- `active_unacknowledged`: cause is present and operator attention is pending.
- `active_acknowledged`: operator saw it, but the cause remains active.
- `returned_unacknowledged`: the source proved return-to-normal; the occurrence
  still needs acknowledgement.
- `returned_acknowledged`: complete history; no current attention popup.

Stopping Isaac or another required provider after initialization sets a
provider-unavailable condition. Restarting a healthy stream returns the
condition, but does not acknowledge it for the operator.

One operator press should be sufficient for an enabled action. A stale request
is rejected without side effects. If the same action requires a second press,
capture both request responses plus the three surfaces above.
`acknowledge_and_continue` is a non-moving action that leaves the cycle held,
so a separate later Run/Resume press is required to plan and move.

### Collision rejection

**Symptom**: a motion is refused with the typed code `validation_collision`.
The same code also covers cancellation or an inconclusive fail-closed query;
use the message and `details` to distinguish them. Collision details include
the colliding pair and body ids when available. `details.classification:
clearance` means the trajectory passes closer than the move's collision
clearance (`collision_clearance_m`) to that pair without touching it
(`signed_clearance_m`); the planner retries a detour first. A goal (or start)
inside the clearance is refused by the planner (`planning_failed`,
`native_goal_state_collision`, `classification: clearance`). For deliberate
close work, set a smaller `clearance_m` on that motion node or in Robot
Control's Advanced settings, or lower the Deployment `collision_clearance_m`.

**Where to look**:

1. Runtime/history in Composer, or `GET /api/execution/history` — the
   refusal event and its details.
1. `GET /api/scene` — the backend-authoritative scene the planner used.
   **First place to look on the Isaac lane**: a stale Isaac pose can drive a
   rejection even when the viewer looks correct. Enable **Measured state
   ghosts** in the viewer to compare observed and planning poses.

**Common causes**:

- Stale pose source after a scene edit (Isaac lane especially). Check the
  object's `apply_policy` in the Deployment's `policies.scene_pose_sources`
  (see [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md) §4).
- Genuine collision the operator did not anticipate.

### AI assistant

**Symptom**: the AI tab says "No AI model component in this Deployment", or a
chat request fails with a typed code.

**Cause and step**: the tab needs an assistant profile and an `ai_model`
component for its chat alias in the active Deployment; requests need the model
server to serve the component's `model` and, when `api_key_env` is set, that
`MANYFORGE_AI_*` variable exported before starting the workbench. Check
`GET /api/assistant/modes` (`ai_model_component_id`), the `endpoint` component
stream, and `curl {base_url}/models`. Codes are listed in
[`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) §"Failure codes".

### Isaac slower than real time

**Symptom**: Isaac moves abort with `aborted_path_tolerance`; the controller
log shows "State tolerances failed for joint N … Position Tolerance: 0.2". The
same move succeeds at other times.

**Cause**: the Isaac lane's trajectory controller follows wall-clock time,
while the simulated arm moves at simulation speed. When Isaac runs below real
time (for example on a host busy with builds), every joint falls behind the
plan in the same proportion until one passes the 0.2 rad path tolerance.

**Steps**: keep Isaac at about real time: don't run heavy builds on the Isaac
host while it drives a Program, and check Isaac's real-time factor in its
status. Raising the tolerance does not help; a slow simulation still trips.

### Isaac controller discovery failing

**Symptom**: on an Isaac Scenario (for example
`examples/sim_ur10e_pick_and_place.scenario`), the arm component stays not
prepared or offline after Load.

**Where to look**:

1. `./scripts/compose-dev.sh status` — readiness and
   `unavailable_reasons` of the current Scenario.
1. Composer's Components page, or `GET /api/components`.
1. `docker logs --tail 200 manyforge-development-workbench`.

**ROS graph checks**, run inside the workbench container (the image entrypoint
sources ROS):

```bash
W=manyforge-development-workbench
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 topic list
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 control list_controllers
docker exec $W docker-entrypoint.ros-jazzy-full.sh ros2 action list
```

**Common causes**:

- DDS discovery isolation: a different `ROS_DOMAIN_ID`, RMW or DDS profile in
  Isaac Sim and in the shell that ran `./scripts/compose-dev.sh start`, or
  `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST` without `ROS_STATIC_PEERS` naming
  the Isaac host.
- The Isaac scene does not publish the joint-state topic named in the
  Deployment (for the UR10e example, `/isaac_joint_states`), or its ROS 2
  bridge is not enabled.

### Site file errors

`./scripts/compose-dev.sh start` reads `MANYFORGE_DEV_SITE_SETTINGS` and stops
with a message (exit 64) before it starts anything:

| Message | Fix |
| --- | --- |
| `Development site settings must be an absolute path` | Export the absolute host path, e.g. `export MANYFORGE_DEV_SITE_SETTINGS=$HOME/manyforge.site.json`. |
| `Development site settings file is unavailable: <path>` | The file doesn't exist at that path on the host. |
| `project_folder_invalid: site settings file must be JSON: …` | Fix the JSON syntax. |
| `project_folder_invalid: site settings schema must be manyforge.site.v1` | Add `"schema": "manyforge.site.v1"`. |
| `project_folder_invalid: site settings has unknown fields: …` | Only `schema`, `device_addresses` and `project_folders` are accepted. |
| `project_folder_invalid: project folder must be an existing absolute directory: …` | Create the folder, fix the path, or remove it from `project_folders`. |
| `project_folder_invalid: project_folders repeats …` / `unsupported project folder …` | List each folder once; paths can't contain `:` and folder names can't contain `,`. |

The Manager then validates the file once more at startup. A
`device_addresses` key must be a portable id (lower-case letters, digits, `.`,
`_`, `-`, starting with a lower-case letter) and each address a host name or IP address;
otherwise the Manager refuses to start and `start` ends with its log
(`site settings device_addresses.<key> must be a host name or IP address`).
After any change to the file, `stop` and `start` again: a running workbench
keeps the file it started with.

At Load or Apply, a Real component whose address can't be resolved is refused
with `site_settings_unavailable` (no site file configured) or
`site_device_address_ref_unknown` (the machine card's key is not in
`device_addresses`). A machine on the `real` lane needs an address key, and
only the `real` lane takes one.

### Simulation not playing

**Symptom**: on the Isaac lane, a robot or tool shows the reason
**Simulation not playing** and stale streams; a new motion or gripper command
is refused with `simulation_not_playing` and the run holds like after the
operator's Stop ("Waiting for Run"). This is never a fault and never latches
the cell; Enable, Activate and Execute stay available.

**Cause**: ManyForge sees no fresh samples from the simulator. Either the
simulation is paused or stopped, or the workbench does not reach Isaac at all.

**Steps**:

1. Press Play in Isaac Sim, or let the Program play it with a **Set simulation
   state** node (`play`) before its first motion. The reason clears when the
   samples are fresh again; then press **Run** to continue the held node.
1. If the simulation is playing and the reason stays, check DDS: the same
   `ROS_DOMAIN_ID` on both machines, and `ROS_AUTOMATIC_DISCOVERY_RANGE` unset
   or `SUBNET` (or `LOCALHOST` with `ROS_STATIC_PEERS` listing the Isaac host)
   in the shell that ran `start`. A running workbench keeps the values it was
   started with: `stop`, export, `start`.
1. Check that the topics arrive in the container:

   ```bash
   docker exec manyforge-development-workbench docker-entrypoint.ros-jazzy-full.sh ros2 topic list
   ```

### Real robot does not connect

**Symptom**: the machine's Prepare fails with `xarm_status_unavailable`, or
the machine is in fault `fault_communication`; its members show offline with
`owner_unavailable`.

**Steps**: `ping` the robot's address from the ManyForge host; check that the
machine card's **Address (site settings key)** names a key in
`device_addresses`, that `MANYFORGE_DEV_SITE_SETTINGS` was exported before
`start`, and that the control box is on; then Prepare (or Recover) the
machine. The full table is in
[Real robot setup](REAL_ROBOT_SETUP.md#troubleshooting).

### Error left after an e-stop

**Symptom**: after an emergency stop is released, the machine shows a fault
such as `fault_estop` with vendor code C2 (the control box emergency input),
and **Controller status** shows the error.

**Cause**: normal. The xArm controller keeps an error until it is cleared.

**Steps**: release the e-stop, **Recover** the machine (it clears the error
with the motors off), then **Enable** the arm. If Recover fails with
`robot_error_not_cleared`, the e-stop is still engaged. Servo errors
(C10–C17) are not cleared by Recover: power-cycle the robot.

### Component process crashes on shutdown

**Symptom**: a component process (an execution host, driver or ROS node)
dies with SIGSEGV, exit code -11, while it is being stopped, for example at
Close or Load. The backtrace ends in `~TypeLookupManager` and
`pthread_mutex_lock`.

**Cause**: a known Fast DDS 3.6.2 bug
([eProsima/Fast-DDS #6599](https://github.com/eProsima/Fast-DDS/issues/6599)).
A participant being destroyed can crash when another process asks it for
type information at that moment. It has not been seen on a live cell.

**Effect**: harmless to motion: the process was already being stopped.

**Steps**: none. The fix comes with an upstream Fast DDS release; ManyForge
updates Fast DDS when it ships.

A second, similar case: a controller process (`ros2_control_node`) aborts with
exit code -6 and `ExecutorNotifyWaitable::is_ready: Internal error, got index
out of range` right after "Shutting down the controller manager". This is a
race in rclcpp 32.0.x shutdown, fixed upstream in rclcpp 33.1.0
([ros2/rclcpp #3174](https://github.com/ros2/rclcpp/pull/3174)). It only
happens while the process is being stopped (about 1 in 50 teardowns), and
ManyForge records no fault for it. ManyForge takes the fix with the next rclcpp
update.

### I/O device senders gone

**Symptom**: a generic I/O device (ROS bool or GPIO inputs, e.g. a PLC bridge)
shows a fault "Every sender of this I/O device's inputs has gone"
(`io_device_senders_gone`); while it is not enabled it shows offline with that
reason, and Prepare, Enable and Recover are refused with the same code.

**Cause**: every publisher of the device's input topics has left the ROS graph
after at least one had been seen since install. A device that is only quiet,
or never had a sender, is not a fault. (Optional: a budget on the device's
**Inputs** stream in Components also faults it when no message arrives in time,
as `fault_stream_stale`.)

**Steps**:

1. Restart or reconnect the sender (the PLC bridge or ROS I/O node); check with
   `ros2 topic info <input topic>` that a publisher is back.
1. Recover the device in Components, then Enable.

### Everything goes stale for seconds at a time (Jetson NVMe stalls)

**Symptom**: on a Jetson host (seen on an Orin with an NVMe SSD), every
component, the Manager and the Program state go stale together for several
seconds up to about 30 s; New or Load may be refused meanwhile
(`manager_projection_unavailable`). The host's kernel log shows
`nvme nvme0: I/O tag … timeout, completion polled`.

**Cause**: the SSD lost an I/O completion and the kernel waits for its 30 s
I/O timeout. Every process waiting on that disk stalls, including the ManyForge
Manager, which writes its status files with a sync every 500 ms. A move already
sent to the controller keeps executing; ManyForge's supervision may be delayed
while it waits on the disk (being reviewed).

**Steps**:

1. Disable NVMe power saving: add `nvme_core.default_ps_max_latency_us=0` to the
   kernel command line (Jetson: `/boot/extlinux/extlinux.conf`, `APPEND` line)
   and reboot; check with `cat /sys/module/nvme_core/parameters/default_ps_max_latency_us`.
1. If it still happens, update the SSD firmware or use another drive.
1. Until fixed, don't run a real robot on that host.

### Lite6 trocar FoundationPose stream is unstable or missing

Use [`ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)
as the primary runbook. The short diagnostic order is:

1. Confirm the DDS environment is set in all three contexts: Isaac Sim host,
   Isaac ROS container, and the shell that runs `./scripts/compose-dev.sh start`.

   ```bash
   echo "$ROS_DOMAIN_ID"
   echo "$RMW_IMPLEMENTATION"
   echo "$ROS_AUTOMATIC_DISCOVERY_RANGE"
   echo "$ROS_STATIC_PEERS"
   ```

   If any value is empty or different from the runbook baseline, source the
   profile before launching that participant:

   ```bash
   MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
   source "${MANYFORGE_ROOT}/config/ros/isaac_lite6_trocar_foundationpose.env.sh"
   ```

1. Check low-payload graph info before subscribing to raw image topics:

   ```bash
   ros2 topic list --no-daemon --spin-time 20 -t | \
     grep -E 'image_rect|depth|camera_info|detections_output|/output'
   ros2 topic info --no-daemon --spin-time 20 -v /output
   ```

1. Echo `/output` with explicit type and QoS:

   ```bash
   ros2 topic echo /output vision_msgs/msg/Detection3DArray --once --timeout 120 \
     --qos-reliability reliable --qos-durability volatile
   ```

1. If ManyForge reports `tf_lookup_failed`, verify:

   ```bash
   ros2 run tf2_ros tf2_echo link_base camera_color_optical_frame
   ```

Known commissioning limitation: the Isaac ROS FoundationPose `/output` may
publish empty detection id/class, score `0.0`, and zero covariance. The trocar
deployment allows that only for the controlled single-trocar scene. Treat a
production cell with those quality fields still empty as unqualified.

## Where else troubleshooting lives

- [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)
  §"Operation and troubleshooting" — startup failures, port
  conflicts, slow first start, stale pages.
- [`STACK_SETUP.md`](STACK_SETUP.md) §"Troubleshooting" — tree not progressing,
  model selection ignored.
- [`REAL_ROBOT_SETUP.md`](REAL_ROBOT_SETUP.md) §"Troubleshooting" — xArm
  connection, faults and Enable failures.
- [`ISAAC_SCENE_AUTHORING.md`](ISAAC_SCENE_AUTHORING.md)
  §"Troubleshooting" — Isaac mesh inventory, transfer, parse, allowlist issues.
- [`ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md) — AI assistant
  setup, checks and failure codes.

## Capturing a failure for later analysis

See [`OBSERVABILITY.md`](OBSERVABILITY.md) §"Capturing a bug report"
for the full capture recipe. The minimum bundle:

```bash
RUN_ID=<program_run_id from Runtime/history>
API=http://127.0.0.1:9020
curl -s "$API/api/debug/export?run_id=${RUN_ID}" > debug-export.json
curl -s "$API/api/execution/history?program_run_id=${RUN_ID}" > history.json
curl -s "$API/api/execution/alarms" > alarms.json
curl -s "$API/api/cycle/status" > cycle-status.json
docker logs --tail 2000 manyforge-development-workbench > workbench.log
```

Attach also the start command (`./scripts/compose-dev.sh start --project …`)
and the `./scripts/compose-dev.sh status` output.
