# Operator launcher (advanced)

`manyforge-operator` starts, checks and stops one ManyForge stack from a
materialized, digest-checked package, and serves Composer on the network at
port 9000. It keeps a launch record so that `status` and `stop` act only on the
processes or container it started.

For everyday use, follow the
[workbench guide](DEVELOPMENT_WORKBENCH.md) instead. The workbench builds from
your checkout, serves Composer with hot reload at `http://localhost:3000`,
covers the Mock, Isaac and Real lanes, and needs no package preparation. This
page covers only what differs for the operator launcher.

Use the operator launcher when you need one of these:

- ManyForge runs on a separate Docker host, Isaac Sim runs on another machine,
  and you control both from one machine (`isaac` profile);
- the stack must run from a fixed package root whose aggregate digest is
  checked at start, with Composer reachable from the LAN at port 9000;
- `status` and `stop` must be authenticated against a recorded launch (exact
  container ID, image and labels, or exact process identities).

ManyForge Stops are not safety-rated. The software Stop is not an emergency
stop; a robot installation needs its own safety system.

## Profiles

| Profile | Where the stack runs | Requirements |
|---|---|---|
| `isaac` | In a container on a remote runtime host, started over SSH through `scripts/compose-dev.sh` in operator mode. Isaac Sim runs on its own host; ManyForge attaches to it. | Docker on the runtime host; the `manyforge-ros-lyrical-full` image (ROS 2 Lyrical) built there; SSH key login. No ROS install on either host. |
| `native` | As host processes on the machine that runs the command. | **A host ROS 2 install** matching the build, a host build of ManyForge, and systemd journald. |

The `native` profile needs ROS on the host: the Manager starts the ROS
execution host and controllers as host processes. If you do not want ROS on
the host, use the workbench or the `isaac` profile.

## What it starts

Both profiles start the same five processes. The Manager in turn owns the ROS
execution host and the planning worker.

| Process | Port | Bound to |
|---|---|---|
| Composer (built frontend, no Vite) | 9000 | all interfaces |
| Headless API | 9010 | loopback |
| Program runner | 9011 | loopback |
| Assistant bridge | 8100 | loopback |
| Manager | none (local sockets) | |

All four ports must be free; ports are fixed. The workbench uses 3000 and 9020
for its frontend and Composer instead of 9000. Composer has no login: expose
port 9000 only on a trusted network.

`start` waits until the owners report ready and leaves the stack idle. It does
not Load a Scenario, Apply or Run.

## Prerequisites

- A clean Git checkout (no modified or untracked files). Start refuses a dirty
  source. For `isaac`, the runtime host needs a clean checkout of the same
  commit; the two source identities must match exactly.
- Python 3.11 or later with `pydantic` 2 and `rfc8785`. Either install the
  tool with `pip install ./manyforge_operator`, which provides the
  `manyforge-operator` command, or run `python3 -m manyforge_operator` from the
  repository root.
- A package root (next section) and the node and alarm catalogs:
  `manyforge_behavior/resources/node_catalog.yaml` and
  `manyforge_headless/resources/operator-alarm-catalog.v1.yaml`.

## Prepare the package root

The package is produced by the maintained materializer
`manyforge_maintained_deployment_occurrences`, built with `manyforge_deployment`.
It requires one prepared model root for each of the five maintained robot
families. A `manyforge_deployment` build configured with `BUILD_TESTING`
creates them under `generated/<family>_structural` in its build directory.

| Family ID | Robot and gripper |
|---|---|
| `lite6_lite_gripper` | UFactory Lite 6, Lite gripper |
| `uf850_xarm_gripper` | UFactory UF850, xArm gripper |
| `ur10e_robotiq_2f_140` | UR10e, Robotiq 2F-140 |
| `ur5_roboplan` | UR5 (RoboPlan example model) |
| `ur5e_robotiq_2f_85` | UR5e, Robotiq 2F-85 |

```bash
PACKAGE_ROOT=/absolute/path/to/new-empty-package-root
mkdir "$PACKAGE_ROOT"
"$MATERIALIZER" --check \
  --workspace-root "$MANYFORGE_ROOT" --package-root "$PACKAGE_ROOT" \
  --prepared "lite6_lite_gripper=$GENERATED/lite6_lite_gripper_structural" \
  --prepared "uf850_xarm_gripper=$GENERATED/uf850_xarm_gripper_structural" \
  --prepared "ur10e_robotiq_2f_140=$GENERATED/ur10e_robotiq_2f_140_structural" \
  --prepared "ur5_roboplan=$GENERATED/ur5_roboplan_structural" \
  --prepared "ur5e_robotiq_2f_85=$GENERATED/ur5e_robotiq_2f_85_structural"
PACKAGE_SHA256=$(python3 -m manyforge_operator.package_aggregate \
  --package-root "$PACKAGE_ROOT")
```

