# Operator launcher (advanced)

`manyforge-operator` starts, checks and stops one ManyForge stack from a
materialized, digest-checked package, and serves Composer on the network at
port 9000. It keeps a launch record so that `status` and `stop` act only on the
processes or container it started.

This is install option 4 of the
[workbench guide](DEVELOPMENT_WORKBENCH.md#install). For everyday use, follow
that guide's option 1 (the prebuilt ManyForge image, Composer at
`http://localhost:9000`) or, for development, option 2 or 3 (built from your
checkout, Composer with hot reload at `http://localhost:3000`) instead: they
cover the Mock, Isaac and Real lanes and need no package preparation. This page covers only
what differs for the operator launcher.

Use the operator launcher when you need one of these:

- ManyForge runs on a separate Docker host, Isaac Sim runs on another machine,
  and you control both from one machine (`isaac` profile);
- the stack must run from a fixed package root whose aggregate digest is
  checked at start, with Composer reachable from the LAN at port 9000;
- `status` and `stop` must be authenticated against a recorded launch (exact
  container ID, image and labels, or exact process identities).

In short:

1. **Install.** Meet the [prerequisites](#prerequisites). For the `isaac`
   profile, get the workbench image on the runtime host first, with
   `./scripts/compose-dev.sh pull` or `build` in its checkout
   ([workbench guide](DEVELOPMENT_WORKBENCH.md#install)); the launcher neither
   pulls nor builds images. Then
   [prepare the package root](#prepare-the-package-root).
1. **Start** with the [command of your profile](#commands).
1. **Verify.** `manyforge-operator … status` reports `ready`
   ([Status and failures](#status-and-failures)); then open
   `http://<advertised-host>:9000/`.
1. **Update.** `stop`, move every checkout to the new commit, update the
   image on the runtime host (`pull` or `build`) and pass its new ID, prepare
   a new package root and digest with the new build, then `start`.
1. **Fall back.** The workbench (`./scripts/compose-dev.sh start`) runs the
   same application from the checkout, without a package.

ManyForge Stops are not safety-rated. The software Stop is not an emergency
stop; a robot installation needs its own safety system.

Real robots: this launcher reads only the disk limits from a site settings
file (`--site-settings`), so robot addresses can't be configured here. Use the workbench and follow
[Real robot setup](REAL_ROBOT_SETUP.md).

## Profiles

| Profile | Where the stack runs | Requirements |
|---|---|---|
| `isaac` | In a container on a remote runtime host, started over SSH through `scripts/compose-dev.sh` in operator mode. Isaac Sim runs on its own host; ManyForge attaches to it. | Docker on the runtime host; the `manyforge-ros-lyrical-full` image (ROS 2 Lyrical) pulled or built there; SSH key login. No ROS install on either host. |
| `standard` | As host processes on the machine that runs the command. | **A host ROS 2 install** matching the build, a host build of ManyForge, and systemd journald. |

The `standard` profile needs ROS on the host: the Manager starts the ROS
execution host and controllers as host processes. If you do not want ROS on
the host, use the workbench or the `isaac` profile.

## What it starts

Both profiles run the same four processes. The launcher starts only the
Manager. The Manager starts Headless, the Program runner and Composer, and
owns the ROS execution host and the planning worker; the execution host
starts each component's processes, including the AI assistant's bridge.

| Process | Port | Bound to |
|---|---|---|
| Composer (built frontend, no Vite) | 9000 | all interfaces |
| Headless API | 9010 | loopback |
| Program runner | none (local sockets) | |
| Manager | none (local sockets) | |

Both ports must be free; ports are fixed. The workbench uses 3000 and 9020
for its frontend and Composer instead of 9000. Composer has no login: expose
port 9000 only on a trusted network.

Every start begins with an empty cell: nothing from an earlier run is
restored. `start` waits until the owners report ready and leaves the stack
idle. It does not Load a Scenario, Apply or Run.

The Manager supervises the whole application. If any of these processes, or
the execution host, exits, the Manager ends the application: it stops the
cell, stops the other processes and exits, and its log names the process that
exited (`<process> exited with status N`). Nothing is restarted on its own;
start the instance again. Every end of the application, `stop` included,
first asks for a Halt of the cell and waits until it is complete before the
processes are stopped; then each process and the execution host are waited
for until they exit. No step has a time limit, and the log names the step
being waited for. Each `stop`, Ctrl+C or SIGTERM to the `start` process is
one stop request: the first starts the stop, a second ends the processes
still running. `stop` waits until the application has ended. If that Stop is refused or cannot
be followed, the log shows `manager_planned_stop_overrun` or
`execution_host_planned_stop_overrun`; check that the robot has stopped
before starting again. If a step of the stop did not complete, `start` ends
with `application_stop_incomplete` and the next start reports that the
shutdown was unclean or could not be verified.

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

The package root is an empty-cell package for one cell. It is written by
`manyforge_generate_canonical_empty_package`, a `manyforge_deployment` tool.
The workbench build step builds it: after `./scripts/compose-dev.sh
build-native` it is
`<cache base>/images/<image id>/build/deployment-authoring/manyforge_generate_canonical_empty_package`
(`<cache base>` is `.manyforge-dev` in the checkout unless
`MANYFORGE_DEV_CACHE_BASE` is set). A host build of `manyforge_deployment`
builds the same target.

```bash
PACKAGE_ROOT=/absolute/path/to/new-empty-package-root
CELL_ID=lite6_pick_place_cell
mkdir "$PACKAGE_ROOT"
"$GENERATOR" --output-root "$PACKAGE_ROOT" --cell-id "$CELL_ID" \
  --ros-runtime-profile "$MANYFORGE_ROOT/manyforge_deployment/resources/maintained/catalog/runtime_profile/manyforge.ros2_control_execution_runtime.json"
PACKAGE_SHA256=$(python3 -m manyforge_operator.package_aggregate \
  --package-root "$PACKAGE_ROOT")
```

`GENERATOR` is the path of the tool and `MANYFORGE_ROOT` the checkout; run
the last command from the repository root. `CELL_ID` is a name you choose: a
lowercase letter, then lowercase letters, digits, `_`, `-` or `.`, at most
128 characters.

Discard a root whose generation failed. Do not change a root after
taking its digest: `start` recomputes the aggregate and refuses a mismatch.
The package holds the empty-cell admission file of its cell,
`canonical-empty-admission.<cell_id>.json`. Pass it with
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
- `--instance-id`: one launch instance (default `workbench`). Use the same
  value for `status` and `stop`.
- `--json`: print `manyforge.operator_command_result.v3` JSON objects, one per
  line. `start` prints one object, except the standard profile, which prints
  two: `started` once the owners are ready, then `stopped` when the
  application ends.

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
| `--remote-composer-static-dir` | Optional. Built frontend with `index.html`. Default: the build step builds `manyforge_composer/frontend/dist` from the remote checkout. |
| `--remote-python-executable` | Python on the runtime host, used for the source check (default `python3`). |
| `--remote-manager-executable`, `--remote-state-root`, `--remote-bridge-state-root`, `--remote-ros-setup-script`, `--remote-ros-environment-script` | Required absolute paths that are validated and recorded but not used by the container, which builds and stages its own runtime and gets ROS from the image. Pass paths on your runtime host. |
| `--control-host` | Recorded name of the controlling machine (default: its hostname). |
| `--joint-commands-topic` | Recorded in the launch identity only. |
| `--readiness-timeout-s` (60, max 600), `--poll-interval-s` (0.25), `--manager-timeout-s` (60, max 60) | Validated and recorded but not used by the container; its build and start calls have the fixed bounds below. |

The ROS topology is fixed: `ROS_DOMAIN_ID=0`, `rmw_fastrtps_cpp`,
`ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, and the static peers above. Isaac
Sim's ROS bridge must use the same settings.

Isaac Sim is attach-only. The launcher does not start, stop or play it. Start
Isaac Sim on the Isaac host and open the scene before you Load the Scenario.

`start` checks the local and remote source identities, runs the incremental
native build step as its own call (`compose-dev.sh build-native`, allowed 45
minutes for a cold build; it also builds the frontend Composer serves, skipped
while the existing build matches the frontend source), then starts the container with `compose-dev.sh
start` (this call allows about 15 minutes for readiness only), waits
for Manager, Headless, Program runner and Composer to report ready, and checks
the remote source again. The container carries the labels
`manyforge.launch-instance` and `manyforge.launch-input-digest`; `status` and
`stop` act only on the recorded container ID with that image and those labels.
`stop` exports evidence as described in the workbench guide, then removes the
container.

### `standard` profile

Requires a host ROS 2 install (see [Profiles](#profiles)). Run in a shell where
that ROS environment is sourced and where `manyforge_headless_api`,
`manyforge_program_runner`, `manyforge_composer` and
`manyforge_assistant_bridge` import from `python3` (the AI assistant
component runs the bridge).

```bash
manyforge-operator --state-root "$STATE_ROOT" --instance-id workbench --json \
  start --profile standard --source-root "$MANYFORGE_ROOT" \
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
| `--composer-static-dir` | Optional built frontend with `index.html`. Default: `manyforge_composer/frontend/dist` in the source tree, if built (`npm ci && npm run build` there); without one Composer serves only its API. |
| `--simulation-control-config` | Optional simulation-control site file for the Program runner. |
| `--expected-source-fingerprint`, `--expected-head-commit`, `--expected-head-tree` | Optional; start refuses a source that differs. Also accepted by `isaac`. |
| `--poll-interval-s` (0.25), `--reconciliation-period-ms` (500), `--manager-timeout-s` (60, max 60) | Timeouts. `--readiness-timeout-s` is accepted but not used by `standard`. |

The `standard` start stays in the foreground for the application's whole
life. It prints a `started` result once every owner reports ready, then waits;
when the application ends it prints `stopped`. Run it in its own terminal or
under a service manager, and use `stop` (or Ctrl+C on the `start` terminal) to
end it.

The launcher keeps all files of an instance under `$STATE_ROOT/$INSTANCE_ID/`
(layout `manyforge.cell_instance_layout.v2`):

| Path | Content |
|---|---|
| `data/` | Saved data, kept across starts: persistent Program values (`data/program-runner/persistent-keys.v<N>.json`, named by its format version; an earlier-format file there is left in place and not read), Composer settings (`data/composer/`) and the Program workspace (`data/program-workspace/`). |
| `history/` | History (`history.v<N>.sqlite3`); see [Observability](OBSERVABILITY.md#runtime-events). |
| `operator/` | The launch record (`operator/launch_instance.json`) and the instance lock. |
| `current/` | Everything one start writes while it runs: journals, sockets, published files, motion-trace snapshots for debug exports (`current/motion-traces/`, at most 64 MiB, oldest first; start with `MANYFORGE_MOTION_TRACE_DIR=` empty to keep traces inline) and, once it has ended, how the application ended (`current/manager-run/application-end.json`, see [Observability](OBSERVABILITY.md#how-the-application-ended)). |
| `previous-runs/`, `session-reports/` | Earlier runs and their reports. |

At each start the previous `current/` moves to `previous-runs/<id>` before any
process starts, and a new, empty `current/` is created. Before that, and before
anything is written, previous runs are pruned to five runs, counting the
`current/` about to move, and to 2 GiB of archived runs and older History files
together, oldest first; the newest archived run, the run the current report
describes and the current History file are never deleted.

Disk limits: the files under the instance take at most 10 % of the disk, and
10 GB of it always stay free, unless `start --site-settings PATH` names a site
settings file whose optional `disk_max_percent`, `disk_always_free_bytes` and
`disk_machine_minimum_bytes` change them (see
[Site settings](DEVELOPMENT_WORKBENCH.md#site-settings); a missing member
keeps its default). This launcher reads nothing else from that file and does
not pass it on; `--site-settings` is refused with the `isaac` profile, and an
invalid file refuses the start with `operator_configuration_invalid`. While either limit is exceeded, the same pass
also deletes previous runs and older History files below the five-run and
2 GiB limits, oldest first, at most eight per pass, with the same
protections. Then, if the disk that holds the instance (or its `history/`)
has less than 10 GB free (`disk_always_free_bytes` plus
`disk_machine_minimum_bytes`), or no free inodes, `start` is refused with
`disk_space_below_floor` (exit status 71 in the `standard` profile) before any
process starts:

```text
disk_space_below_floor: Not started: the disk that holds <instance> has 4 GB free. ManyForge needs at least 10 GB free (10 GB plus 0 GB for the machine). Free 6 GB and start again.
```

The space the machine itself needs besides ManyForge
(`disk_machine_minimum_bytes`) is 0 by default: it is not determined yet.
While ManyForge runs,
the launcher repeats the pass every 60 seconds, only when a limit is
exceeded; the current run's files are never deleted. Each deletion is one
line on the launcher's standard error, naming the folder and the limits.

The launcher creates and checks
this layout itself; paths cannot be overridden. An instance created by an
earlier launcher (with `state/` and `run/`) is refused with
`cell_layout_invalid` and is not converted: use a new `--instance-id` or
`--state-root`. The persistent Program values in the old
`state/program-runner/persistent-keys.json` can be kept only if that file is
in the current register format: then copy it into the new instance's
`data/program-runner/` as `persistent-keys.v<N>.json`, N being the number at
the end of its `schema_version`, before its first start. A file in an older
format is not read. Output goes to journald: read it
with `journalctl -t manyforge-<instance-id>-<component>` (`manager`,
`headless`, `program_runner`, `composer`).

## Using Composer

In the live Scene, a carried-object card offers **Detach** while the
Program is idle or held. Confirm "Detach <object> from <component> / <target>
at its current pose?" to detach it at that pose. The action is disabled while the Program is
running and reports a typed Scene refusal if the detach cannot be completed.
The availability check uses the current run status; another client can start a
Run between that check and the Scene commit.

Open `http://<advertised-host>:9000/` once `start` or `status` reports
`ready`. The Composer flow is the same as with the workbench:

1. **Load Scenario** to open a Scenario, or **New** to open the empty
   Untitled Scenario. New empties the cell (Apply of the empty Untitled
   Scenario); Load is New, then Apply of the file. New always works: a
   finished or stopped Program run is Reset first, every component of the
   current cell is disabled and its processes are stopped, and the Scene is
   initialized. A loaded Scenario's components come back prepared, not
   enabled. The result loads as a Draft. Robots and physically held parts
   may remain; account for them before using the next cell.
2. Review the Draft. **Discard** drops unapplied changes; **Revert** returns
   to the saved files.
3. **Apply** makes the Draft the running configuration; affected components
   come back prepared.
4. **Execute** starts the Program (or **Activate and execute**, which prepares
   and enables every active component first). Every Program starts held:
   press **Run** on its motion groups; use **Stop**, **Resume**, **Halt** and
   **Reset** as needed. A group's **Stop** stops that group, the **All
   groups** row's **Stop** stops all motion in the cell (Stop all), and
   **Halt** also pauses the Program and blocks new manual moves until
   **Execute** or **Reset**; see
   [Stop, Stop all and Halt](DEVELOPMENT_WORKBENCH.md#stop-stop-all-and-halt).
   These are operational stops; emergency stops belong to the cell's own
   safety system.
5. **Save Scenario** or **Save Scenario as** to keep your changes. Save
   writes what is applied, so a saved Scenario always loads again: Apply
   your edits first. With edits that are not applied, Save asks once and
   then writes the applied state; the edits stay in the Draft. The
   Untitled Scenario is read-only: **Save Scenario** is disabled for it and
   **Save Scenario as** names and places it, then continues in the saved
   project. Unsaved Untitled changes are lost when Composer restarts.

Where Scenarios can be loaded and saved depends on the profile:

- `standard`: Composer can read the checkout's `examples/`, the package root and
  the instance workspace
  `$STATE_ROOT/$INSTANCE_ID/data/program-workspace`. Only
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
| `port_occupied` | Free 9000 or 9010. Ports cannot be remapped. |
| `journald_unavailable` | `standard` needs `systemd-cat` and the journald socket. |
| `application_exited` | A process of the application exited and the Manager ended the application (`start` exits with status 73). Its log names the process; read it, then start again. |
| `application_stop_incomplete` | A step of the application's stop was forced or did not complete (`start` exits with status 73). The Manager's log names the step. The next start treats this run as ended uncleanly. Check that the robot has stopped before starting again. |
| `cell_layout_invalid` | The instance's files are missing, unsafe, or use the earlier layout; see the layout above. |
| `session_selection_unavailable` | The previous run could not be archived and a fresh `current/` could not be made; nothing was started. Check the instance directory. |
| `run_end_not_recorded` | The application ended, but the launcher could not write that into the run folder (`start` exits with status 71). The next start treats this run as ended uncleanly. Check the disk that holds the instance folder. |
| `disk_space_below_floor` | The disk that holds the instance has less free space than ManyForge needs, or no free inodes; nothing was started. Free the amount the message names on that disk, then start again. |
| `isaac_readiness_timeout`, `runtime_container_unready` | Read the logs (`journalctl` or `docker logs <container-id>`). |
| `runtime_host_unavailable`, `remote_protocol_invalid` | SSH or the runtime host failed. Absence cannot be proved; restore the host, then run `status`. |
| `container_identity_mismatch` | Another container has that name. It is not adopted. |
| `incompatible_owner`, `stale_launch_record` | The instance has a different configuration or still owns resources. `stop` it, or use another `--instance-id`. |
| `status_record_invalid` | The record is unreadable, is in the format of an earlier ManyForge version (it is not read or converted), or is an `isaac` record from an older launcher that started Isaac Sim itself. Stop the old runtime with the version that started it, then use a new instance. |
| `cleanup_deadline_exceeded` | `stop` could not prove everything stopped. Investigate, then run `stop` again. |
| `instance_unavailable` | No record exists for this state root and instance ID. |

If a stop does not end, the last `application stop: waiting at <step>: …`
or `native shutdown entering/waiting at <phase>` line in the log says what it
waits for. Run `stop` again (or press Ctrl+C again on the `start` terminal):
the Manager ends the processes still running and logs which step was forced.
Never kill ManyForge processes by name.

To replace the package root, `stop` and `start` again; there is no live
package upgrade.

## Inspection

Composer serves `/api/health`, `/api/manager/status`, `/api/runtime` and
`/openapi.json`. See the [Composer REST API](../reference/REST_API.md) and the
[Headless REST API](../reference/HEADLESS_REST_API.md).