Discard a root whose materialization failed. Do not change a root after
taking its digest: `start` recomputes the aggregate and refuses a mismatch.
The package holds one empty-cell admission file per cell,
`canonical-empty-admission.<cell_id>.json`. Pass one of them with
`--canonical-empty-admission-filename` and the same cell with `--cell-id`.

`--compiler-build-manifest-sha256` and `--node-environment-sha256` are
recorded as provenance in the launch record. The launcher checks only that
they are 64 lowercase hex characters; take them from your build record.

## Commands

Global options come before the command:

```text
manyforge-operator [--state-root DIR] [--instance-id ID] [--json] {start,status,stop}
```

- `--state-root`: where launch records live (default
  `.manyforge-var/operator` under the current directory).
- `--instance-id`: one launch instance (default `native`). Use the same
  value for `status` and `stop`.
- `--json`: print one `manyforge.operator_command_result.v1` JSON object.

`status` and `stop` read the profile from the saved record and take no other
options.

### `isaac` profile

Run on the machine you control from. Example:

```bash
manyforge-operator --state-root "$STATE_ROOT" --instance-id isaac --json \
  start --profile isaac --source-root "$MANYFORGE_ROOT" \
  --remote-target USER@RUNTIME_HOST \
  --advertised-host RUNTIME_HOST \
  --simulator-host ISAAC_HOST \
  --static-peer ISAAC_HOST --static-peer RUNTIME_HOST \
  --remote-source-root "$R/manyforge" \
  --remote-container-image-id "$IMAGE_ID" \
  --remote-container-name manyforge-operator-isaac \
  --remote-cache-root "$R/cache" \
  --remote-runtime-root "$R/runtime" \
  --remote-evidence-root "$R/evidence" \
  --remote-install-root "$R/package" \
  --remote-node-catalog-path "$R/manyforge/manyforge_behavior/resources/node_catalog.yaml" \
  --remote-alarm-catalog-path "$R/manyforge/manyforge_headless/resources/operator-alarm-catalog.v1.yaml" \
  --remote-simulation-control-config "$R/manyforge/manyforge_ros_bringup/config/isaac_program_simulation_site.json" \
  --remote-manager-executable "$R/unused/manyforge_manager_daemon" \
  --remote-state-root "$R/operator-state" \
  --remote-bridge-state-root "$R/bridge-state" \
  --remote-ros-setup-script /opt/ros/lyrical/setup.bash \
  --remote-ros-environment-script "$R/unused/ros_env.sh" \
  --cell-id isaac_ur10e_light_cell \
  --canonical-empty-admission-filename canonical-empty-admission.isaac_ur10e_light_cell.json \
  --package-aggregate-sha256 "$PACKAGE_SHA256" \
  --compiler-build-manifest-sha256 "$COMPILER_SHA256" \
  --node-environment-sha256 "$NODE_ENV_SHA256"
```

`R` stands for an absolute directory on the runtime host. Get the image ID on
the runtime host with
`docker image inspect --format '{{.Id}}' manyforge-ros-lyrical-full`.

| Option | Meaning |
|---|---|
| `--source-root` | Required. Local clean checkout. |
| `--remote-target` | Required: your runtime host, as an SSH target (`user@host`). Key login must work without a prompt. |
| `--advertised-host` | Required: your runtime host's address. Composer is at `http://<advertised-host>:9000/`. |
| `--simulator-host` | Required: your Isaac host. Recorded in the launch identity. |
| `--static-peer` | Required, repeatable: your Isaac host and your runtime host. Becomes `ROS_STATIC_PEERS`. |
| `--remote-source-root` | Required. Clean checkout on the runtime host, same commit as `--source-root`. |
| `--remote-container-image-id` | Required. Full `sha256:<64 hex>` ID of the Lyrical image on the runtime host. The launcher neither pulls nor builds images. |
| `--remote-container-name` | Required. Docker container name. |
| `--remote-cache-root`, `--remote-runtime-root`, `--remote-evidence-root` | Required absolute paths: build cache, persistent runtime state (not `/tmp`, `/run` or tmpfs), evidence exports. |
| `--remote-install-root` | Required. Package root on the runtime host; mounted read-only. Its aggregate is checked again in the container. |
| `--remote-node-catalog-path`, `--remote-alarm-catalog-path` | Required. Catalog files on the runtime host. |
| `--remote-simulation-control-config` | Required. Isaac simulation-control site file. |
| `--cell-id`, `--canonical-empty-admission-filename` | Set both to a cell in the package. |
| `--package-aggregate-sha256`, `--compiler-build-manifest-sha256`, `--node-environment-sha256` | Required digests (see above). |
| `--remote-composer-static-dir` | Optional. Built frontend with `index.html`; default is the one in the image. |
| `--remote-python-executable` | Python on the runtime host, used for the source check (default `python3`). |
| `--remote-manager-executable`, `--remote-state-root`, `--remote-bridge-state-root`, `--remote-ros-setup-script`, `--remote-ros-environment-script` | Required absolute paths that are validated and recorded but not used by the container, which builds and stages its own runtime and gets ROS from the image. Pass paths on your runtime host. |
| `--control-host` | Recorded name of the controlling machine (default: its hostname). |
| `--joint-commands-topic` | Recorded in the launch identity only. |
| `--readiness-timeout-s` (60, max 600), `--poll-interval-s` (0.25), `--manager-timeout-s` (60, max 60) | Timeouts. |

The ROS topology is fixed: `ROS_DOMAIN_ID=0`, `rmw_fastrtps_cpp`,
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, and the static peers above. Isaac
Sim's ROS bridge must use the same settings.

Isaac Sim is attach-only. The launcher does not start, stop or play it. Start
Isaac Sim on the Isaac host and open the scene before you Load the Scenario.

`start` checks the local and remote source identities, starts the container
(the call allows about 15 minutes, including the incremental build), waits
for Manager, Headless, Program runner and Composer to report ready, and checks
the remote source again. The container carries the labels
`manyforge.launch-instance` and `manyforge.launch-input-digest`; `status` and
`stop` act only on the recorded container ID with that image and those labels.
`stop` exports evidence as described in the workbench guide, then removes the
container.

### `native` profile

Requires a host ROS 2 install (see [Profiles](#profiles)). Run in a shell where
that ROS environment is sourced and where `manyforge_headless_api`,
`manyforge_program_runner`, `manyforge_composer` and
`manyforge_assistant_bridge` import from `python3`.

```bash
manyforge-operator --state-root "$STATE_ROOT" --instance-id native --json \
  start --profile native --source-root "$MANYFORGE_ROOT" \
  --manager-executable "$BUILD/manyforge_manager_daemon" \
  --install-root "$PACKAGE_ROOT" \
  --node-catalog-path "$MANYFORGE_ROOT/manyforge_behavior/resources/node_catalog.yaml" \
  --alarm-catalog-path "$MANYFORGE_ROOT/manyforge_headless/resources/operator-alarm-catalog.v1.yaml" \
  --cell-id lite6_pick_place_cell \
  --canonical-empty-admission-filename canonical-empty-admission.lite6_pick_place_cell.json \
  --advertised-host RUNTIME_HOST \
  --package-aggregate-sha256 "$PACKAGE_SHA256" \
  --compiler-build-manifest-sha256 "$COMPILER_SHA256" \
  --node-environment-sha256 "$NODE_ENV_SHA256"
```

| Option | Meaning |
|---|---|
| `--manager-executable` | Required. Built `manyforge_manager_daemon`. The built `manyforge_ros2_control_execution_host` and `manyforge_roboplan_candidate_worker` must be in the same directory. |
| `--install-root` | Required. Package root. |
| `--node-catalog-path`, `--alarm-catalog-path` | Required. |
| digests | Required (see above). |
| `--cell-id`, `--canonical-empty-admission-filename` | Set both to a cell in the package. |
| `--advertised-host` | Address used in the Composer URL (default `localhost`). |
| `--composer-static-dir` | Optional built frontend with `index.html`. |
| `--simulation-control-config` | Optional simulation-control site file for the Program runner. |
| `--expected-source-fingerprint`, `--expected-head-commit`, `--expected-head-tree` | Optional; start refuses a source that differs. Also accepted by `isaac`. |
| `--readiness-timeout-s` (60, max 300), `--poll-interval-s` (0.25), `--reconciliation-period-ms` (500), `--manager-timeout-s` (60, max 60) | Timeouts. |

The launcher keeps all state under `$STATE_ROOT/$INSTANCE_ID/`: durable state
in `state/` (the launch record is `state/operator/launch_instance.json`),
sockets and rebuildable files in `run/`. It creates and checks this layout
itself; paths cannot be overridden. Output goes to journald: read it with
`journalctl -t manyforge-<instance-id>-<component>` (`manager`, `headless`,
`program_runner`, `assistant_bridge`, `composer`).

## Using Composer

Open `http://<advertised-host>:9000/` once `start` or `status` reports
`ready`. The Composer flow is the same as with the workbench:

1. **Load Scenario** to open a Scenario, or **New** to open the empty
   Untitled Scenario. Both close the current Scenario first; the result loads
   as a Draft. Close can proceed with a latched Stop fault when the required
   effects and teardown are confirmed: it acknowledges open component
   incidents (recorded in History), disables eligible components without a
   recovery and stops their processes; a faulted component, or one whose
   Disable is confirmed to have no effect, is only process-stopped. An unknown
   output or operation effect, or another unresolved condition, refuses Close.
   Close keeps an open cell Stop fault; **Recover cell** cannot clear that latch
   while Close is in progress (component acknowledgements in the same press may
   still happen). After Close, load a Scenario with the robots the fault names,
   then press **Recover cell** when its reported conditions, including any
   required rest proof, are satisfied (right after Load it may report that
   the configuration is not installed yet; press again shortly). No robot
   moves until then.
2. Review the Draft. **Discard** drops unapplied changes; **Revert** returns
   to the saved files.
3. **Apply** makes the Draft the running configuration.
4. Start the Program with **Run**; use **Stop**, **Resume** and **Reset** as
   needed.
5. **Save Scenario** or **Save Scenario as** to keep your changes. The
   Untitled Scenario is read-only: **Save Scenario** is disabled for it and
   **Save Scenario as** names and places it, then continues in the saved
   project. Unsaved Untitled changes are lost when Composer restarts.

Where Scenarios can be loaded and saved depends on the profile:

- `native`: Composer can read the checkout's `examples/`, the package root and
  the instance workspace
  `$STATE_ROOT/$INSTANCE_ID/state/composer-authoring/program-workspace`. Only
  that workspace is writable, so use **Save Scenario as** into it. It survives
  `stop` and `start`.
- `isaac`: Composer shows the runtime host checkout's `examples/` folder. A
  Save there changes that checkout, and the next `start` refuses the dirty
  source. Commit or move the saved files before starting again.

## Status and failures

| State | Meaning |
|---|---|
| `starting` | Processes are starting; readiness is pending. |
| `ready` | Every owned process or the container is present and every owner reports ready. |
| `degraded` | A process, the container, or an owner readiness check is missing or failed. |
| `stopping` | Shutdown is in progress. |
| `stopped` | The recorded processes or container were observed absent. |
| `unavailable` | No readiness observation could be obtained. |

`start` on a `ready` instance with the same configuration reports
`already_running`; on a `degraded` instance it first cleans up what the record
owns. Repeating `stop` on a stopped instance reports `already_stopped`.

| Code | What to do |
|---|---|
| `operator_configuration_invalid` | A flag is missing or invalid; the message names it. |
| `source_dirty`, `source_identity_mismatch` | Use a clean checkout; for `isaac`, the same commit on both hosts. |
| `package_aggregate_mismatch`, `package_aggregate_unavailable` | Recompute the digest of the exact package root; do not edit the root. |
| `port_occupied` | Free 9000, 9010, 9011 or 8100. Ports cannot be remapped. |
| `journald_unavailable` | `native` needs `systemd-cat` and the journald socket. |
| `readiness_timeout`, `isaac_readiness_timeout`, `runtime_container_unready` | Read the logs (`journalctl` or `docker logs <container-id>`). |
| `runtime_host_unavailable`, `remote_protocol_invalid` | SSH or the runtime host failed. Absence cannot be proved; restore the host, then run `status`. |
| `container_identity_mismatch` | Another container has that name. It is not adopted. |
| `incompatible_owner`, `stale_launch_record` | The instance has a different configuration or still owns resources. `stop` it, or use another `--instance-id`. |
| `status_record_invalid` | The record is unreadable, or it is an `isaac` record from an older launcher that started Isaac Sim itself. Stop the old runtime, then use a new instance. |
| `cleanup_deadline_exceeded` | `stop` could not prove everything stopped. Investigate, then run `stop` again. |
| `instance_unavailable` | No record exists for this state root and instance ID. |

If the Manager logs `shutdown stuck at <phase>; to force: manyforge-operator
stop`, run `stop` with the same `--state-root` and `--instance-id`. It ends
only the recorded processes (TERM, then KILL). Never kill ManyForge processes
by name.

To replace the package root, `stop` and `start` again; there is no live
package upgrade.

## Inspection

Composer serves `/api/health`, `/api/manager/status`, `/api/runtime` and
`/openapi.json`. See the [Composer REST API](../reference/REST_API.md) and the
[Headless REST API](../reference/HEADLESS_REST_API.md).
